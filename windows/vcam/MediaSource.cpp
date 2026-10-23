#include "VCam.h"

#include <vector>

namespace {

struct Format {
    UINT32 width, height, fps;
    GUID subtype;
};

// O primeiro é o padrão. Todos 16:9 para nunca distorcer a imagem do celular.
const Format kFormats[] = {
    {1920, 1080, 30, MFVideoFormat_NV12}, {1280, 720, 30, MFVideoFormat_NV12},  {1920, 1080, 60, MFVideoFormat_NV12},
    {1280, 720, 60, MFVideoFormat_NV12},  {3840, 2160, 30, MFVideoFormat_NV12}, {2560, 1440, 30, MFVideoFormat_NV12},
    {960, 540, 30, MFVideoFormat_NV12},   {640, 360, 30, MFVideoFormat_NV12},   {1920, 1080, 30, MFVideoFormat_RGB32},
    {1280, 720, 30, MFVideoFormat_RGB32},
};

HRESULT CreateType(const Format& f, IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    bool nv12 = f.subtype == MFVideoFormat_NV12;
    UINT32 stride = nv12 ? f.width : f.width * 4;
    UINT32 size = nv12 ? f.width * f.height * 3 / 2 : f.width * f.height * 4;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, f.subtype);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, f.width, f.height);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, f.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, size);
    t->SetUINT32(MF_MT_AVG_BITRATE, size * 8 * f.fps);
    if (nv12) {
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    }
    *out = t.Detach();
    return S_OK;
}

HRESULT SetStreamAttributes(IMFAttributes* a) {
    HRESULT hr = a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    return hr;
}

}  // namespace

// ------------------------------------------------------------------------------------ MediaSource

MediaSource::MediaSource() = default;

MediaSource::~MediaSource() {
    Shutdown();
    if (m_mfStarted) MFShutdown();
}

HRESULT MediaSource::RuntimeClassInitialize() {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return hr;
    m_mfStarted = true;

    hr = MFCreateEventQueue(&m_queue);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&m_attributes, 4);
    if (FAILED(hr)) return hr;

    std::vector<ComPtr<IMFMediaType>> types;
    std::vector<IMFMediaType*> raw;
    for (const Format& f : kFormats) {
        ComPtr<IMFMediaType> t;
        hr = CreateType(f, &t);
        if (FAILED(hr)) return hr;
        raw.push_back(t.Get());
        types.push_back(t);
    }
    hr = MFCreateStreamDescriptor(0, (DWORD)raw.size(), raw.data(), &m_descriptor);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = m_descriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(raw[0]);
    if (FAILED(hr)) return hr;
    hr = SetStreamAttributes(m_descriptor.Get());
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* sd = m_descriptor.Get();
    hr = MFCreatePresentationDescriptor(1, &sd, &m_pd);
    if (FAILED(hr)) return hr;
    hr = m_pd->SelectStream(0);
    if (FAILED(hr)) return hr;

    hr = Microsoft::WRL::MakeAndInitialize<MediaStream>(&m_stream, this);
    return hr;
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_queue->BeginGetEvent(cb, state);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) {
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_queue->EndGetEvent(result, ev);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** ev) {
    ComPtr<IMFMediaEventQueue> q;
    AcquireSRWLockShared(&m_lock);
    if (!m_shutdown) q = m_queue;
    ReleaseSRWLockShared(&m_lock);
    return q ? q->GetEvent(flags, ev) : MF_E_SHUTDOWN;  // pode bloquear: fora da trava
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* value) {
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_queue->QueueEventParamVar(type, ext, status, value);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    AcquireSRWLockShared(&m_lock);
    bool down = m_shutdown;
    ReleaseSRWLockShared(&m_lock);
    if (down) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
    if (!pd) return E_POINTER;
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_pd->Clone(pd);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* start) {
    if (!pd || !start) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    if (start->vt != VT_EMPTY && start->vt != VT_I8) return MF_E_UNSUPPORTED_TIME_FORMAT;

    AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = S_OK;
    if (m_shutdown) hr = MF_E_SHUTDOWN;

    DWORD count = 0;
    if (SUCCEEDED(hr)) hr = pd->GetStreamDescriptorCount(&count);
    for (DWORD i = 0; SUCCEEDED(hr) && i < count; i++) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(i, &selected, &sd);
        if (FAILED(hr)) break;
        DWORD id = 0;
        sd->GetStreamIdentifier(&id);
        if (id != 0) {
            hr = MF_E_INVALIDSTREAMNUMBER;
            break;
        }
        if (selected) {
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&handler))) handler->GetCurrentMediaType(&type);
            ComPtr<IUnknown> unk;
            m_stream.As(&unk);
            hr = m_queue->QueueEventParamUnk(m_streamActive ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                             unk.Get());
            if (SUCCEEDED(hr)) hr = m_stream->Start(type.Get());
            if (SUCCEEDED(hr)) m_streamActive = true;
        } else if (m_streamActive) {
            m_stream->Stop();
            m_streamActive = false;
        }
    }

    if (SUCCEEDED(hr)) {
        PROPVARIANT time;
        PropVariantInit(&time);
        time.vt = VT_I8;
        time.hVal.QuadPart = MFGetSystemTime();
        hr = m_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
    }
    ReleaseSRWLockExclusive(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::Stop() {
    AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = S_OK;
    if (m_shutdown) {
        hr = MF_E_SHUTDOWN;
    } else {
        if (m_streamActive) m_stream->Stop();
        m_streamActive = false;
        PROPVARIANT time;
        PropVariantInit(&time);
        time.vt = VT_I8;
        time.hVal.QuadPart = MFGetSystemTime();
        hr = m_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
    }
    ReleaseSRWLockExclusive(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Shutdown() {
    AcquireSRWLockExclusive(&m_lock);
    if (m_shutdown) {
        ReleaseSRWLockExclusive(&m_lock);
        return MF_E_SHUTDOWN;
    }
    m_shutdown = true;
    ComPtr<MediaStream> stream = m_stream;
    if (m_queue) m_queue->Shutdown();
    ReleaseSRWLockExclusive(&m_lock);

    if (stream) stream->Shutdown();  // espera a thread do stream: fora da trava
    AcquireSRWLockExclusive(&m_lock);
    m_stream.Reset();
    ReleaseSRWLockExclusive(&m_lock);
    return S_OK;
}

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_attributes.CopyTo(attributes);
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD id, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    AcquireSRWLockShared(&m_lock);
    HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_stream.CopyTo(IID_PPV_ARGS(attributes));
    ReleaseSRWLockShared(&m_lock);
    return hr;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
    return S_OK;  // as amostras são em memória de sistema
}

STDMETHODIMP MediaSource::SetMediaType(DWORD id, IMFMediaType* type) {
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (!type) return E_POINTER;
    AcquireSRWLockShared(&m_lock);
    ComPtr<MediaStream> stream = m_shutdown ? nullptr : m_stream;
    ReleaseSRWLockShared(&m_lock);
    return stream ? stream->SetMediaType(type) : MF_E_SHUTDOWN;
}

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (object) *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ------------------------------------------------------------------------------------ Activator

Activator::Activator() { MFCreateAttributes(&m_attributes, 4); }

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    AcquireSRWLockExclusive(&m_lock);
    HRESULT hr = S_OK;
    if (!m_source) hr = Microsoft::WRL::MakeAndInitialize<MediaSource>(&m_source);
    if (SUCCEEDED(hr)) hr = m_source.CopyTo(riid, object);
    ReleaseSRWLockExclusive(&m_lock);
    return hr;
}

STDMETHODIMP Activator::ShutdownObject() {
    AcquireSRWLockExclusive(&m_lock);
    ComPtr<MediaSource> source = m_source;
    m_source.Reset();
    ReleaseSRWLockExclusive(&m_lock);
    if (source) source->Shutdown();
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    AcquireSRWLockExclusive(&m_lock);
    m_source.Reset();
    ReleaseSRWLockExclusive(&m_lock);
    return S_OK;
}
