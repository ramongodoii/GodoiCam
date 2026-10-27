#include "System.h"

#include <windows.h>
#include <mfapi.h>
#include <setupapi.h>
#include <mfvirtualcamera.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include "GodoiCamShared.h"
#include "Net.h"

#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "setupapi.lib")

using Microsoft::WRL::ComPtr;

namespace sys {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L'\\'));
}

std::string ComputerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &n)) return "PC";
    return Narrow(name);
}

bool VCamDllRegistered() {
    HKEY key;
    std::wstring path = L"SOFTWARE\\Classes\\CLSID\\" GDC_VCAM_CLSID_STR L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t value[MAX_PATH] = {};
    DWORD size = sizeof(value);
    bool ok = RegQueryValueExW(key, nullptr, nullptr, nullptr, (BYTE*)value, &size) == ERROR_SUCCESS &&
              GetFileAttributesW(value) != INVALID_FILE_ATTRIBUTES;
    RegCloseKey(key);
    return ok;
}

bool RegisterVCamDllElevated(std::wstring& error) {
    std::wstring dll = ExeDir() + L"\\GodoiCamVCam.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"GodoiCamVCam.dll não está ao lado do GodoiCam.exe.";
        return false;
    }
    std::wstring args = L"/s \"" + dll + L"\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"regsvr32.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        error = L"O registro da câmera virtual foi cancelado.";
        return false;
    }
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0 || !VCamDllRegistered()) {
        error = L"O regsvr32 não conseguiu registrar a câmera virtual.";
        return false;
    }
    return true;
}

static HRESULT CreateVCam(ComPtr<IMFVirtualCamera>& vcam) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                 MFVirtualCameraAccess_CurrentUser, GDC_VCAM_NAME, GDC_VCAM_CLSID_STR, nullptr, 0,
                                 &vcam);
}

static std::wstring HrText(HRESULT hr) {
    wchar_t buf[32];
    swprintf_s(buf, L"0x%08X", (unsigned)hr);
    return buf;
}

bool EnsureVirtualCamera(std::wstring& error) {
    BOOL supported = FALSE;
    if (FAILED(MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported)) || !supported) {
        error = L"Este Windows não suporta câmera virtual (precisa do Windows 11).";
        return false;
    }
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = CreateVCam(vcam);
    if (SUCCEEDED(hr)) hr = vcam->Start(nullptr);
    if (FAILED(hr)) {
        error = L"Falha ao criar a câmera virtual (" + HrText(hr) +
                (hr == E_ACCESSDENIED ? L"). Verifique Privacidade > Câmera nas Configurações." : L").");
        return false;
    }
    // Vida "System": a câmera continua registrada depois que este objeto é liberado.
    vcam->Shutdown();
    return true;
}

bool RemoveVirtualCamera(std::wstring& error) {
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = CreateVCam(vcam);
    if (SUCCEEDED(hr)) hr = vcam->Remove();
    if (FAILED(hr)) {
        error = L"Falha ao remover a câmera virtual (" + HrText(hr) + L").";
        return false;
    }
    return true;
}

std::wstring FindAdb() {
    std::wstring candidates[3];
    candidates[0] = ExeDir() + L"\\adb\\adb.exe";
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) candidates[1] = found;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        candidates[2] = std::wstring(local) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
    CoTaskMemFree(local);
    for (auto& c : candidates)
        if (!c.empty() && GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    return {};
}

// Executa um programa sem janela e captura stdout+stderr.
static bool RunCapture(const std::wstring& exe, const std::wstring& args, std::string& output, DWORD& code,
                       DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                  &si, &pi);
    CloseHandle(writePipe);
    if (!started) {
        CloseHandle(readPipe);
        return false;
    }
    char buf[512];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) output.append(buf, n);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, timeoutMs);
    code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool AdbForward(std::wstring& error) {
    std::wstring adb = FindAdb();
    if (adb.empty()) {
        error = L"adb.exe não encontrado. Coloque a pasta platform-tools em \"adb\" ao lado do GodoiCam.exe.";
        return false;
    }
    std::string output;
    DWORD code = 1;
    if (!RunCapture(adb, L"forward tcp:47180 tcp:47180", output, code, 15000)) {
        error = L"Não foi possível executar o adb.";
        return false;
    }
    if (code == 0) return true;
    if (output.find("no devices") != std::string::npos)
        error = L"Nenhum celular no USB. Conecte o cabo e ative a Depuração USB.";
    else if (output.find("unauthorized") != std::string::npos)
        error = L"Autorize este PC na janela de depuração USB que apareceu no celular.";
    else if (output.find("more than one") != std::string::npos)
        error = L"Há mais de um aparelho no adb. Deixe só o celular conectado.";
    else
        error = L"adb: " + Widen(output);
    return false;
}

// Há uma linha "serial<TAB>estado" com este estado no `adb devices`?
static bool AdbHasState(const std::string& output, const char* state) {
    std::string tag = std::string("\t") + state;
    size_t pos = 0;
    while ((pos = output.find(tag, pos)) != std::string::npos) {
        size_t end = pos + tag.size();
        if (end >= output.size() || output[end] == '\r' || output[end] == '\n') return true;
        pos = end;
    }
    return false;
}

static bool AdbDevices(std::string& output) {
    static std::wstring adb = FindAdb();
    if (adb.empty()) return false;
    DWORD code = 1;
    return RunCapture(adb, L"devices", output, code, 5000) && code == 0;
}

bool AdbDevicePresent() {
    std::string output;
    return AdbDevices(output) && AdbHasState(output, "device");
}

// Primeiro aparelho portátil (MTP: celular, câmera) ligado por USB; vazio se nenhum.
static bool UsbPortableDevice(std::wstring& name) {
    // GUID_DEVCLASS_WPD (devguid.h), sem initguid.h para não redefinir os GUIDs do Media Foundation.
    static const GUID kWpdClass = {0xeec5ad98, 0x8080, 0x425f, {0x92, 0x2a, 0xda, 0xbf, 0x3d, 0xe3, 0xf6, 0x9a}};
    HDEVINFO set = SetupDiGetClassDevsW(&kWpdClass, L"USB", nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA info{sizeof(info)};
    bool found = SetupDiEnumDeviceInfo(set, 0, &info) != FALSE;
    if (found) {
        wchar_t buf[256] = {};
        if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)buf, sizeof(buf) - 2,
                                              nullptr))
            name = buf;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

UsbPhone UsbPhoneState(std::wstring& name) {
    name.clear();
    std::string output;
    bool adb = AdbDevices(output);
    if (adb && AdbHasState(output, "device")) return UsbPhone::Ready;
    if (adb && (AdbHasState(output, "unauthorized") || AdbHasState(output, "offline"))) {
        UsbPortableDevice(name);
        return UsbPhone::Unauthorized;
    }
    if (!UsbPortableDevice(name)) return UsbPhone::None;
    return FindAdb().empty() ? UsbPhone::NoAdb : UsbPhone::NoDebugging;
}

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool StartWithWindows() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(key, L"GodoiCam", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return on;
}

void SetStartWithWindows(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(path) + L"\" --minimized";
        RegSetValueExW(key, L"GodoiCam", 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"GodoiCam");
    }
    RegCloseKey(key);
}

}  // namespace sys
