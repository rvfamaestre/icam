#pragma once

// iCam virtual camera: a Media Foundation media source loaded by the Windows Camera Frame Server
// (and registered with MFCreateVirtualCamera). It never touches USB, the network or a decoder: it
// copies NV12 frames that the agent publishes in shared memory (icam/shm.h), or shows a placeholder.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/module.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "icam/image.h"
#include "icam/shm.h"

namespace icam::vcam {

using Microsoft::WRL::ComPtr;

// {4883E02E-087A-4FE6-8468-2339757D3864}
constexpr GUID kClsid = {0x4883e02e, 0x087a, 0x4fe6, {0x84, 0x68, 0x23, 0x39, 0x75, 0x7d, 0x38, 0x64}};
constexpr wchar_t kClsidString[] = L"{4883E02E-087A-4FE6-8468-2339757D3864}";
constexpr wchar_t kFriendlyName[] = L"iPhone Camera";

void Trace(const char* fmt, ...);

struct VideoFormat {
    GUID subtype;
    uint32_t width, height, fps;
};

class MediaSource;

// Supplies frames for the stream: live frames from shared memory, the agent's status cards, or our
// own placeholder when no agent is running. Never blocks.
class FrameProvider {
public:
    void Start(uint32_t width, uint32_t height, uint32_t fps);
    void Stop();
    HANDLE FrameReadyEvent() const;
    bool HasFresh();
    // Fills NV12 (y/uv planes with pitches) with the best available frame.
    // fresh: a new frame from the agent; live: a camera frame (not a card).
    void Fill(uint8_t* y, uint32_t y_pitch, uint8_t* uv, uint32_t uv_pitch, bool& fresh, bool& live);
    void Heartbeat(bool repeated, bool placeholder);

private:
    bool AgentAlive() const;
    void EnsureShm();
    void Publish();  // mu_ held, shm ready
    bool active_ = false;

    std::mutex mu_;
    shm::Objects shm_;
    bool shm_ok_ = false;
    ULONGLONG last_shm_try_ = 0;
    uint32_t width_ = 0, height_ = 0, fps_ = 30;
    uint64_t last_frame_ = 0;
    bool have_frame_ = false;   // `current_` holds a frame from the agent at the current size
    image::Nv12 current_;
    image::Nv12 incoming_;   // ReadLatest target; swapped into current_ only when not torn
    uint64_t owner_ = 0;     // identifies this stream's demand in shared memory
    image::Nv12 placeholder_;
    int placeholder_kind_ = -1;
};

class MediaStream : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                        Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>, IKsControl> {
public:
    HRESULT Initialize(MediaSource* source, DWORD id);
    ~MediaStream();

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override;
    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;
    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;
    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY p, ULONG plen, LPVOID data, ULONG dlen, ULONG* ret) override;
    STDMETHODIMP KsMethod(PKSMETHOD m, ULONG mlen, LPVOID data, ULONG dlen, ULONG* ret) override;
    STDMETHODIMP KsEvent(PKSEVENT e, ULONG elen, LPVOID data, ULONG dlen, ULONG* ret) override;

    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();
    void SetAllocator(IUnknown* allocator);
    IMFAttributes* Attributes() const { return attributes_.Get(); }

private:
    HRESULT CheckShutdown() const;
    void Worker();
    HRESULT Deliver(IUnknown* token, bool& fresh_out);
    HRESULT CreateOutputSample(ComPtr<IMFSample>& sample, bool& fresh, bool& live);

    std::mutex mu_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    ComPtr<IMFMediaType> type_;
    ComPtr<IMFMediaSource> source_;  // keep parent alive until Shutdown explicitly breaks the cycle
    DWORD id_ = 0;
    bool shutdown_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    VideoFormat format_{};
    bool allocator_ready_ = false;

    std::deque<ComPtr<IUnknown>> tokens_;
    std::deque<ComPtr<IUnknown>> graveyard_;  // tokens to release outside our lock (worker/Shutdown)
    void DiscardTokens();
    HANDLE wake_ = nullptr;
    HANDLE timer_ = nullptr;
    std::thread worker_;
    std::atomic<bool> stop_worker_{false};
    FrameProvider frames_;
    image::Nv12 scratch_;  // NV12 staging for YUY2 output
    uint64_t last_delivery_ = 0;  // QPC 100 ns
    uint64_t delivered_ = 0;
};

class MediaSource : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                        Microsoft::WRL::ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                                                        IMFGetService, IKsControl, IMFSampleAllocatorControl> {
public:
    HRESULT Initialize(IMFAttributes* activate_attributes);
    ~MediaSource();

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override;
    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* time_format, const PROPVARIANT* start) override;
    STDMETHODIMP Stop() override;
    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;
    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;
    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY p, ULONG plen, LPVOID data, ULONG dlen, ULONG* ret) override;
    STDMETHODIMP KsMethod(PKSMETHOD m, ULONG mlen, LPVOID data, ULONG dlen, ULONG* ret) override;
    STDMETHODIMP KsEvent(PKSEVENT e, ULONG elen, LPVOID data, ULONG dlen, ULONG* ret) override;
    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD stream_id, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD stream_id, DWORD* input_stream_id, MFSampleAllocatorUsage* usage) override;

private:
    HRESULT CheckShutdown() const;

    std::mutex mu_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFPresentationDescriptor> pd_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<MediaStream> stream_;
    bool shutdown_ = false;
    bool started_ = false;
};

class __declspec(uuid("4883E02E-087A-4FE6-8468-2339757D3864")) Activator
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    Activator();

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IMFAttributes (delegated to an inner attribute store)
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return attrs_->Compare(o, t, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return attrs_->GetStringLength(k, l); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return attrs_->GetString(k, s, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return attrs_->GetAllocatedString(k, s, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return attrs_->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* l) override { return attrs_->GetBlob(k, b, n, l); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return attrs_->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* p) override { return attrs_->GetUnknown(k, r, p); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attrs_->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attrs_->SetUnknown(k, u); }
    STDMETHODIMP LockStore() override { return attrs_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return attrs_->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attrs_->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* dst) override { return attrs_->CopyAllItems(dst); }

private:
    ComPtr<IMFAttributes> attrs_;
    ComPtr<MediaSource> source_;
    std::mutex mu_;
};

// The formats we advertise, best default first.
const VideoFormat* Formats(size_t& count);

}  // namespace icam::vcam
