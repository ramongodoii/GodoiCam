// camprobe: lista câmeras do Media Foundation, abre uma e lê quadros, mostrando erros e tempos.
//   camprobe list
//   camprobe read <parte do nome> [quadros] [saida.bmp]
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <wrl/client.h>

#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static std::vector<ComPtr<IMFActivate>> Devices(std::vector<std::wstring>& names) {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** list = nullptr;
    UINT32 n = 0;
    std::vector<ComPtr<IMFActivate>> out;
    if (FAILED(MFEnumDeviceSources(a.Get(), &list, &n))) return out;
    for (UINT32 i = 0; i < n; i++) {
        WCHAR* name = nullptr;
        UINT32 len = 0;
        list[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        names.push_back(name ? name : L"?");
        CoTaskMemFree(name);
        out.emplace_back(list[i]);
        list[i]->Release();
    }
    CoTaskMemFree(list);
    return out;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    std::vector<std::wstring> names;
    auto devices = Devices(names);
    if (argc < 3 || std::wstring(argv[1]) == L"list") {
        for (size_t i = 0; i < names.size(); i++) wprintf(L"%zu: %s\n", i, names[i].c_str());
        return 0;
    }
    int idx = -1;
    for (size_t i = 0; i < names.size(); i++)
        if (names[i].find(argv[2]) != std::wstring::npos) idx = (int)i;
    if (idx < 0) {
        wprintf(L"camera nao encontrada\n");
        return 1;
    }
    int frames = argc > 3 ? _wtoi(argv[3]) : 60;
    wprintf(L"abrindo: %s\n", names[idx].c_str());

    double t0 = NowMs();
    ComPtr<IMFMediaSource> source;
    HRESULT hr = devices[idx]->ActivateObject(IID_PPV_ARGS(&source));
    wprintf(L"ActivateObject: 0x%08X (%.0f ms)\n", (unsigned)hr, NowMs() - t0);
    if (FAILED(hr)) return 2;

    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 1);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader);
    wprintf(L"CreateSourceReader: 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 3;

    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
        UINT32 w = 0, h = 0, num = 0, den = 0;
        GUID sub{};
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        char fcc[5] = {(char)(sub.Data1), (char)(sub.Data1 >> 8), (char)(sub.Data1 >> 16), (char)(sub.Data1 >> 24), 0};
        wprintf(L"  tipo %lu: %ux%u @ %u/%u %S\n", i, w, h, num, den, sub.Data1 < 0x100 ? "RGB32" : fcc);
    }

    ComPtr<IMFMediaType> nv12;
    MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get());
    wprintf(L"SetCurrentMediaType NV12: 0x%08X\n", (unsigned)hr);
    ComPtr<IMFMediaType> cur;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);

    double first = 0, last = 0, maxGap = 0;
    int got = 0;
    for (int i = 0; i < frames; i++) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        double before = NowMs();
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s);
        double now = NowMs();
        if (FAILED(hr)) {
            wprintf(L"ReadSample: 0x%08X\n", (unsigned)hr);
            return 4;
        }
        if (!s) continue;
        if (got == 0) {
            first = now;
            wprintf(L"primeiro quadro em %.0f ms (%ux%u)\n", now - t0, w, h);
        } else if (now - last > maxGap) {
            maxGap = now - last;
        }
        last = now;
        got++;
        if (i == frames - 1 && argc > 4) {
            ComPtr<IMFMediaBuffer> b;
            s->ConvertToContiguousBuffer(&b);
            BYTE* p = nullptr;
            DWORD len = 0;
            b->Lock(&p, nullptr, &len);
            // Salva só o plano Y como BMP em tons de cinza.
            FILE* f = _wfopen(argv[4], L"wb");
            if (f) {
                BITMAPFILEHEADER fh{0x4D42};
                BITMAPINFOHEADER ih{sizeof(ih), (LONG)w, -(LONG)h, 1, 8};
                RGBQUAD pal[256];
                for (int k = 0; k < 256; k++) pal[k] = {(BYTE)k, (BYTE)k, (BYTE)k, 0};
                fh.bfOffBits = sizeof(fh) + sizeof(ih) + sizeof(pal);
                fh.bfSize = fh.bfOffBits + w * h;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                fwrite(pal, sizeof(pal), 1, f);
                fwrite(p, 1, (size_t)w * h, f);
                fclose(f);
            }
            b->Unlock();
        }
    }
    if (got > 1)
        wprintf(L"%d quadros, %.1f fps, maior intervalo %.0f ms\n", got, (got - 1) * 1000.0 / (last - first), maxGap);
    source->Shutdown();
    return 0;
}
