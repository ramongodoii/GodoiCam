#include "Decoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <stdio.h>

using Microsoft::WRL::ComPtr;

bool H264Decoder::Init(int width, int height, int fps) {
    Close();
    m_width = width;
    m_height = height;
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
    return true;
}

void H264Decoder::Close() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    m_mft.Reset();
    m_out.Reset();
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
