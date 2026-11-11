// MediaStream: answers RequestSample from a worker thread so Frame Server is never blocked.
//
// Pacing: Frame Server asks for samples as fast as we produce them, so we pace ourselves. Each sample
// is delivered as soon as a *new* frame from the agent is available, but not earlier than 3/4 of a
// frame interval after the previous one (so a 60 fps phone stream feeds a 30 fps consumer at 30 fps),
// and not later than 1.5 intervals (then the previous frame is repeated). A plain fixed-rate timer
// beats against the phone's own 30/60 Hz clock and turns small jitter into repeat+skip pairs.

#include "vcam.h"

#include <cstring>
#include "icam/video_layout.h"

namespace icam::vcam {

namespace {

uint32_t SampleBytes(const VideoFormat& f) {
    return f.subtype == MFVideoFormat_YUY2 ? f.width * f.height * 2 : f.width * f.height * 3 / 2;
}

HRESULT CreateMediaType(const VideoFormat& f, IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, f.subtype);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, SampleBytes(f));
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, f.subtype == MFVideoFormat_YUY2 ? f.width * 2 : f.width);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, f.width, f.height);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, f.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MAX, f.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MIN, f.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_AVG_BITRATE, SampleBytes(f) * 8 * f.fps);
    // The phone encodes SDR BT.709 limited range; say so, so consumers don't guess BT.601.
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    *out = t.Detach();
    return S_OK;
}

void SetStreamAttributes(IMFAttributes* a, DWORD id) {
    a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, id);
    a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
}

void Nv12ToYuy2(const uint8_t* y, const uint8_t* uv, uint32_t w, uint32_t h, uint8_t* dst, uint32_t pitch) {
    for (uint32_t r = 0; r < h; ++r) {
        const uint8_t* yr = y + size_t(r) * w;
        const uint8_t* uvr = uv + size_t(r / 2) * w;
        uint8_t* d = dst + size_t(r) * pitch;
        for (uint32_t c = 0; c < w; c += 2) {
            d[0] = yr[c];
            d[1] = uvr[c];
            d[2] = yr[c + 1];
            d[3] = uvr[c + 1];
            d += 4;
        }
    }
}

}  // namespace

MediaStream::~MediaStream() {
    Shutdown();
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
        else worker_.join();
    }
    if (wake_) CloseHandle(wake_);
    if (timer_) CloseHandle(timer_);
}

HRESULT MediaStream::Initialize(MediaSource* source, DWORD id) {
    source_ = static_cast<IMFMediaSource*>(source);
    id_ = id;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!wake_ || !timer_) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attributes_, 8);
    if (FAILED(hr)) return hr;
    SetStreamAttributes(attributes_.Get(), id_);

    size_t n = 0;
    const VideoFormat* formats = Formats(n);
    std::vector<ComPtr<IMFMediaType>> types(n);
    std::vector<IMFMediaType*> raw(n);
    for (size_t i = 0; i < n; ++i) {
        hr = CreateMediaType(formats[i], &types[i]);
        if (FAILED(hr)) return hr;
        raw[i] = types[i].Get();
    }
    hr = MFCreateStreamDescriptor(id_, static_cast<DWORD>(n), raw.data(), &descriptor_);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = descriptor_->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(raw[0]);
    if (FAILED(hr)) return hr;
    SetStreamAttributes(descriptor_.Get(), id_);
    return S_OK;
}

HRESULT MediaStream::CheckShutdown() const { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, ev);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** ev) {
    ComPtr<IMFMediaEventQueue> q;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
        q = queue_;
    }
    return q->GetEvent(flags, ev);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(met, ext, hr, v);
}

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    *source = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown()) || !source_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** sd) {
    if (!sd) return E_POINTER;
    *sd = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(sd);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    // Stopped: the documented "wrong state" answer (not a fatal error). Paused: keep the request and
    // serve it when the stream runs again.
    if (state_ == MF_STREAM_STATE_STOPPED) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    if (tokens_.size() >= 16) return MF_E_SAMPLEALLOCATOR_EMPTY;  // pathological client; don't grow
    tokens_.emplace_back(token);
    SetEvent(wake_);
    return S_OK;
}

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    if (state == state_) return S_OK;
    switch (state) {
        case MF_STREAM_STATE_PAUSED:
            if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
            state_ = state;
            return S_OK;
        case MF_STREAM_STATE_RUNNING:
            state_ = state;
            SetEvent(wake_);
            return S_OK;
        case MF_STREAM_STATE_STOPPED:
            state_ = state;
            DiscardTokens();
            return S_OK;
        default: return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

STDMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG plen, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    if (plen < sizeof(KSPROPERTY)) return E_INVALIDARG;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

void MediaStream::SetAllocator(IUnknown* allocator) {
    std::lock_guard<std::mutex> lock(mu_);
    allocator_.Reset();
    allocator_ready_ = false;
    if (allocator) allocator->QueryInterface(IID_PPV_ARGS(&allocator_));
}

HRESULT MediaStream::Start(IMFMediaType* type) {
    if (!type) return E_INVALIDARG;
    std::unique_lock<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;

    GUID subtype{};
    UINT32 w = 0, h = 0, num = 0, den = 1;
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    if (subtype != MFVideoFormat_NV12 && subtype != MFVideoFormat_YUY2) return MF_E_INVALIDMEDIATYPE;
    if (w == 0 || h == 0 || w > shm::kMaxWidth || h > shm::kMaxHeight || (w & 1) || (h & 1))
        return MF_E_INVALIDMEDIATYPE;
    uint32_t fps = den ? (num + den / 2) / den : 30;
    if (fps == 0 || fps > 60) fps = 30;
    format_ = {subtype, w, h, fps};
    type_ = type;

    if (allocator_) {
        allocator_->UninitializeSampleAllocator();
        HRESULT hr = allocator_->InitializeSampleAllocator(10, type);
        allocator_ready_ = SUCCEEDED(hr);
        if (FAILED(hr)) Trace("InitializeSampleAllocator failed 0x%08lx; using memory samples", hr);
    }
    if (subtype == MFVideoFormat_YUY2) scratch_.Allocate(w, h);
    DiscardTokens();
    last_delivery_ = 0;
    delivered_ = 0;
    state_ = MF_STREAM_STATE_RUNNING;
    frames_.Start(w, h, fps);
    Trace("Stream start %ux%u@%u %s (allocator %s)", w, h, fps, subtype == MFVideoFormat_NV12 ? "NV12" : "YUY2",
          allocator_ready_ ? "provided" : "memory");

    if (!worker_.joinable()) {
        stop_worker_ = false;
        // A client's token may re-enter Shutdown when released on this worker. Keep the stream
        // alive until Worker returns, even if that callback releases the parent's last reference.
        ComPtr<MediaStream> self(this);
        worker_ = std::thread([self = std::move(self)] { self->Worker(); });
    }
    HRESULT hr = queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    SetEvent(wake_);
    return hr;
}

HRESULT MediaStream::Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
    state_ = MF_STREAM_STATE_STOPPED;
    DiscardTokens();
    frames_.Stop();
    if (allocator_ && allocator_ready_) allocator_->UninitializeSampleAllocator();
    allocator_ready_ = false;
    Trace("Stream stop after %llu samples", delivered_);
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown() {
    ComPtr<IMFMediaSource> released_source;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) return;
        shutdown_ = true;
        state_ = MF_STREAM_STATE_STOPPED;
        DiscardTokens();
        if (queue_) queue_->Shutdown();
        stop_worker_ = true;
        if (wake_) SetEvent(wake_);
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    std::deque<ComPtr<IUnknown>> released;
    {
        std::lock_guard<std::mutex> lock(mu_);
        frames_.Stop();
        if (allocator_ && allocator_ready_) allocator_->UninitializeSampleAllocator();
        allocator_.Reset();
        allocator_ready_ = false;
        released_source = std::move(source_);
        released.swap(graveyard_);
    }
    // `released` drops the client's tokens here, outside our lock.
}

void MediaStream::DiscardTokens() {
    // Called with mu_ held. Client tokens are released later, outside any of our locks: a token's
    // final Release can call back into the pipeline (and into us).
    for (auto& t : tokens_) graveyard_.push_back(std::move(t));
    tokens_.clear();
    if (wake_) SetEvent(wake_);
}

HRESULT MediaStream::CreateOutputSample(ComPtr<IMFSample>& sample, bool& fresh, bool& live) {
    // Called with mu_ held.
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr;
    if (allocator_ && allocator_ready_) {
        hr = allocator_->AllocateSample(&sample);
        if (FAILED(hr)) return hr;
        hr = sample->GetBufferByIndex(0, &buffer);
        if (FAILED(hr)) return hr;
    } else {
        DWORD fourcc = format_.subtype.Data1;  // FOURCC-based subtype GUIDs carry the code in Data1
        hr = MFCreate2DMediaBuffer(format_.width, format_.height, fourcc, FALSE, &buffer);
        if (FAILED(hr)) return hr;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;
        sample->AddBuffer(buffer.Get());
    }

    ComPtr<IMF2DBuffer2> b2;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    hr = buffer.As(&b2);
    if (SUCCEEDED(hr)) hr = b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len);
    if (FAILED(hr)) return hr;
    const uint32_t w = format_.width, h = format_.height;
    const auto base = reinterpret_cast<uintptr_t>(start), scan = reinterpret_cast<uintptr_t>(scan0);
    const size_t offset = scan >= base ? scan - base : SIZE_MAX;
    if (!start || !scan0 || pitch <= 0 || offset > len) {
        // Bottom-up buffers don't happen for YUV; refuse rather than write out of bounds.
        b2->Unlock2D();
        return MF_E_UNEXPECTED;
    }
    if (format_.subtype == MFVideoFormat_NV12) {
        if (!video::RowsFit(len, offset, static_cast<uint32_t>(pitch), w, h)) {
            b2->Unlock2D();
            return MF_E_BUFFERTOOSMALL;
        }
        // The UV plane starts after the *allocated* luma height, which can exceed the visible height
        // (e.g. 1080 lines in a 1088-line surface). Derive it from the locked buffer's size.
        uint64_t plane_h = (uint64_t(len - offset) / uint64_t(pitch)) * 2 / 3;
        if (plane_h < h || plane_h > h + 64) plane_h = h;
        const uint64_t uv_offset = offset + uint64_t(pitch) * plane_h;
        if (uv_offset > len || !video::RowsFit(len, static_cast<size_t>(uv_offset), static_cast<uint32_t>(pitch), w, h / 2)) {
            b2->Unlock2D();
            return MF_E_BUFFERTOOSMALL;
        }
        frames_.Fill(scan0, pitch, scan0 + size_t(pitch) * plane_h, pitch, fresh, live);
    } else {
        if (!video::RowsFit(len, offset, static_cast<uint32_t>(pitch), w * 2, h)) {
            b2->Unlock2D();
            return MF_E_BUFFERTOOSMALL;
        }
        frames_.Fill(scratch_.y(), w, scratch_.uv(), w, fresh, live);
        Nv12ToYuy2(scratch_.y(), scratch_.uv(), w, h, scan0, pitch);
    }
    b2->Unlock2D();
    buffer->SetCurrentLength(SampleBytes(format_));
    return S_OK;
}

HRESULT MediaStream::Deliver(IUnknown* token, bool& fresh_out) {
    // Called with mu_ held.
    ComPtr<IMFSample> sample;
    bool fresh = false, live = false;
    HRESULT hr = CreateOutputSample(sample, fresh, live);
    if (FAILED(hr)) return hr;
    const uint64_t now = MFGetSystemTime();
    sample->SetSampleTime(static_cast<LONGLONG>(now));
    sample->SetSampleDuration(10000000ll / format_.fps);
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    if (SUCCEEDED(hr)) {
        last_delivery_ = now;
        ++delivered_;
        frames_.Heartbeat(!fresh, !live && !fresh);
    }
    fresh_out = fresh;
    return hr;
}

void MediaStream::Worker() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failures = 0;
    for (;;) {
        HANDLE waits[3] = {wake_, timer_, nullptr};
        DWORD nwait = 2;
        std::deque<ComPtr<IUnknown>> released;  // destroyed after the lock below is released
        {
            std::lock_guard<std::mutex> lock(mu_);
            released.swap(graveyard_);
            if (stop_worker_) break;
            HANDLE fr = frames_.FrameReadyEvent();
            if (fr) waits[nwait++] = fr;

            if (state_ == MF_STREAM_STATE_RUNNING && !tokens_.empty()) {
                const uint64_t interval = 10000000ull / format_.fps;
                const uint64_t now = MFGetSystemTime();
                const bool first = last_delivery_ == 0;
                const uint64_t since = first ? interval * 2 : now - last_delivery_;
                const bool fresh_ready = frames_.HasFresh();
                const bool due_fresh = fresh_ready && since >= interval * 3 / 4;
                const bool due_repeat = since >= interval * 3 / 2;
                if (first || due_fresh || due_repeat) {
                    bool fresh = false;
                    HRESULT hr = Deliver(tokens_.front().Get(), fresh);
                    if (SUCCEEDED(hr)) {
                        graveyard_.push_back(std::move(tokens_.front()));
                        tokens_.pop_front();
                        failures = 0;
                        continue;  // re-evaluate immediately for further pending tokens
                    }
                    if (hr == MF_E_SHUTDOWN) break;
                    // The consumer still holds every pooled sample (MF_E_SAMPLEALLOCATOR_EMPTY) or a
                    // transient failure: keep the request and retry shortly. Only a persistent failure
                    // (~2 s) is reported as a stream error.
                    if (++failures == 1 || failures % 100 == 0) Trace("Deliver failed 0x%08lx (x%d)", hr, failures);
                    if (failures >= 200) {
                        queue_->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
                        graveyard_.push_back(std::move(tokens_.front()));
                        tokens_.pop_front();
                        failures = 0;
                        continue;
                    }
                    LARGE_INTEGER retry;
                    retry.QuadPart = -100000;  // 10 ms
                    SetWaitableTimer(timer_, &retry, 0, nullptr, nullptr, FALSE);
                    nwait = 2;  // wait for the retry timer (or a state change), not for frames
                } else {
                    // Wake at the next deadline: early bound if a fresh frame is waiting, else repeat bound.
                    uint64_t target = fresh_ready ? interval * 3 / 4 : interval * 3 / 2;
                    int64_t wait_100ns = static_cast<int64_t>(target) - static_cast<int64_t>(since);
                    if (wait_100ns < 10000) wait_100ns = 10000;  // ≥ 1 ms
                    LARGE_INTEGER due;
                    due.QuadPart = -wait_100ns;  // relative
                    SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
                }
            }
        }
        // Idle or waiting for a deadline. 500 ms cap keeps the heartbeat/agent checks ticking.
        WaitForMultipleObjects(nwait, waits, FALSE, 500);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace icam::vcam
