#include "Decoder.h"

#include <codecapi.h>
#include <dxgi1_2.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <stdio.h>

using Microsoft::WRL::ComPtr;

bool H264Decoder::Init(int width, int height, int fps) {
    if (!m_noHardware && InitWith(width, height, fps, true)) return true;
    return InitWith(width, height, fps, false);
}

// Placa de vídeo com mais memória dedicada: evita adaptadores virtuais (Parsec, Basic Render)
// que podem aparecer como "padrão" e não decodificam H.264.
bool H264Decoder::CreateDevice() {
    if (m_deviceTried) return m_manager != nullptr;
    m_deviceTried = true;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> best;
    SIZE_T bestMemory = 0;
    DXGI_ADAPTER_DESC1 bestDesc{};
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            d.DedicatedVideoMemory > bestMemory) {
            best = adapter;
            bestMemory = d.DedicatedVideoMemory;
            bestDesc = d;
        }
        adapter.Reset();
    }
    if (!best) return false;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    HRESULT hr = D3D11CreateDevice(best.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
    if (FAILED(hr)) return false;
    // O MFT usa o dispositivo nas threads dele; nós copiamos a textura nesta.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &m_manager)) || FAILED(m_manager->ResetDevice(m_device.Get(), token))) {
        m_manager.Reset();
        m_context.Reset();
        m_device.Reset();
        return false;
    }
    m_adapterName = bestDesc.Description;
    return true;
}

bool H264Decoder::InitWith(int width, int height, int fps, bool hardware) {
    Close();
    m_width = width;
    m_height = height;
    m_hardware = false;
    if (hardware && !CreateDevice()) return false;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_mft))))
        return false;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m_mft->GetAttributes(&attrs))) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(m_mft.As(&codec))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    if (hardware) {
        UINT32 aware = 0;
        if (!attrs || FAILED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware ||
            FAILED(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_manager.Get()))) {
            Close();
            return false;
        }
    }

    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps > 0 ? fps : 30, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(m_mft->SetInputType(0, in.Get(), 0)) || !SetOutputNV12()) {
        Close();
        return false;
    }
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_hardware = hardware;
    return true;
}

void H264Decoder::Close() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    m_mft.Reset();
    m_out.Reset();
    m_staging.Reset();
}

bool H264Decoder::SetOutputNV12() {
    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t;
        if (FAILED(m_mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub != MFVideoFormat_NV12) continue;
        if (FAILED(m_mft->SetOutputType(0, t.Get(), 0))) return false;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &m_frameW, &m_frameH);
        UINT32 stride = 0;
        m_stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)m_frameW;
        MFT_OUTPUT_STREAM_INFO info{};
        m_mft->GetOutputStreamInfo(0, &info);
        m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                             MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m_outSize = info.cbSize;
        m_out.Reset();
        m_staging.Reset();
        return true;
    }
}

bool H264Decoder::Decode(const uint8_t* data, size_t len, int64_t ptsUs, const Sink& sink) {
    if (!m_mft) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer((DWORD)len, &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, data, len);
    buf->Unlock();
    buf->SetCurrentLength((DWORD)len);
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(ptsUs * 10);

    HRESULT hr = m_mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!Drain(sink)) return false;
        hr = m_mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        sprintf_s(m_error, "ProcessInput 0x%08X", (unsigned)hr);
        return false;
    }
    return Drain(sink);
}

bool H264Decoder::DeliverTexture(IMFMediaBuffer* buffer, int64_t ptsUs, const Sink& sink) {
    ComPtr<IMFDXGIBuffer> dxgi;
    if (!m_context || FAILED(buffer->QueryInterface(IID_PPV_ARGS(&dxgi)))) return false;
    ComPtr<ID3D11Texture2D> tex;
    UINT sub = 0;
    if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgi->GetSubresourceIndex(&sub))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sd{};
    if (m_staging) m_staging->GetDesc(&sd);
    if (!m_staging || sd.Width != desc.Width || sd.Height != desc.Height || sd.Format != desc.Format) {
        m_staging.Reset();
        sd = desc;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc = {1, 0};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &m_staging))) return false;
    }
    m_context->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

    int w = m_width > 0 && m_width <= (int)desc.Width ? m_width : (int)desc.Width;
    int h = m_height > 0 && m_height <= (int)desc.Height ? m_height : (int)desc.Height;
    DecodedFrame f{};
    f.y = (const uint8_t*)m.pData;
    f.yPitch = (int)m.RowPitch;
    // NV12 mapeado: o plano UV começa logo depois das linhas de Y da textura inteira.
    f.uv = f.y + (size_t)m.RowPitch * desc.Height;
    f.uvPitch = (int)m.RowPitch;
    f.width = w & ~1;
    f.height = h & ~1;
    f.ptsUs = ptsUs;
    sink(f);
    m_context->Unmap(m_staging.Get(), 0);
    return true;
}

bool H264Decoder::Drain(const Sink& sink) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        if (!m_providesSamples) {
            if (!m_out) {
                ComPtr<IMFMediaBuffer> b;
                if (FAILED(MFCreateMemoryBuffer(m_outSize, &b))) return false;
                MFCreateSample(&m_out);
                m_out->AddBuffer(b.Get());
            }
            // O decodificador recusa (E_FAIL) um buffer que ainda diz estar cheio do quadro anterior.
            ComPtr<IMFMediaBuffer> reuse;
            if (SUCCEEDED(m_out->GetBufferByIndex(0, &reuse))) reuse->SetCurrentLength(0);
            out.pSample = m_out.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SetOutputNV12()) return false;
            continue;
        }
        if (FAILED(hr)) {
            sprintf_s(m_error, "ProcessOutput 0x%08X", (unsigned)hr);
            return false;
        }

        ComPtr<IMFSample> produced = out.pSample;
        if (m_providesSamples && out.pSample) out.pSample->Release();
        if (!produced) continue;

        LONGLONG time = 0;
        produced->GetSampleTime(&time);

        if (m_hardware) {
            ComPtr<IMFMediaBuffer> first;
            if (SUCCEEDED(produced->GetBufferByIndex(0, &first)) && DeliverTexture(first.Get(), time / 10, sink))
                continue;
        }

        ComPtr<IMFMediaBuffer> b;
        if (FAILED(produced->ConvertToContiguousBuffer(&b))) continue;

        int w = m_width > 0 && m_width <= (int)m_frameW ? m_width : (int)m_frameW;
        int h = m_height > 0 && m_height <= (int)m_frameH ? m_height : (int)m_frameH;
        ComPtr<IMF2DBuffer> b2d;
        BYTE* base = nullptr;
        LONG pitch = 0;
        bool locked2d = SUCCEEDED(b.As(&b2d)) && SUCCEEDED(b2d->Lock2D(&base, &pitch));
        DWORD maxLen = 0, curLen = 0;
        if (!locked2d) {
            if (FAILED(b->Lock(&base, &maxLen, &curLen))) continue;
            pitch = m_stride > 0 ? m_stride : (LONG)m_frameW;
        }
        DecodedFrame f{};
        f.y = base;
        f.yPitch = pitch;
        f.uv = base + (size_t)pitch * m_frameH;
        f.uvPitch = pitch;
        f.width = w & ~1;
        f.height = h & ~1;
        f.ptsUs = time / 10;
        sink(f);
        if (locked2d) b2d->Unlock2D();
        else b->Unlock();
    }
}
