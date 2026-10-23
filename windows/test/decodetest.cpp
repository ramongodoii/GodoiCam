// Reproduz um arquivo gravado com GODOICAN_DUMP no decodificador do app.
#include <windows.h>
#include <mfapi.h>
#include <stdio.h>
#include <vector>
#include "../app/Decoder.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    FILE* f = _wfopen(argv[1], L"rb");
    if (!f) return 3;
    int w = argc > 2 ? _wtoi(argv[2]) : 1280, h = argc > 3 ? _wtoi(argv[3]) : 720;
    H264Decoder dec;
    if (!dec.Init(w, h, 30)) { printf("init falhou\n"); return 4; }
    int in = 0, out = 0;
    uint32_t n; int64_t pts;
    while (fread(&n, 4, 1, f) == 1 && fread(&pts, 8, 1, f) == 1) {
        std::vector<uint8_t> d(n);
        fread(d.data(), 1, n, f);
        in++;
        bool ok = dec.Decode(d.data(), n, pts, [&](const DecodedFrame& fr) { out++; });
        if (!ok) printf("pacote %d (%u bytes, nal0=%d): %s\n", in, n, n > 4 ? (d[4] & 31) : -1, dec.LastError());
    }
    printf("entrada %d, saida %d\n", in, out);
    return 0;
}
