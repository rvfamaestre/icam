// Retain a stream after dropping the caller's parent reference. The stream must keep its parent
// alive until explicit Shutdown, rather than race a dangling raw pointer in GetMediaSource.
#include "vcam.h"
#include <cstdio>
#include <propvarutil.h>

namespace icam::vcam { void Trace(const char*, ...) {} }

class ShutdownToken final : public IUnknown {
public:
    ShutdownToken(IMFMediaSource* parent, HANDLE done) : parent_(parent), done_(done) {}
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
private:
    ~ShutdownToken() { parent_->Shutdown(); SetEvent(done_); }
    std::atomic<ULONG> refs_{1};
    Microsoft::WRL::ComPtr<IMFMediaSource> parent_;
    HANDLE done_;
};

int main() {
    using namespace icam::vcam;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) return 1;
    auto source = Microsoft::WRL::Make<MediaSource>();
    if (!source || FAILED(source->Initialize(nullptr))) return 2;
    ComPtr<IMFPresentationDescriptor> descriptor;
    if (FAILED(source->CreatePresentationDescriptor(&descriptor))) return 3;
    PROPVARIANT position;
    PropVariantInit(&position);
    if (FAILED(source->Start(descriptor.Get(), nullptr, &position))) return 4;
    ComPtr<IMFMediaEvent> event;
    if (FAILED(source->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event))) return 5;
    MediaEventType type{};
    if (FAILED(event->GetType(&type)) || type != MENewStream) return 6;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (FAILED(event->GetValue(&value)) || value.vt != VT_UNKNOWN || !value.punkVal) return 7;
    ComPtr<IMFMediaStream> stream;
    HRESULT hr = value.punkVal->QueryInterface(IID_PPV_ARGS(&stream));
    PropVariantClear(&value);
    if (FAILED(hr)) return 8;
    source.Reset();
    ComPtr<IMFMediaSource> parent;
    if (FAILED(stream->GetMediaSource(&parent)) || !parent) return 9;
    if (FAILED(parent->Shutdown())) return 10;
    parent.Reset();
    if (stream->GetMediaSource(&parent) != MF_E_SHUTDOWN) return 11;
    stream.Reset();
    event.Reset();
    descriptor.Reset();

    // A paused request is discarded to the worker's deferred-release queue. Its last Release
    // re-enters parent Shutdown on that same worker: this must neither self-join nor free `this`.
    auto second = Microsoft::WRL::Make<MediaSource>();
    if (!second || FAILED(second->Initialize(nullptr)) || FAILED(second->CreatePresentationDescriptor(&descriptor)) ||
        FAILED(second->Start(descriptor.Get(), nullptr, &position)) ||
        FAILED(second->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event))) return 12;
    PropVariantInit(&value);
    if (FAILED(event->GetValue(&value)) || value.vt != VT_UNKNOWN || !value.punkVal) return 13;
    hr = value.punkVal->QueryInterface(IID_PPV_ARGS(&stream));
    PropVariantClear(&value);
    ComPtr<IMFMediaStream2> stream2;
    if (FAILED(hr) || FAILED(stream.As(&stream2)) || FAILED(stream2->SetStreamState(MF_STREAM_STATE_PAUSED))) return 14;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return 15;
    auto* token = new ShutdownToken(second.Get(), done);
    hr = stream->RequestSample(token);
    token->Release();
    if (FAILED(hr) || FAILED(second->Stop()) || WaitForSingleObject(done, 5000) != WAIT_OBJECT_0) return 16;
    CloseHandle(done);
    stream2.Reset(); stream.Reset(); second.Reset(); event.Reset(); descriptor.Reset();
    MFShutdown();
    CoUninitialize();
    std::puts("Parent lifetime and reentrant worker Shutdown are safe; closed streams reject access.");
    return 0;
}
