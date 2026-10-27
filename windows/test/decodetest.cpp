// Reproduz um arquivo gravado com GODOICAM_DUMP no decodificador do app.
#include <windows.h>
#include <mfapi.h>
#include <stdio.h>
#include <vector>
#include "../app/Decoder.h"
#include "../app/H264Sps.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    FILE* f = _wfopen(argv[1], L"rb");
    if (!f) return 3;
    int w = argc > 2 ? _wtoi(argv[2]) : 1280, h = argc > 3 ? _wtoi(argv[3]) : 720;
    H264Decoder dec;
    if (argc > 4 && _wtoi(argv[4]) == 0) dec.ForceSoftware();
    if (!dec.Init(w, h, 30)) { printf("init falhou\n"); return 4; }
    printf("decodificador: %s %ls\n", dec.Hardware() ? "GPU" : "CPU", dec.AdapterName().c_str());
    int in = 0, out = 0;
    uint32_t n; int64_t pts;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    double totalMs = 0, maxMs = 0;
    bool colorShown = false;
    while (fread(&n, 4, 1, f) == 1 && fread(&pts, 8, 1, f) == 1) {
        std::vector<uint8_t> d(n);
        fread(d.data(), 1, n, f);
        in++;
        H264Color color;
        if (!colorShown && ReadH264Color(d.data(), n, color)) {
            colorShown = true;
            printf("cor: %s\n", DescribeH264Color(color).c_str());
        }
        QueryPerformanceCounter(&t0);
        bool ok = dec.Decode(d.data(), n, pts, [&](const DecodedFrame& fr) {
            // Soma de conferência do quadro 100 (comparar GPU × CPU).
            if (++out == 100) {
                uint64_t sum = 0;
                for (int row = 0; row < fr.height; row++)
                    for (int col = 0; col < fr.width; col += 7) sum += fr.y[row * fr.yPitch + col];
                for (int row = 0; row < fr.height / 2; row++)
                    for (int col = 0; col < fr.width; col += 7) sum += fr.uv[row * fr.uvPitch + col];
                printf("quadro 100: %dx%d soma %llu\n", fr.width, fr.height, (unsigned long long)sum);
            }
        });
        QueryPerformanceCounter(&t1);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        totalMs += ms;
        if (ms > maxMs) maxMs = ms;
        if (!ok) printf("pacote %d (%u bytes, nal0=%d): %s\n", in, n, n > 4 ? (d[4] & 31) : -1, dec.LastError());
    }
    printf("entrada %d, saida %d, decodificacao media %.2f ms, max %.2f ms\n", in, out, in ? totalMs / in : 0.0, maxMs);
    return 0;
}
