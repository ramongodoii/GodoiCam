// GodoiCan para Windows. Janela no estilo "só o vídeo": uma barra fina com o seletor de
// celular e o menu ☰; o resto é a imagem. Conecta sozinho ao celular pareado (Wi‑Fi ou USB)
// e continua na bandeja quando a janela é fechada.
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Discovery.h"
#include "FrameWriter.h"
#include "Net.h"
#include "PairStore.h"
#include "Session.h"
#include "System.h"
#include "resource.h"

namespace {

enum : UINT {
    WM_APP_STATUS = WM_APP + 1,
    WM_APP_PREVIEW,
    WM_APP_DEVICES,
    WM_APP_TRAY,
};

enum : int {
    ID_DEVICE = 100,
    ID_MENU,
    ID_PANEL_TEXT,
    ID_PANEL_EDIT,
    ID_PANEL_OK,
    ID_PANEL_CANCEL,

    IDM_IP = 200,
    IDM_USB,
    IDM_DISCONNECT,
    IDM_AUTO,
    IDM_BLANK,
    IDM_STARTUP,
    IDM_STATS,
    IDM_CAMERA,
    IDM_FORGET,
    IDM_FOLDER,
    IDM_OPEN,
    IDM_EXIT,

    TIMER_TICK = 1,
};

enum class Panel { None, Code, Ip };

struct Choice {
    enum Kind { Phone, Usb, Ip, Current } kind;
    PhoneInfo phone;
};

const COLORREF kBar = RGB(32, 34, 40);
const COLORREF kVideo = RGB(16, 17, 20);
const COLORREF kText = RGB(236, 238, 242);
const COLORREF kMuted = RGB(150, 156, 168);
const COLORREF kOk = RGB(61, 220, 132);
const COLORREF kWait = RGB(255, 194, 71);
const COLORREF kBad = RGB(255, 107, 107);

struct App {
    HWND wnd = nullptr;
    HWND combo = nullptr, btnMenu = nullptr, panelText = nullptr, panelEdit = nullptr, panelOk = nullptr,
         panelCancel = nullptr;
    HFONT font = nullptr, fontSmall = nullptr, fontBig = nullptr, fontTitle = nullptr, fontIcon = nullptr;
    HBRUSH barBrush = nullptr, videoBrush = nullptr;
    UINT dpi = 96;

    PairStore pairs;
    FrameWriter writer;
    Discovery discovery;
    std::unique_ptr<Session> session;
    std::vector<PhoneInfo> phones;
    std::vector<Choice> choices;
    bool suppressCombo = false;

    bool userStopped = false;  // o usuário desconectou: nada de reconectar sozinho
    bool cameraReady = false;
    std::wstring cameraMessage;
    bool showStats = false;
    bool blankIdle = false;
    Panel panel = Panel::None;

    std::set<std::string> autoPairTried;  // celulares não pareados já oferecidos nesta execução
    std::atomic<bool> usbPresent{false};
    std::atomic<bool> quitting{false};
    std::thread usbThread;
    ULONGLONG nextAuto = 0;
    ULONGLONG lastUsbFail = 0;
    bool autoUsbRunning = false;

    std::vector<uint8_t> preview;
    int previewW = 0, previewH = 0;
    RECT barRect{}, videoRect{};

    bool trayAdded = false;
    bool trayHintShown = false;
} g;

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

// ------------------------------------------------------------------------------------ preferências

DWORD GetPref(const wchar_t* name, DWORD def) {
    DWORD value = def, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\GodoiCan", name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}

void SetPref(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\GodoiCan", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(key);
    }
}

// ------------------------------------------------------------------------------------ fontes e layout

void CreateFonts() {
    for (HFONT* f : {&g.font, &g.fontSmall, &g.fontBig, &g.fontTitle, &g.fontIcon})
        if (*f) DeleteObject(*f);
    auto make = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    g.font = make(14, FW_NORMAL, L"Segoe UI");
    g.fontSmall = make(12, FW_NORMAL, L"Segoe UI");
    g.fontBig = make(26, FW_SEMIBOLD, L"Segoe UI");
    g.fontTitle = make(20, FW_SEMIBOLD, L"Segoe UI");
    g.fontIcon = make(18, FW_NORMAL, L"Segoe UI Symbol");
}

void ApplyFonts() {
    for (HWND h : {g.combo, g.panelText, g.panelOk, g.panelCancel}) SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    SendMessageW(g.btnMenu, WM_SETFONT, (WPARAM)g.fontIcon, TRUE);
    SendMessageW(g.panelEdit, WM_SETFONT, (WPARAM)g.fontBig, TRUE);
}

void Layout() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int barH = S(44);
    g.barRect = {0, 0, rc.right, barH};
    g.videoRect = {0, barH, rc.right, rc.bottom};

    MoveWindow(g.btnMenu, S(8), S(6), S(36), S(32), TRUE);
    // A altura do combo inclui a lista aberta.
    MoveWindow(g.combo, S(52), S(8), std::min(S(340), std::max(S(160), (int)rc.right - S(320))), S(300), TRUE);

    int cx = (g.videoRect.left + g.videoRect.right) / 2, cy = (g.videoRect.top + g.videoRect.bottom) / 2;
    int pw = S(440);
    MoveWindow(g.panelText, cx - pw / 2, cy - S(96), pw, S(56), TRUE);
    MoveWindow(g.panelEdit, cx - S(110), cy - S(30), S(220), S(48), TRUE);
    MoveWindow(g.panelOk, cx - S(116), cy + S(32), S(110), S(34), TRUE);
    MoveWindow(g.panelCancel, cx + S(6), cy + S(32), S(110), S(34), TRUE);

    bool visible = IsWindowVisible(g.wnd) && !IsIconic(g.wnd);
    g.session->SetPreviewSize(visible ? g.videoRect.right - g.videoRect.left : 0,
                              visible ? g.videoRect.bottom - g.videoRect.top : 0);
    InvalidateRect(g.wnd, nullptr, FALSE);
}

// ------------------------------------------------------------------------------------ painel central

void ShowPanel(Panel p) {
    g.panel = p;
    bool on = p != Panel::None;
    for (HWND h : {g.panelText, g.panelEdit, g.panelOk, g.panelCancel}) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    if (!on) {
        InvalidateRect(g.wnd, nullptr, FALSE);
        return;
    }
    SetWindowTextW(g.panelEdit, L"");
    LONG_PTR style = GetWindowLongPtrW(g.panelEdit, GWL_STYLE);
    if (p == Panel::Code) {
        SetWindowLongPtrW(g.panelEdit, GWL_STYLE, style | ES_NUMBER);
        SendMessageW(g.panelEdit, EM_SETLIMITTEXT, 6, 0);
        SendMessageW(g.panelEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"000000");
        Session::Status s = g.session->GetStatus();
        std::wstring text = L"Digite o código de 6 dígitos que apareceu no celular";
        if (!s.phoneName.empty()) text += L"\n" + s.phoneName;
        SetWindowTextW(g.panelText, text.c_str());
        SetWindowTextW(g.panelOk, L"Parear");
    } else {
        SetWindowLongPtrW(g.panelEdit, GWL_STYLE, style & ~ES_NUMBER);
        SendMessageW(g.panelEdit, EM_SETLIMITTEXT, 40, 0);
        SendMessageW(g.panelEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"192.168.0.15");
        SetWindowTextW(g.panelText, L"IP do celular\n(aparece no topo do app GodoiCan no celular)");
        SetWindowTextW(g.panelOk, L"Conectar");
    }
    SetFocus(g.panelEdit);
    InvalidateRect(g.wnd, nullptr, FALSE);
}

void Start(Target t) {
    g.userStopped = false;
    g.autoUsbRunning = t.automatic && t.usb;
    g.preview.clear();
    g.session->Connect(t);
}

void PanelOk() {
    wchar_t buf[64];
    GetWindowTextW(g.panelEdit, buf, 64);
    std::string text = Narrow(buf);
    if (g.panel == Panel::Code) {
        std::string digits;
        for (char c : text)
            if (c >= '0' && c <= '9') digits += c;
        if (digits.size() != 6) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        g.session->SubmitCode(digits);
        ShowPanel(Panel::None);
    } else if (g.panel == Panel::Ip) {
        while (!text.empty() && text.back() == ' ') text.pop_back();
        while (!text.empty() && text.front() == ' ') text.erase(0, 1);
        if (text.empty()) return;
        Target t;
        t.host = text;
        size_t colon = text.find(':');
        if (colon != std::string::npos) {
            t.host = text.substr(0, colon);
            t.port = (uint16_t)atoi(text.c_str() + colon + 1);
        }
        t.name = t.host;
        t.persistent = true;
        ShowPanel(Panel::None);
        Start(t);
    }
}

void PanelCancel() {
    if (g.panel == Panel::Code) {
        g.userStopped = true;
        g.session->Disconnect();
    }
    ShowPanel(Panel::None);
}

// ------------------------------------------------------------------------------------ seletor de celular

void RefreshChoices() {
    g.phones = g.discovery.Phones();
    Target cur = g.session->CurrentTarget();
    bool active = g.session->Active();

    g.choices.clear();
    g.suppressCombo = true;
    ComboBox_ResetContent(g.combo);
    int selected = -1;
    for (auto& p : g.phones) {
        Bytes key;
        std::wstring label = Widen(p.name) + L"  [Wi-Fi]" + (g.pairs.Key(p.id, key) ? L"" : L"  • não pareado");
        if (active && !cur.usb && cur.phoneId == p.id) selected = (int)g.choices.size();
        g.choices.push_back({Choice::Phone, p});
        ComboBox_AddString(g.combo, label.c_str());
    }
    bool manualIp = active && !cur.usb && cur.phoneId.empty() && !cur.host.empty();
    if (manualIp) {
        selected = (int)g.choices.size();
        g.choices.push_back({Choice::Current, {}});
        ComboBox_AddString(g.combo, (Widen(cur.host) + L"  [IP]").c_str());
    }
    std::wstring usbLabel = active && cur.usb && !cur.name.empty() && cur.name != "celular (USB)"
                                ? Widen(cur.name) + L"  [USB]"
                                : std::wstring(L"Celular pelo cabo USB");
    if (active && cur.usb) selected = (int)g.choices.size();
    g.choices.push_back({Choice::Usb, {}});
    ComboBox_AddString(g.combo, usbLabel.c_str());
    g.choices.push_back({Choice::Ip, {}});
    ComboBox_AddString(g.combo, L"Conectar pelo IP…");
    ComboBox_SetCurSel(g.combo, selected);
    g.suppressCombo = false;
}

void OnChoice() {
    int i = ComboBox_GetCurSel(g.combo);
    if (i < 0 || i >= (int)g.choices.size()) return;
    const Choice& c = g.choices[i];
    Target cur = g.session->CurrentTarget();
    bool active = g.session->Active();
    if (c.kind == Choice::Phone) {
        if (active && !cur.usb && cur.phoneId == c.phone.id) return;
        Target t;
        t.host = c.phone.ip;
        t.port = c.phone.port;
        t.phoneId = c.phone.id;
        t.name = c.phone.name;
        Start(t);
    } else if (c.kind == Choice::Usb) {
        if (active && cur.usb) return;
        Target t;
        t.usb = true;
        t.name = "celular (USB)";
        t.persistent = true;
        Start(t);
    } else if (c.kind == Choice::Ip) {
        RefreshChoices();
        ShowPanel(Panel::Ip);
    }
}

// ------------------------------------------------------------------------------------ conexão automática

// Chamado a cada segundo: se nada está conectado, escolhe sozinho USB > celular pareado >
// único celular não pareado (abre o código uma vez por execução).
void AutoTick() {
    if (g.session->Active() || g.userStopped || !g.pairs.AutoConnect() || g.panel != Panel::None) return;
    ULONGLONG now = GetTickCount64();
    if (now < g.nextAuto) return;
    g.nextAuto = now + 2000;

    if (g.usbPresent && now - g.lastUsbFail > 3000) {
        Target t;
        t.usb = true;
        t.name = "celular (USB)";
        t.automatic = true;
        Start(t);
        return;
    }
    std::string last = g.pairs.LastPhone();
    const PhoneInfo* pick = nullptr;
    for (auto& p : g.phones) {
        Bytes key;
        if (!g.pairs.Key(p.id, key)) continue;
        if (!pick || p.id == last) pick = &p;
    }
    if (!pick && g.phones.size() == 1 && !g.autoPairTried.count(g.phones[0].id)) {
        pick = &g.phones[0];
        g.autoPairTried.insert(pick->id);
    }
    if (!pick) return;
    Target t;
    t.host = pick->ip;
    t.port = pick->port;
    t.phoneId = pick->id;
    t.name = pick->name;
    t.automatic = true;
    Start(t);
}

void StartUsbWatcher() {
    g.usbThread = std::thread([] {
        while (!g.quitting) {
            if (!g.session->Active()) g.usbPresent = sys::AdbDevicePresent();
            for (int i = 0; i < 30 && !g.quitting; i++) Sleep(100);
        }
    });
}

// ------------------------------------------------------------------------------------ câmera virtual

void SetupCamera(bool force) {
    std::wstring error;
    if (!sys::VCamDllRegistered()) {
        if (!force) {
            g.cameraReady = false;
            g.cameraMessage = L"Câmera virtual não instalada — menu ☰ → Instalar câmera virtual";
            return;
        }
        if (!sys::RegisterVCamDllElevated(error)) {
            g.cameraReady = false;
            g.cameraMessage = error;
            return;
        }
    }
    if (force) sys::RemoveVirtualCamera(error);
    g.cameraReady = sys::EnsureVirtualCamera(error);
    g.cameraMessage = error;
}

// ------------------------------------------------------------------------------------ bandeja

void UpdateTray() {
    if (!g.trayAdded) return;
    Session::Status s = g.session->GetStatus();
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g.wnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    std::wstring tip = L"GodoiCan — " + (s.title.empty() ? std::wstring(L"desconectado") : s.title);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AddTray() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g.wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    wcscpy_s(nid.szTip, L"GodoiCan");
    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTray() {
    if (!g.trayAdded) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g.wnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g.trayAdded = false;
}

void ShowFromTray() {
    ShowWindow(g.wnd, IsIconic(g.wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.wnd);
    Layout();
}

void HideToTray() {
    ShowWindow(g.wnd, SW_HIDE);
    Layout();  // zera o tamanho da prévia: nada de converter imagem sem janela
    if (g.trayAdded && !g.trayHintShown) {
        g.trayHintShown = true;
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = g.wnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"O GodoiCan continua ativo");
        wcscpy_s(nid.szInfo, L"A câmera segue funcionando. Para sair, clique com o botão direito no ícone.");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

// ------------------------------------------------------------------------------------ menu ☰

void ShowMainMenu(POINT pt, bool tray) {
    HMENU m = CreatePopupMenu();
    bool active = g.session->Active();
    if (tray) {
        AppendMenuW(m, MF_STRING, IDM_OPEN, L"Abrir o GodoiCan");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, IDM_IP, L"Conectar pelo IP…");
    AppendMenuW(m, MF_STRING, IDM_USB, L"Conectar pelo cabo USB");
    AppendMenuW(m, MF_STRING | (active ? 0 : MF_GRAYED), IDM_DISCONNECT, L"Desconectar");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g.pairs.AutoConnect() ? MF_CHECKED : 0), IDM_AUTO, L"Conectar automaticamente");
    AppendMenuW(m, MF_STRING | (g.blankIdle ? MF_CHECKED : 0), IDM_BLANK, L"Vídeo preto quando desconectado");
    AppendMenuW(m, MF_STRING | (sys::StartWithWindows() ? MF_CHECKED : 0), IDM_STARTUP, L"Iniciar com o Windows");
    AppendMenuW(m, MF_STRING | (g.showStats ? MF_CHECKED : 0), IDM_STATS, L"Mostrar estatísticas");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_CAMERA, g.cameraReady ? L"Reinstalar câmera virtual" : L"Instalar câmera virtual");
    AppendMenuW(m, MF_STRING, IDM_FORGET, L"Esquecer celulares pareados");
    AppendMenuW(m, MF_STRING, IDM_FOLDER, L"Abrir pasta de diagnóstico");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Sair");
    SetForegroundWindow(g.wnd);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g.wnd, nullptr);
    DestroyMenu(m);
}

void OnMenu(int id) {
    switch (id) {
        case IDM_OPEN:
            ShowFromTray();
            break;
        case IDM_IP:
            ShowFromTray();
            ShowPanel(Panel::Ip);
            break;
        case IDM_USB: {
            Target t;
            t.usb = true;
            t.name = "celular (USB)";
            t.persistent = true;
            Start(t);
            break;
        }
        case IDM_DISCONNECT:
            g.userStopped = true;
            g.session->Disconnect();
            g.preview.clear();
            break;
        case IDM_AUTO:
            g.pairs.SetAutoConnect(!g.pairs.AutoConnect());
            break;
        case IDM_BLANK:
            g.blankIdle = !g.blankIdle;
            SetPref(L"BlankIdle", g.blankIdle);
            break;
        case IDM_STARTUP:
            sys::SetStartWithWindows(!sys::StartWithWindows());
            break;
        case IDM_STATS:
            g.showStats = !g.showStats;
            SetPref(L"ShowStats", g.showStats);
            break;
        case IDM_CAMERA:
            SetupCamera(true);
            if (!g.cameraReady) MessageBoxW(g.wnd, g.cameraMessage.c_str(), L"GodoiCan", MB_ICONWARNING);
            break;
        case IDM_FORGET:
            if (MessageBoxW(g.wnd, L"Esquecer todos os celulares pareados? Será preciso digitar o código de novo.",
                            L"GodoiCan", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g.userStopped = true;
                g.session->Disconnect();
                g.pairs.Clear();
                g.autoPairTried.clear();
            }
            break;
        case IDM_FOLDER: {
            std::wstring dir = AppDataFile(L"");
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDM_EXIT:
            DestroyWindow(g.wnd);
            return;
    }
    RefreshChoices();
    InvalidateRect(g.wnd, nullptr, FALSE);
}

// ------------------------------------------------------------------------------------ estado

void OnStatus() {
    Session::Status s = g.session->GetStatus();
    if (s.state == Session::State::NeedCode && g.panel != Panel::Code) {
        if (!IsWindowVisible(g.wnd)) ShowFromTray();
        ShowPanel(Panel::Code);
        FLASHWINFO fi{sizeof(fi), g.wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&fi);
    } else if (s.state != Session::State::NeedCode && g.panel == Panel::Code) {
        ShowPanel(Panel::None);
    }
    if (!g.session->Active()) {
        // Uma tentativa automática pelo USB que nem chegou a conectar: espera antes de tentar de novo.
        if (g.autoUsbRunning && !g.session->LastRunConnected()) g.lastUsbFail = GetTickCount64();
        g.autoUsbRunning = false;
        g.preview.clear();
    }
    RefreshChoices();
    UpdateTray();
    InvalidateRect(g.wnd, nullptr, FALSE);
}

// ------------------------------------------------------------------------------------ pintura

void DrawLines(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &r, flags);
}

void Paint(HDC screen) {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g.font);
    SetBkMode(dc, TRANSPARENT);

    FillRect(dc, &g.barRect, g.barBrush);
    FillRect(dc, &g.videoRect, g.videoBrush);

    Session::Status s = g.session->GetStatus();
    bool active = g.session->Active();

    // Barra: estado à direita (ponto colorido + texto curto).
    COLORREF dot = kMuted;
    std::wstring state = L"Desconectado";
    if (active) {
        switch (s.state) {
            case Session::State::Streaming:
                dot = kOk;
                state = s.brief.empty() ? L"Transmitindo" : s.brief;
                break;
            case Session::State::NeedCode:
                dot = kWait;
                state = L"Aguardando o código";
                break;
            case Session::State::Failed:
                dot = kBad;
                state = L"Reconectando…";
                break;
            default:
                dot = kWait;
                state = L"Conectando…";
        }
    }
    RECT stateRect = {g.barRect.right - S(300), g.barRect.top, g.barRect.right - S(16), g.barRect.bottom};
    SelectObject(dc, g.fontSmall);
    SIZE ts{};
    GetTextExtentPoint32W(dc, state.c_str(), (int)state.size(), &ts);
    int dotX = stateRect.right - ts.cx - S(14), dotY = (g.barRect.bottom - S(8)) / 2;
    HBRUSH dotBrush = CreateSolidBrush(dot);
    HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dotX, dotY, dotX + S(8), dotY + S(8));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(dotBrush);
    DrawLines(dc, state, stateRect, g.fontSmall, kText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT v = g.videoRect;
    int vw = v.right - v.left, vh = v.bottom - v.top;
    if (!g.preview.empty() && g.previewW > 0 && g.panel == Panel::None) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = g.previewW;
        bmi.bmiHeader.biHeight = -g.previewH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        int x = v.left + (vw - g.previewW) / 2, y = v.top + (vh - g.previewH) / 2;
        if (g.previewW <= vw && g.previewH <= vh) {
            SetDIBitsToDevice(dc, x, y, g.previewW, g.previewH, 0, 0, 0, g.previewH, g.preview.data(), &bmi,
                              DIB_RGB_COLORS);
        } else {
            // A janela encolheu e a próxima prévia ainda não chegou: ajusta esta.
            int dw = vw, dh = (int)((int64_t)vw * g.previewH / g.previewW);
            if (dh > vh) {
                dh = vh;
                dw = (int)((int64_t)vh * g.previewW / g.previewH);
            }
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, v.left + (vw - dw) / 2, v.top + (vh - dh) / 2, dw, dh, 0, 0, g.previewW, g.previewH,
                          g.preview.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        if (g.showStats && !s.stats.empty()) {
            RECT sr = {v.left + S(12), v.bottom - S(52), v.right - S(12), v.bottom - S(8)};
            RECT shadow = sr;
            OffsetRect(&shadow, 1, 1);
            DrawLines(dc, s.stats, shadow, g.fontSmall, RGB(0, 0, 0), DT_LEFT | DT_BOTTOM);
            DrawLines(dc, s.stats, sr, g.fontSmall, kText, DT_LEFT | DT_BOTTOM);
        }
    } else if (g.panel == Panel::None) {
        // Tela de espera, no estilo do Iriun: o que está acontecendo e o que fazer.
        std::wstring title, body;
        if (active && s.state == Session::State::Failed) {
            title = s.title;
            body = s.detail;
        } else if (active && s.state == Session::State::Connected) {
            title = s.title;
            body = L"Aguardando vídeo…";
        } else if (active) {
            title = s.title.empty() ? L"Conectando…" : s.title;
        } else if (g.phones.empty()) {
            title = L"Nenhum celular encontrado";
            body = L"Abra o GodoiCan no celular.\n"
                   L"Celular e PC no mesmo Wi-Fi — ou o cabo USB com a depuração USB ligada.\n"
                   L"Celulares já pareados conectam sozinhos.";
        } else {
            title = g.userStopped ? L"Desconectado" : L"Celular encontrado";
            body = L"Escolha o celular na lista acima para conectar.";
        }
        if (!active && !s.detail.empty() && s.state == Session::State::Failed) body += L"\n\n" + s.detail;
        RECT tr = {v.left + S(24), v.top + vh / 2 - S(70), v.right - S(24), v.top + vh / 2 - S(30)};
        DrawLines(dc, title, tr, g.fontTitle, kText, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT br = {v.left + S(24), v.top + vh / 2 - S(18), v.right - S(24), v.bottom - S(40)};
        DrawLines(dc, body, br, g.font, kMuted, DT_CENTER | DT_TOP | DT_WORDBREAK);
        if (!g.cameraReady && !g.cameraMessage.empty()) {
            RECT cr = {v.left + S(24), v.bottom - S(36), v.right - S(24), v.bottom - S(10)};
            DrawLines(dc, g.cameraMessage, cr, g.fontSmall, kWait, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ------------------------------------------------------------------------------------ janela

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.wnd = hwnd;
            g.dpi = GetDpiForWindow(hwnd);
            CreateFonts();
            g.barBrush = CreateSolidBrush(kBar);
            g.videoBrush = CreateSolidBrush(kVideo);
            HINSTANCE inst = GetModuleHandleW(nullptr);
            auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)id, inst,
                                       nullptr);
            };
            g.btnMenu = child(L"BUTTON", L"☰", WS_VISIBLE | BS_PUSHBUTTON, ID_MENU);
            g.combo = child(L"COMBOBOX", L"", WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, ID_DEVICE);
            SendMessageW(g.combo, CB_SETCUEBANNER, 0, (LPARAM)L"Escolha o celular");
            g.panelText = child(L"STATIC", L"", SS_CENTER, ID_PANEL_TEXT);
            g.panelEdit = child(L"EDIT", L"", WS_BORDER | ES_CENTER | ES_AUTOHSCROLL, ID_PANEL_EDIT);
            g.panelOk = child(L"BUTTON", L"OK", BS_DEFPUSHBUTTON, ID_PANEL_OK);
            g.panelCancel = child(L"BUTTON", L"Cancelar", BS_PUSHBUTTON, ID_PANEL_CANCEL);
            ApplyFonts();

            g.showStats = GetPref(L"ShowStats", 0) != 0;
            g.blankIdle = GetPref(L"BlankIdle", 0) != 0;
            g.session = std::make_unique<Session>(
                g.pairs, g.writer, [] { PostMessageW(g.wnd, WM_APP_STATUS, 0, 0); },
                [] { PostMessageW(g.wnd, WM_APP_PREVIEW, 0, 0); });
            g.discovery.Start([] { PostMessageW(g.wnd, WM_APP_DEVICES, 0, 0); });
            StartUsbWatcher();
            SetupCamera(false);
            AddTray();
            RefreshChoices();
            SetTimer(hwnd, TIMER_TICK, 250, nullptr);
            Layout();
            return 0;
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_SHOWWINDOW:
            PostMessageW(hwnd, WM_SIZE, 0, 0);
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {S(520), S(360)};
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
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == g.panelText) {
                SetTextColor((HDC)wp, kText);
                SetBkColor((HDC)wp, kVideo);
                return (LRESULT)g.videoBrush;
            }
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_DEVICE && HIWORD(wp) == CBN_SELCHANGE && !g.suppressCombo) {
                OnChoice();
            } else if (id == ID_MENU) {
                RECT r;
                GetWindowRect(g.btnMenu, &r);
                ShowMainMenu({r.left, r.bottom}, false);
            } else if (id == ID_PANEL_OK) {
                PanelOk();
            } else if (id == ID_PANEL_CANCEL) {
                PanelCancel();
            } else if (id >= IDM_IP && id <= IDM_EXIT) {
                OnMenu(id);
            }
            return 0;
        }
        case WM_APP_STATUS:
            OnStatus();
            return 0;
        case WM_APP_PREVIEW:
            if (g.session->TakePreview(g.preview, g.previewW, g.previewH)) InvalidateRect(hwnd, &g.videoRect, FALSE);
            return 0;
        case WM_APP_DEVICES:
            RefreshChoices();
            AutoTick();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                ShowFromTray();
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                ShowMainMenu(pt, true);
            }
            return 0;
        case WM_TIMER: {
            // Segura o "transmitindo" por 3 s em quedas curtas: a câmera virtual mantém o último quadro.
            bool streaming = g.session->Streaming() || (g.session->Active() && g.session->RecentlyStreaming(3000));
            g.writer.Heartbeat(streaming, g.blankIdle ? GDC_FLAG_BLANK_WHEN_IDLE : 0);
            static int ticks = 0;
            if (++ticks % 4 == 0) AutoTick();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // tudo é pintado no WM_PAINT, com buffer duplo
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            if (g.trayAdded) {
                HideToTray();
                return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_TICK);
            g.quitting = true;
            if (g.usbThread.joinable()) g.usbThread.join();
            g.session.reset();
            g.discovery.Stop();
            RemoveTray();
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
    bool startUsb = false, minimized = false;
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
        if (a == L"--minimized") minimized = true;
        if (a == L"--ip" && i + 1 < argc) startIp = argv[++i];
    }
    LocalFree(argv);

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\GodoiCanApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"GodoiCanWindow", nullptr)) {
            ShowWindow(other, SW_SHOW);
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
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"GodoiCan", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(980, dpi, 96), MulDiv(620, dpi, 96), nullptr,
                                nullptr, inst, nullptr);
    if (!minimized) ShowWindow(hwnd, show);
    if (startUsb) {
        OnMenu(IDM_USB);
    } else if (!startIp.empty()) {
        Target t;
        t.host = Narrow(startIp);
        t.name = t.host;
        t.persistent = true;
        Start(t);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == g.panelEdit) {
            if (msg.wParam == VK_RETURN) {
                PanelOk();
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                PanelCancel();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
