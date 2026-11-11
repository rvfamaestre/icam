// MediaSource: one video stream, live, NV12/YUY2 types. Patterned on Microsoft's VirtualCamera
// sample (SimpleMediaSource) and the MIT-licensed VCamSample; written from scratch for iCam.

#include "vcam.h"

#include <propvarutil.h>

namespace icam::vcam {

namespace {

const VideoFormat kFormats[] = {
    // First entry is the default for apps that don't choose.
    {MFVideoFormat_NV12, 1920, 1080, 30},
    {MFVideoFormat_NV12, 1280, 720, 30},
    {MFVideoFormat_NV12, 640, 480, 30},
    {MFVideoFormat_NV12, 640, 360, 30},
    {MFVideoFormat_NV12, 1920, 1080, 60},
    {MFVideoFormat_NV12, 1280, 720, 60},
    {MFVideoFormat_NV12, 2560, 1440, 30},
    {MFVideoFormat_NV12, 3840, 2160, 30},
    // YUY2 for consumers that won't take NV12 (converted on the CPU).
    {MFVideoFormat_YUY2, 1920, 1080, 30},
    {MFVideoFormat_YUY2, 1280, 720, 30},
    {MFVideoFormat_YUY2, 640, 480, 30},
};

}  // namespace

const VideoFormat* Formats(size_t& count) {
    count = _countof(kFormats);
    return kFormats;
}

MediaSource::~MediaSource() { Shutdown(); }

HRESULT MediaSource::Initialize(IMFAttributes* activate_attributes) {
    std::lock_guard<std::mutex> lock(mu_);
    HRESULT hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    if (activate_attributes) activate_attributes->CopyAllItems(attributes_.Get());

    stream_ = Microsoft::WRL::Make<MediaStream>();
    if (!stream_) return E_OUTOFMEMORY;
    hr = stream_->Initialize(this, 0);
    if (FAILED(hr)) { stream_->Shutdown(); stream_.Reset(); return hr; }
    ComPtr<IMFStreamDescriptor> sd;
    hr = stream_->GetStreamDescriptor(&sd);
    if (FAILED(hr)) { stream_->Shutdown(); stream_.Reset(); return hr; }
    IMFStreamDescriptor* list[] = {sd.Get()};
    hr = MFCreatePresentationDescriptor(1, list, &pd_);
    if (FAILED(hr)) { stream_->Shutdown(); stream_.Reset(); return hr; }
    // Streams start deselected; a camera's only stream is selected by default.
    pd_->SelectStream(0);
    Trace("MediaSource initialized (pid %lu)", GetCurrentProcessId());
    return S_OK;
}

HRESULT MediaSource::CheckShutdown() const { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, ev);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** ev) {
    ComPtr<IMFMediaEventQueue> q;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
        q = queue_;
    }
    return q->GetEvent(flags, ev);  // may block: don't hold the lock
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(met, ext, hr, v);
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
    if (!pd) return E_POINTER;
    *pd = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return pd_->Clone(pd);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Shutdown() {
    ComPtr<MediaStream> stream;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) return S_OK;
        shutdown_ = true;
        if (queue_) queue_->Shutdown();
        stream = std::move(stream_);
        pd_.Reset();
    }
    if (stream) stream->Shutdown();  // joins the worker thread: outside our lock
    Trace("MediaSource shut down");
    return S_OK;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* time_format, const PROPVARIANT* start) {
    if (!pd || !start) return E_INVALIDARG;
    if (time_format && *time_format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;

    DWORD count = 0;
    HRESULT hr = pd->GetStreamDescriptorCount(&count);
    if (FAILED(hr)) return hr;
    if (count != 1) return MF_E_UNSUPPORTED_REPRESENTATION;
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (FAILED(hr)) return hr;
    DWORD id = 0;
    sd->GetStreamIdentifier(&id);
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;

    BOOL was_selected = FALSE;
    ComPtr<IMFStreamDescriptor> ours;
    pd_->GetStreamDescriptorByIndex(0, &was_selected, &ours);

    if (selected) {
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        hr = sd->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
        if (FAILED(hr)) return hr;
        pd_->SelectStream(0);
        // Start the stream before announcing it: a consumer reacting to MENewStream may call
        // RequestSample right away, from another thread.
        hr = stream_->Start(type.Get());
        if (FAILED(hr)) return hr;
        ComPtr<IUnknown> unk;
        stream_.As(&unk);
        hr = queue_->QueueEventParamUnk(started_ && was_selected ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                        unk.Get());
        if (FAILED(hr)) return hr;
    } else if (was_selected) {
        pd_->DeselectStream(0);
        stream_->Stop();
    }

    PROPVARIANT t;
    InitPropVariantFromInt64(MFGetSystemTime(), &t);
    hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &t);
    PropVariantClear(&t);
    started_ = true;
    return hr;
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    if (!started_) return MF_E_INVALID_STATE_TRANSITION;
    started_ = false;
    stream_->Stop();
    // Keep the stream selected in our descriptor: a consumer may restart from a fresh clone of it.
    PROPVARIANT t;
    InitPropVariantFromInt64(MFGetSystemTime(), &t);
    HRESULT hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &t);
    PropVariantClear(&t);
    return hr;
}

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
    IMFAttributes* a = stream_->Attributes();
    a->AddRef();
    *attributes = a;
    return S_OK;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
    // We always fill system-memory or allocator-provided samples on the CPU.
    return E_NOTIMPL;
}

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG plen, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    if (plen < sizeof(KSPROPERTY)) return E_INVALIDARG;
    // Same answer a KS driver gives for property sets it doesn't implement.
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD stream_id, IUnknown* allocator) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
    stream_->SetAllocator(allocator);
    return S_OK;
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD stream_id, DWORD* input_stream_id, MFSampleAllocatorUsage* usage) {
    if (!input_stream_id || !usage) return E_POINTER;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
    *input_stream_id = stream_id;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

}  // namespace icam::vcam
