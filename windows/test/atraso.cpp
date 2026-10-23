// atraso: mede o atraso "vidro a vidro" de qualquer câmera (GodoiCan, Iriun...).
//
// A janela pisca entre preto e branco em tela cheia. Aponte a câmera do celular para a tela
// (bem perto, para a tela ocupar a imagem toda). O programa lê a câmera pelo Media Foundation,
// como um app de vídeo faria, e mede quanto tempo cada troca leva para aparecer na imagem.
// Inclui: tela do PC + câmera do celular + codificação + rede + decodificação + câmera virtual.
//
//   atraso <parte do nome da câmera> [trocas=30]
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <stdio.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

double NowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

struct Flash {
    double time;
    bool white;
};

std::mutex g_lock;
std::vector<Flash> g_flashes;
std::vector<double> g_delays;
std::atomic<bool> g_white{false};
std::atomic<bool> g_done{false};
std::atomic<int> g_frames{0};
std::atomic<double> g_luma{0};
std::wstring g_status = L"Abrindo a câmera…";
HWND g_wnd;
int g_target = 30;

// Brilho médio do terço central da imagem (plano Y do NV12).
double CenterLuma(const BYTE* y, int w, int h) {
    uint64_t sum = 0;
    int count = 0;
    for (int r = h / 3; r < 2 * h / 3; r += 4)
        for (int c = w / 3; c < 2 * w / 3; c += 4) {
            sum += y[(size_t)r * w + c];
            count++;
        }
    return count ? (double)sum / count : 0;
}

void CameraThread(ComPtr<IMFActivate> device) {
    ComPtr<IMFMediaSource> source;
    if (FAILED(device->ActivateObject(IID_PPV_ARGS(&source)))) {
        g_status = L"Não consegui abrir a câmera.";
        return;
    }
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    MFCreateSourceReaderFromMediaSource(source.Get(), a.Get(), &reader);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get());
    ComPtr<IMFMediaType> cur;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);

    double lo = 255, hi = 0;
    size_t next = 0;  // próxima troca ainda não detectada
    while (!g_done) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        double arrival = NowMs();
        if (!s) continue;
        ComPtr<IMFMediaBuffer> b;
        s->ConvertToContiguousBuffer(&b);
        BYTE* p = nullptr;
        DWORD len = 0;
        b->Lock(&p, nullptr, &len);
        double luma = len >= w * h ? CenterLuma(p, w, h) : 0;
        b->Unlock();
        g_frames++;
        g_luma = luma;
        lo = std::min(lo, luma);
        hi = std::max(hi, luma);
        if (hi - lo < 40) continue;  // ainda calibrando: precisa ver preto e branco
        double threshold = (lo + hi) / 2;

        std::lock_guard<std::mutex> lock(g_lock);
        while (next < g_flashes.size()) {
            const Flash& f = g_flashes[next];
            if (arrival < f.time) break;
            bool seenWhite = luma > threshold;
            if (seenWhite != f.white) break;
            double d = arrival - f.time;
            // A primeira troca serve só de calibração; atrasos absurdos são perdas de sincronia.
            if (next > 0 && d < 1500) g_delays.push_back(d);
            next++;
        }
        if ((int)g_delays.size() >= g_target) g_done = true;
    }
    source->Shutdown();
}

std::wstring Summary() {
    std::vector<double> d;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        d = g_delays;
    }
    if (d.empty()) return L"Sem medições ainda.";
    std::sort(d.begin(), d.end());
    auto pct = [&](double q) { return d[std::min(d.size() - 1, (size_t)(q * (d.size() - 1) + 0.5))]; };
    wchar_t buf[256];
    swprintf_s(buf, L"%zu medições • mín %.0f ms • p50 %.0f ms • p95 %.0f ms • máx %.0f ms", d.size(), d.front(),
               pct(0.5), pct(0.95), d.back());
    return buf;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER: {
            if (g_done) {
                KillTimer(hwnd, 1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            g_white = !g_white;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);  // pinta agora, antes de registrar o horário
            GdiFlush();
            {
                std::lock_guard<std::mutex> lock(g_lock);
                g_flashes.push_back({NowMs(), g_white.load()});
            }
            static std::mt19937 rng(1234);
            SetTimer(hwnd, 1, 900 + rng() % 500, nullptr);  // intervalo irregular evita "sincronizar" com a câmera
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            bool done = g_done;
            FillRect(dc, &rc, (HBRUSH)GetStockObject(done ? GRAY_BRUSH : (g_white ? WHITE_BRUSH : BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, done ? RGB(255, 255, 255) : RGB(128, 128, 128));
            wchar_t line[512];
            swprintf_s(line, L"%s\n%s\nquadros recebidos: %d • brilho no centro: %.0f\nEsc para sair",
                       done ? L"Pronto!" : L"Aponte a câmera do celular para esta tela.", Summary().c_str(),
                       g_frames.load(), g_luma.load());
            RECT text = rc;
            text.top = rc.bottom - 140;
            DrawTextW(dc, line, -1, &text, DT_CENTER);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_done = true;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        wprintf(L"uso: atraso <parte do nome da câmera> [trocas]\n");
        return 2;
    }
    if (argc > 2) g_target = _wtoi(argv[2]);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** list = nullptr;
    UINT32 n = 0;
    MFEnumDeviceSources(a.Get(), &list, &n);
    ComPtr<IMFActivate> chosen;
    std::wstring chosenName;
    for (UINT32 i = 0; i < n; i++) {
        WCHAR* name = nullptr;
        UINT32 len = 0;
        list[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        if (!chosen && name && wcsstr(name, argv[1])) {
            chosen = list[i];
            chosenName = name;
        }
        CoTaskMemFree(name);
        list[i]->Release();
    }
    CoTaskMemFree(list);
    if (!chosen) {
        wprintf(L"câmera \"%s\" não encontrada\n", argv[1]);
        return 1;
    }
    wprintf(L"Câmera: %s\n", chosenName.c_str());
    std::thread cam(CameraThread, chosen);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GodoiCanAtraso";
    RegisterClassW(&wc);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT r = mi.rcMonitor;
    g_wnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"GodoiCan — medidor de atraso", WS_POPUP | WS_VISIBLE,
                            r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance,
                            nullptr);
    SetTimer(g_wnd, 1, 1000, nullptr);
    SetTimer(g_wnd, 2, 250, [](HWND h, UINT, UINT_PTR, DWORD) { InvalidateRect(h, nullptr, FALSE); });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_done = true;
    cam.join();

    std::wstring result = Summary();
    wprintf(L"%s\n", result.c_str());
    PWSTR dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &dir))) {
        std::wstring path = std::wstring(dir) + L"\\GodoiCan\\atraso.txt";
        if (FILE* f = _wfopen(path.c_str(), L"a, ccs=UTF-8")) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fwprintf(f, L"%04d-%02d-%02d %02d:%02d • %s • %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                     chosenName.c_str(), result.c_str());
            {
                std::lock_guard<std::mutex> lock(g_lock);
                for (double d : g_delays) fwprintf(f, L"  %.1f\n", d);
            }
            fclose(f);
            wprintf(L"Salvo em %s\n", path.c_str());
        }
    }
    CoTaskMemFree(dir);
    return 0;
}
