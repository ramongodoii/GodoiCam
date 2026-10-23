#include "VCam.h"

#include <algorithm>

MediaStream::MediaStream() = default;

MediaStream::~MediaStream() {
    Shutdown();
    if (m_wake) CloseHandle(m_wake);
}

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* source) {
    m_source = source;
    m_descriptor = source->Descriptor();
    HRESULT hr = MFCreateEventQueue(&m_queue);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&m_attributes, 4);
    if (FAILED(hr)) return hr;
    hr = m_attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(hr)) hr = m_attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (SUCCEEDED(hr)) hr = m_attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    if (SUCCEEDED(hr))
        hr = m_attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaTypeHandler> handler;
    if (SUCCEEDED(m_descriptor->GetMediaTypeHandler(&handler))) {
        ComPtr<IMFMediaType> type;
        if (SUCCEEDED(handler->GetCurrentMediaType(&type))) ApplyType(type.Get());
    }

    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wake) return HRESULT_FROM_WIN32(GetLastError());
    m_frames = std::make_unique<FrameSource>();
    m_worker = std::thread([this] { Worker(); });
    return S_OK;
}

HRESULT MediaStream::ApplyType(IMFMediaType* type) {
    UINT32 w = 0, h = 0, num = 0, den = 0;
    GUID subtype{};
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDMEDIATYPE;
    if (subtype != MFVideoFormat_NV12 && subtype != MFVideoFormat_RGB32) return MF_E_INVALIDMEDIATYPE;
    if (w == 0 || h == 0 || w > GDC_MAX_W || h > GDC_MAX_H || (w & 1) || (h & 1)) return MF_E_INVALIDMEDIATYPE;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) || num == 0 || den == 0) {
        num = 30;
        den = 1;
    }
    m_type = type;
    m_width = w;
    m_height = h;
    m_fpsNum = num;
    m_fpsDen = den;
    m_subtype = subtype;
    return S_OK;
}

// ------------------------------------------------------------------------------------ eventos

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_queue->BeginGetEvent(cb, state);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) {
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_queue->EndGetEvent(result, ev);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** ev) {
    ComPtr<IMFMediaEventQueue> q;
    AcquireSRWLockShared(&m_lock);
    if (!m_shutdown) q = m_queue;
    ReleaseSRWLockShared(&m_lock);
    return q ? q->GetEvent(flags, ev) : MF_E_SHUTDOWN;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* value) {
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_queue->QueueEventParamVar(type, ext, status, value);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

// ------------------------------------------------------------------------------------ IMFMediaStream

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_source.CopyTo(source);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** sd) {
    if (!sd) return E_POINTER;
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_descriptor.CopyTo(sd);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = S_OK;
    if (m_shutdown) {
        hr = MF_E_SHUTDOWN;
    } else if (m_state != MF_STREAM_STATE_RUNNING) {
        hr = MF_E_MEDIA_SOURCE_WRONGSTATE;
    } else if (m_tokens.size() >= 16) {
        hr = MF_E_SAMPLEALLOCATOR_EMPTY;
    } else {
        m_tokens.emplace_back(token);
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (SUCCEEDED(hr)) SetEvent(m_wake);
    return hr;
}

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    switch (state) {
        case MF_STREAM_STATE_RUNNING:
            return Start(nullptr);
        case MF_STREAM_STATE_STOPPED:
            return Stop();
        case MF_STREAM_STATE_PAUSED: {
            AcquireSRWLockExclusive(&m_lock);
            HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : S_OK;
            if (SUCCEEDED(hr)) m_state = MF_STREAM_STATE_PAUSED;
            ReleaseSRWLockExclusive(&m_lock);
            return hr;
        }
        default:
            return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    AcquireSRWLockShared(&m_lock);
    *state = m_state;
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : S_OK;
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ------------------------------------------------------------------------------------ controle

HRESULT MediaStream::Start(IMFMediaType* type) {
    AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : S_OK;
    if (SUCCEEDED(hr) && type) hr = ApplyType(type);
    if (SUCCEEDED(hr)) {
        m_state = MF_STREAM_STATE_RUNNING;
        PROPVARIANT time;
        PropVariantInit(&time);
        time.vt = VT_I8;
        time.hVal.QuadPart = MFGetSystemTime();
        hr = m_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &time);
    }
    ReleaseSRWLockExclusive(&m_lock);
    SetEvent(m_wake);
    return hr;
}

HRESULT MediaStream::Stop() {
    AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : S_OK;
    if (SUCCEEDED(hr)) {
        m_state = MF_STREAM_STATE_STOPPED;
        m_tokens.clear();
        PROPVARIANT time;
        PropVariantInit(&time);
        time.vt = VT_I8;
        time.hVal.QuadPart = MFGetSystemTime();
        hr = m_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, &time);
    }
    ReleaseSRWLockExclusive(&m_lock);
    SetEvent(m_wake);
    return hr;
}

HRESULT MediaStream::SetMediaType(IMFMediaType* type) {
    AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : ApplyType(type);
    ReleaseSRWLockExclusive(&m_lock);
    return hr;
}

void MediaStream::Shutdown() {
    AcquireSRWLockExclusive(&m_lock);
    bool already = m_shutdown;
    m_shutdown = true;
    m_tokens.clear();
    if (!already && m_queue) m_queue->Shutdown();
    ReleaseSRWLockExclusive(&m_lock);

    if (m_wake) SetEvent(m_wake);
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) m_worker.join();

    AcquireSRWLockExclusive(&m_lock);
    m_source.Reset();  // quebra o ciclo fonte ↔ stream
    m_frames.reset();
    ReleaseSRWLockExclusive(&m_lock);
}

// ------------------------------------------------------------------------------------ entrega

// Atende cada pedido assim que chega um quadro novo do app; se nada chegar dentro de
// 1,5 intervalo de quadro, repete o último (ou a tela de espera) para o app não travar.
void MediaStream::Worker() {
    HANDLE frameEvent = m_frames ? m_frames->FrameEvent() : nullptr;
    for (;;) {
        AcquireSRWLockShared(&m_lock);
        bool down = m_shutdown;
        bool pending = !m_tokens.empty() && m_state == MF_STREAM_STATE_RUNNING;
        DWORD intervalMs = (DWORD)((1000ull * m_fpsDen) / m_fpsNum);
        ReleaseSRWLockShared(&m_lock);
        if (down) return;
        if (!pending) {
            WaitForSingleObject(m_wake, 500);
            continue;
        }

        DWORD timeout = intervalMs + intervalMs / 2;
        if (frameEvent) {
            HANDLE handles[2] = {m_wake, frameEvent};
            DWORD r = WaitForMultipleObjects(2, handles, FALSE, timeout);
            if (r == WAIT_OBJECT_0) continue;  // desligamento ou mudança de estado
        } else {
            Sleep(intervalMs);
        }

        ComPtr<IUnknown> token;
        AcquireSRWLockExclusive(&m_lock);
        bool ok = !m_shutdown && m_state == MF_STREAM_STATE_RUNNING && !m_tokens.empty();
        if (ok) {
            token = m_tokens.front();
            m_tokens.pop_front();
        }
        ReleaseSRWLockExclusive(&m_lock);
        if (ok) DeliverSample(token.Get());
    }
}

HRESULT MediaStream::DeliverSample(IUnknown* token) {
    AcquireSRWLockShared(&m_lock);
    UINT32 w = m_width, h = m_height, num = m_fpsNum, den = m_fpsDen;
    GUID subtype = m_subtype;
    ReleaseSRWLockShared(&m_lock);

    DWORD fourcc = subtype == MFVideoFormat_NV12 ? MFVideoFormat_NV12.Data1 : MFVideoFormat_RGB32.Data1;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreate2DMediaBuffer(w, h, fourcc, FALSE, &buffer);
    if (FAILED(hr)) return hr;
    ComPtr<IMF2DBuffer2> buffer2d;
    hr = buffer.As(&buffer2d);
    if (FAILED(hr)) return hr;

    BYTE* scanline0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    hr = buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length);
    if (FAILED(hr)) return hr;
    m_frames->Fill(scanline0, pitch, w, h, subtype);
    buffer2d->Unlock2D();
    DWORD contiguous = 0;
    if (SUCCEEDED(buffer2d->GetContiguousLength(&contiguous))) buffer->SetCurrentLength(contiguous);

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration((LONGLONG)10000000 * den / num);
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);

    AcquireSRWLockShared(&m_lock);
    hr = m_shutdown ? MF_E_SHUTDOWN : m_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    ReleaseSRWLockShared(&m_lock);
    return hr;
}
