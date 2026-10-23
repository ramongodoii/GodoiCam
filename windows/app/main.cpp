// GodoiCan para Windows: encontra o celular, recebe o vídeo e alimenta a câmera virtual.
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <memory>
#include <string>
#include <vector>

#include "Discovery.h"
#include "FrameWriter.h"
#include "PairStore.h"
#include "Session.h"
#include "System.h"
#include "resource.h"

namespace {

enum : UINT {
    WM_APP_STATUS = WM_APP + 1,
    WM_APP_PREVIEW,
    WM_APP_DEVICES,
};

enum : int {
    ID_LIST = 100,
    ID_CONNECT,
    ID_USB,
    ID_IP,
    ID_IP_GO,
    ID_CODE,
    ID_CODE_GO,
    ID_AUTO,
    ID_DISCONNECT,
    ID_CAMERA,
    ID_FORGET,
    ID_TIMER = 1,
};

struct App {
    HWND wnd = nullptr;
    HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr;
    UINT dpi = 96;

    HWND lblPhones, list, btnConnect, btnUsb, lblIp, editIp, btnIp, lblCode, editCode, btnCode, chkAuto,
        btnDisconnect, lblStatus, lblDetail, lblCamera, btnCamera, btnForget;

    PairStore pairs;
    FrameWriter writer;
    Discovery discovery;
    std::unique_ptr<Session> session;
    std::vector<PhoneInfo> phones;

    bool userStopped = false;     // o usuário desconectou: não reconectar sozinho
    bool cameraReady = false;
    std::wstring cameraMessage;

    std::vector<uint8_t> preview;
    int previewW = 0, previewH = 0;
    RECT previewRect{};
} g;

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    return h;
}

void CreateFonts() {
    if (g.font) DeleteObject(g.font);
    if (g.fontBold) DeleteObject(g.fontBold);
    if (g.fontTitle) DeleteObject(g.fontTitle);
    g.font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                         L"Segoe UI");
    g.fontBold = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             L"Segoe UI");
    g.fontTitle = CreateFontW(-S(19), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe UI");
}

void ApplyFonts() {
    for (HWND h : {g.lblPhones, g.list, g.btnConnect, g.btnUsb, g.lblIp, g.editIp, g.btnIp, g.lblCode, g.btnCode,
                   g.chkAuto, g.btnDisconnect, g.lblDetail, g.lblCamera, g.btnCamera, g.btnForget})
        SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    SendMessageW(g.lblStatus, WM_SETFONT, (WPARAM)g.fontTitle, TRUE);
    SendMessageW(g.editCode, WM_SETFONT, (WPARAM)g.fontTitle, TRUE);
}

void Layout() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int x = S(18), w = S(310), y = S(16), gap = S(8), bh = S(32);
    auto put = [](HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, w, hgt, TRUE); };

    put(g.lblPhones, x, y, w, S(20));
    y += S(24);
    put(g.list, x, y, w, S(132));
    y += S(132) + gap;
    int half = (w - gap) / 2;
    put(g.btnConnect, x, y, half, bh);
    put(g.btnUsb, x + half + gap, y, half, bh);
    y += bh + S(14);

    put(g.lblIp, x, y, w, S(20));
    y += S(22);
    put(g.editIp, x, y, w - S(110) - gap, S(28));
    put(g.btnIp, x + w - S(110), y - S(2), S(110), bh);
    y += bh + S(14);

    put(g.lblCode, x, y, w, S(20));
    y += S(22);
    put(g.editCode, x, y, w - S(110) - gap, S(34));
    put(g.btnCode, x + w - S(110), y, S(110), S(34));
    y += S(34) + S(14);

    put(g.chkAuto, x, y, w, S(22));
    y += S(22) + gap;
    put(g.btnDisconnect, x, y, w, bh);
    y += bh + S(18);

    put(g.lblStatus, x, y, w, S(52));
    y += S(54);
    put(g.lblDetail, x, y, w, S(96));

    int bottom = rc.bottom - S(16);
    put(g.btnCamera, x, bottom - bh, half, bh);
    put(g.btnForget, x + half + gap, bottom - bh, half, bh);
    put(g.lblCamera, x, bottom - bh - S(44), w, S(40));

    // Prévia 16:9 à direita.
    int px = x + w + S(24), py = S(16);
    int pw = rc.right - px - S(18), ph = rc.bottom - py - S(16);
    if (pw * 9 > ph * 16) pw = ph * 16 / 9;
    else ph = pw * 9 / 16;
    g.previewRect = {px, py, px + std::max(pw, 0), py + std::max(ph, 0)};
    InvalidateRect(g.wnd, nullptr, TRUE);
}

std::wstring PhoneLabel(const PhoneInfo& p) {
    Bytes key;
    bool paired = g.pairs.Key(p.id, key);
    return Widen(p.name) + L"   " + Widen(p.ip) + (paired ? L"   ✓ pareado" : L"");
}

void RefreshList() {
    g.phones = g.discovery.Phones();
    int sel = ListBox_GetCurSel(g.list);
    std::string selectedId = sel >= 0 && sel < (int)g.phones.size() ? g.phones[sel].id : "";
    ListBox_ResetContent(g.list);
    for (auto& p : g.phones) ListBox_AddString(g.list, PhoneLabel(p).c_str());
    if (g.phones.empty()) {
        ListBox_AddString(g.list, L"Procurando… abra o GodoiCan no celular (mesmo Wi-Fi).");
        EnableWindow(g.list, FALSE);
        return;
    }
    EnableWindow(g.list, TRUE);
    int index = 0;
    for (int i = 0; i < (int)g.phones.size(); i++)
        if (g.phones[i].id == selectedId || (selectedId.empty() && g.phones[i].id == g.pairs.LastPhone())) index = i;
    ListBox_SetCurSel(g.list, index);
}

void Start(const Target& t) {
    g.userStopped = false;
    g.session->Connect(t);
}

void ConnectSelected() {
    int sel = ListBox_GetCurSel(g.list);
    if (sel < 0 || sel >= (int)g.phones.size()) return;
    const PhoneInfo& p = g.phones[sel];
    Target t;
    t.host = p.ip;
    t.port = p.port;
    t.phoneId = p.id;
    t.name = p.name;
    Start(t);
}

void ConnectUsb() {
    Target t;
    t.usb = true;
    t.name = "celular (USB)";
    Start(t);
}

void ConnectIp() {
    wchar_t buf[128];
    GetWindowTextW(g.editIp, buf, 128);
    std::string host = Narrow(buf);
    while (!host.empty() && host.back() == ' ') host.pop_back();
    while (!host.empty() && host.front() == ' ') host.erase(0, 1);
    if (host.empty()) return;
    Target t;
    t.host = host;
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        t.host = host.substr(0, colon);
        t.port = (uint16_t)atoi(host.c_str() + colon + 1);
    }
    t.name = t.host;
    Start(t);
}

void SubmitCode() {
    wchar_t buf[32];
    GetWindowTextW(g.editCode, buf, 32);
    std::string digits;
    for (wchar_t* p = buf; *p; p++)
        if (*p >= L'0' && *p <= L'9') digits += (char)*p;
    if (digits.size() != 6) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g.session->SubmitCode(digits);
    SetWindowTextW(g.editCode, L"");
}

void UpdateCameraLabel() {
    std::wstring text;
    if (!g.cameraReady) {
        text = g.cameraMessage.empty() ? L"Câmera virtual não instalada." : g.cameraMessage;
        SetWindowTextW(g.btnCamera, L"Instalar câmera");
    } else {
        text = L"Câmera virtual \"GodoiCan\" instalada. Escolha-a no Zoom, Teams, Meet ou OBS.";
        SetWindowTextW(g.btnCamera, L"Reinstalar câmera");
    }
    SetWindowTextW(g.lblCamera, text.c_str());
}

void SetupCamera(bool force) {
    std::wstring error;
    if (!sys::VCamDllRegistered()) {
        if (!force) {
            g.cameraReady = false;
            g.cameraMessage = L"Câmera virtual não instalada. Clique em \"Instalar câmera\" (pede administrador).";
            UpdateCameraLabel();
            return;
        }
        if (!sys::RegisterVCamDllElevated(error)) {
            g.cameraReady = false;
            g.cameraMessage = error;
            UpdateCameraLabel();
            return;
        }
    }
    if (force) sys::RemoveVirtualCamera(error);
    g.cameraReady = sys::EnsureVirtualCamera(error);
    g.cameraMessage = error;
    UpdateCameraLabel();
}

void UpdateStatus() {
    Session::Status s = g.session->GetStatus();
    std::wstring title = s.title.empty() ? L"Desconectado" : s.title;
    SetWindowTextW(g.lblStatus, title.c_str());
    SetWindowTextW(g.lblDetail, s.detail.c_str());
    bool needCode = s.state == Session::State::NeedCode;
    EnableWindow(g.editCode, needCode);
    EnableWindow(g.btnCode, needCode);
    EnableWindow(g.btnDisconnect, g.session->Active());
    if (needCode) {
        SetFocus(g.editCode);
        FLASHWINFO fi{sizeof(fi), g.wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&fi);
    }
    if (s.state != Session::State::Streaming && s.state != Session::State::Connected) {
        g.preview.clear();
        InvalidateRect(g.wnd, &g.previewRect, FALSE);
    }
}

// Reconecta sozinho ao último celular usado (ou ao único pareado) quando ele aparece na rede.
void AutoConnect() {
    if (g.userStopped || g.session->Active() || !g.pairs.AutoConnect()) return;
    std::string last = g.pairs.LastPhone();
    auto paired = g.pairs.All();
    for (auto& p : g.phones) {
        bool isLast = !last.empty() && p.id == last;
        bool onlyPaired = last.empty() && paired.size() == 1 && paired.count(p.id);
        if (isLast || onlyPaired) {
            Target t;
            t.host = p.ip;
            t.port = p.port;
            t.phoneId = p.id;
            t.name = p.name;
            g.session->Connect(t);
            return;
        }
    }
}

void PaintPreview(HDC dc) {
    RECT r = g.previewRect;
    HBRUSH bg = CreateSolidBrush(RGB(22, 24, 29));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    if (!g.preview.empty() && g.previewW > 0) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = g.previewW;
        bmi.bmiHeader.biHeight = -g.previewH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        int rw = r.right - r.left, rh = r.bottom - r.top;
        int dw = rw, dh = rw * g.previewH / g.previewW;
        if (dh > rh) {
            dh = rh;
            dw = rh * g.previewW / g.previewH;
        }
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, r.left + (rw - dw) / 2, r.top + (rh - dh) / 2, dw, dh, 0, 0, g.previewW, g.previewH,
                      g.preview.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        return;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(169, 174, 184));
    HGDIOBJ old = SelectObject(dc, g.font);
    DrawTextW(dc, L"A imagem do celular aparece aqui", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.wnd = hwnd;
            g.dpi = GetDpiForWindow(hwnd);
            CreateFonts();
            g.lblPhones = Make(L"STATIC", L"Celulares na rede", SS_LEFT, 0);
            g.list = Make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, ID_LIST);
            g.btnConnect = Make(L"BUTTON", L"Conectar (Wi-Fi)", BS_DEFPUSHBUTTON, ID_CONNECT);
            g.btnUsb = Make(L"BUTTON", L"Conectar por USB", BS_PUSHBUTTON, ID_USB);
            g.lblIp = Make(L"STATIC", L"Ou digite o IP mostrado no celular:", SS_LEFT, 0);
            g.editIp = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_IP);
            g.btnIp = Make(L"BUTTON", L"Conectar IP", BS_PUSHBUTTON, ID_IP_GO);
            g.lblCode = Make(L"STATIC", L"Código de pareamento (aparece no celular):", SS_LEFT, 0);
            g.editCode = Make(L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_CENTER, ID_CODE);
            SendMessageW(g.editCode, EM_SETLIMITTEXT, 6, 0);
            g.btnCode = Make(L"BUTTON", L"Parear", BS_PUSHBUTTON, ID_CODE_GO);
            g.chkAuto = Make(L"BUTTON", L"Reconectar sozinho ao último celular", BS_AUTOCHECKBOX, ID_AUTO);
            g.btnDisconnect = Make(L"BUTTON", L"Desconectar", BS_PUSHBUTTON, ID_DISCONNECT);
            g.lblStatus = Make(L"STATIC", L"Desconectado", SS_LEFT, 0);
            g.lblDetail = Make(L"STATIC", L"", SS_LEFT, 0);
            g.lblCamera = Make(L"STATIC", L"", SS_LEFT, 0);
            g.btnCamera = Make(L"BUTTON", L"Instalar câmera", BS_PUSHBUTTON, ID_CAMERA);
            g.btnForget = Make(L"BUTTON", L"Esquecer celulares", BS_PUSHBUTTON, ID_FORGET);
            ApplyFonts();
            SendMessageW(g.editIp, EM_SETCUEBANNER, TRUE, (LPARAM)L"ex.: 192.168.0.15");
            Button_SetCheck(g.chkAuto, g.pairs.AutoConnect() ? BST_CHECKED : BST_UNCHECKED);

            g.session = std::make_unique<Session>(
                g.pairs, g.writer, [] { PostMessageW(g.wnd, WM_APP_STATUS, 0, 0); },
                [] { PostMessageW(g.wnd, WM_APP_PREVIEW, 0, 0); });
            g.discovery.Start([] { PostMessageW(g.wnd, WM_APP_DEVICES, 0, 0); });
            RefreshList();
            UpdateStatus();
            SetupCamera(false);
            SetTimer(hwnd, ID_TIMER, 250, nullptr);
            Layout();
            return 0;
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {S(860), S(760)};
            return 0;
        }
        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            CreateFonts();
            ApplyFonts();
            auto* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            if ((HWND)lp == g.lblStatus) SetTextColor(dc, RGB(20, 22, 26));
            else if ((HWND)lp == g.lblDetail || (HWND)lp == g.lblCamera) SetTextColor(dc, RGB(90, 96, 106));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_CONNECT:
                    ConnectSelected();
                    break;
                case ID_LIST:
                    if (HIWORD(wp) == LBN_DBLCLK) ConnectSelected();
                    break;
                case ID_USB:
                    ConnectUsb();
                    break;
                case ID_IP_GO:
                    ConnectIp();
                    break;
                case ID_CODE_GO:
                    SubmitCode();
                    break;
                case ID_AUTO:
                    g.pairs.SetAutoConnect(Button_GetCheck(g.chkAuto) == BST_CHECKED);
                    break;
                case ID_DISCONNECT:
                    g.userStopped = true;
                    g.session->Disconnect();
                    break;
                case ID_CAMERA:
                    SetupCamera(true);
                    break;
                case ID_FORGET:
                    if (MessageBoxW(hwnd, L"Esquecer todos os celulares pareados? Será preciso parear de novo.",
                                    L"GodoiCan", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        g.userStopped = true;
                        g.session->Disconnect();
                        g.pairs.Clear();
                        RefreshList();
                    }
                    break;
            }
            return 0;
        case WM_APP_STATUS:
            UpdateStatus();
            return 0;
        case WM_APP_PREVIEW:
            if (g.session->TakePreview(g.preview, g.previewW, g.previewH))
                InvalidateRect(hwnd, &g.previewRect, FALSE);
            return 0;
        case WM_APP_DEVICES:
            RefreshList();
            AutoConnect();
            return 0;
        case WM_TIMER:
            g.writer.Heartbeat(g.session->Streaming());
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HRGN all = CreateRectRgnIndirect(&rc), prev = CreateRectRgnIndirect(&g.previewRect);
            CombineRgn(all, all, prev, RGN_DIFF);
            FillRgn((HDC)wp, all, GetSysColorBrush(COLOR_WINDOW));
            DeleteObject(all);
            DeleteObject(prev);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintPreview(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_RETURN && GetFocus() == g.editCode) SubmitCode();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER);
            g.session.reset();
            g.discovery.Stop();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring startIp;
    bool startUsb = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--remove-camera") {
            // Usado pelo desinstalar.cmd.
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MFStartup(MF_VERSION, MFSTARTUP_LITE);
            std::wstring error;
            bool ok = sys::RemoveVirtualCamera(error);
            MFShutdown();
            CoUninitialize();
            return ok ? 0 : 1;
        }
        if (a == L"--usb") startUsb = true;
        if (a == L"--ip" && i + 1 < argc) startIp = argv[++i];
    }
    LocalFree(argv);

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\GodoiCanApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"GodoiCanWindow", nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g.pairs.Load();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GodoiCanWindow";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"GodoiCan", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(1120, dpi, 96), MulDiv(780, dpi, 96), nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);
    if (startUsb) {
        ConnectUsb();
    } else if (!startIp.empty()) {
        SetWindowTextW(g.editIp, startIp.c_str());
        ConnectIp();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Enter no campo do código confirma; Tab navega entre os controles.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            if (msg.hwnd == g.editCode) {
                SubmitCode();
                continue;
            }
            if (msg.hwnd == g.editIp) {
                ConnectIp();
                continue;
            }
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
