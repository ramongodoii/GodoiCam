#include <string>

#include "VCam.h"

using Microsoft::WRL::InProc;
using Microsoft::WRL::Module;

CoCreatableClass(Activator);

static HMODULE g_module = nullptr;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) {
    return Module<InProc>::GetModule().GetClassObject(clsid, riid, object);
}

STDAPI DllCanUnloadNow() { return Module<InProc>::GetModule().Terminate() ? S_OK : S_FALSE; }

namespace {

const wchar_t kKey[] = L"Software\\Classes\\CLSID\\" GDC_VCAM_CLSID_STR;

LSTATUS SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

// Registro em HKLM (precisa de administrador): o Frame Server roda como serviço e
// não enxerga o HKCU do usuário.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    HKEY clsid = nullptr, inproc = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &clsid, nullptr);
    if (st == ERROR_SUCCESS) st = SetString(clsid, nullptr, L"GodoiCam Virtual Camera");
    if (st == ERROR_SUCCESS)
        st = RegCreateKeyExW(clsid, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
    if (st == ERROR_SUCCESS) st = SetString(inproc, nullptr, path);
    if (st == ERROR_SUCCESS) st = SetString(inproc, L"ThreadingModel", L"Both");
    if (inproc) RegCloseKey(inproc);
    if (clsid) RegCloseKey(clsid);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer() {
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kKey);
    return st == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(st);
}
