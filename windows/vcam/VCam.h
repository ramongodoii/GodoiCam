// Fonte de mídia personalizada para a câmera virtual do Windows 11 (MFCreateVirtualCamera).
// O Frame Server cria o Activator pelo CLSID, pede a fonte e lê amostras do único stream.
#pragma once

// ks.h só declara IKsControl depois que IUnknown e GUID_NULL já existem: fica por último.
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>

#include <deque>
#include <memory>
#include <thread>

#include "FrameSource.h"
#include "GodoiCanShared.h"

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

// Repassa todos os métodos de IMFAttributes para um objeto interno.
#define GDC_FORWARD_ATTRIBUTES(a)                                                                                    \
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return a->GetItem(k, v); }                             \
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return a->GetItemType(k, t); }               \
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return a->CompareItem(k, v, r); }      \
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override {                           \
        return a->Compare(o, m, r);                                                                                   \
    }                                                                                                                 \
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return a->GetUINT32(k, v); }                              \
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return a->GetUINT64(k, v); }                              \
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return a->GetDouble(k, v); }                              \
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return a->GetGUID(k, v); }                                    \
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return a->GetStringLength(k, l); }                  \
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return a->GetString(k, s, n, l); }    \
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override {                                     \
        return a->GetAllocatedString(k, s, l);                                                                        \
    }                                                                                                                 \
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return a->GetBlobSize(k, n); }                          \
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* r) override { return a->GetBlob(k, b, n, r); }        \
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return a->GetAllocatedBlob(k, b, n); }  \
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return a->GetUnknown(k, i, p); }              \
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return a->SetItem(k, v); }                          \
    STDMETHODIMP DeleteItem(REFGUID k) override { return a->DeleteItem(k); }                                          \
    STDMETHODIMP DeleteAllItems() override { return a->DeleteAllItems(); }                                            \
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return a->SetUINT32(k, v); }                               \
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return a->SetUINT64(k, v); }                               \
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return a->SetDouble(k, v); }                               \
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return a->SetGUID(k, v); }                                  \
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return a->SetString(k, v); }                              \
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return a->SetBlob(k, b, n); }                \
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return a->SetUnknown(k, u); }                          \
    STDMETHODIMP LockStore() override { return a->LockStore(); }                                                      \
    STDMETHODIMP UnlockStore() override { return a->UnlockStore(); }                                                  \
    STDMETHODIMP GetCount(UINT32* n) override { return a->GetCount(n); }                                              \
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return a->GetItemByIndex(i, k, v); }    \
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return a->CopyAllItems(d); }

class MediaSource;

class MediaStream : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                        ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
                                        IMFAttributes, IKsControl> {
public:
    MediaStream();
    ~MediaStream();
    HRESULT RuntimeClassInitialize(MediaSource* source);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsMethod(PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsEvent(PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG* r) override;

    GDC_FORWARD_ATTRIBUTES(m_attributes)

    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    HRESULT SetMediaType(IMFMediaType* type);
    void Shutdown();

private:
    void Worker();
    HRESULT DeliverSample(IUnknown* token);
    HRESULT ApplyType(IMFMediaType* type);

    SRWLOCK m_lock = SRWLOCK_INIT;
    ComPtr<IMFMediaSource> m_source;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFStreamDescriptor> m_descriptor;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFMediaType> m_type;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    bool m_shutdown = false;

    UINT32 m_width = 1920;
    UINT32 m_height = 1080;
    UINT32 m_fpsNum = 30;
    UINT32 m_fpsDen = 1;
    GUID m_subtype = MFVideoFormat_NV12;

    std::deque<ComPtr<IUnknown>> m_tokens;  // pedidos de amostra pendentes (token pode ser nulo)
    HANDLE m_wake = nullptr;                // novo pedido, mudança de estado ou desligamento
    std::thread m_worker;
    std::unique_ptr<FrameSource> m_frames;
};

class MediaSource : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                        ChainInterfaces<IMFMediaSource2, IMFMediaSourceEx, IMFMediaSource,
                                                        IMFMediaEventGenerator>,
                                        IMFGetService, IKsControl> {
public:
    MediaSource();
    ~MediaSource();
    HRESULT RuntimeClassInitialize();

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* start) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFMediaSource2
    STDMETHODIMP SetMediaType(DWORD id, IMFMediaType* type) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsMethod(PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsEvent(PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG* r) override;

    IMFStreamDescriptor* Descriptor() { return m_descriptor.Get(); }

private:
    SRWLOCK m_lock = SRWLOCK_INIT;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFPresentationDescriptor> m_pd;
    ComPtr<IMFStreamDescriptor> m_descriptor;
    ComPtr<MediaStream> m_stream;
    bool m_streamActive = false;
    bool m_shutdown = false;
    bool m_mfStarted = false;
};

class __declspec(uuid("6F1C2B7E-4D0A-4E8B-9C33-6A1F0D2E5B71")) Activator
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    Activator();

    STDMETHODIMP ActivateObject(REFIID riid, void** object) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    GDC_FORWARD_ATTRIBUTES(m_attributes)

private:
    SRWLOCK m_lock = SRWLOCK_INIT;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<MediaSource> m_source;
};
