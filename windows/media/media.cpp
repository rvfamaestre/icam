#include "media.h"

#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>

#include "icam/log.h"
#include "icam/video_layout.h"

using Microsoft::WRL::ComPtr;

namespace icam::media {

namespace {
std::once_flag g_mf_once;
bool g_mf_ok = false;

HRESULT SetCodecBool(IUnknown* mft, const GUID& key, bool v) {
    ComPtr<ICodecAPI> api;
    HRESULT hr = mft->QueryInterface(IID_PPV_ARGS(&api));
    if (FAILED(hr)) return hr;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    return api->SetValue(&key, &var);
}

HRESULT SetCodecU32(IUnknown* mft, const GUID& key, uint32_t v) {
    ComPtr<ICodecAPI> api;
    HRESULT hr = mft->QueryInterface(IID_PPV_ARGS(&api));
    if (FAILED(hr)) return hr;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    return api->SetValue(&key, &var);
}

HRESULT MakeSample(const uint8_t* data, size_t len, int64_t pts, IMFSample** out) {
    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &buf);
    if (FAILED(hr)) return hr;
    BYTE* p = nullptr;
    hr = buf->Lock(&p, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    std::memcpy(p, data, len);
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(len));
    ComPtr<IMFSample> s;
    hr = MFCreateSample(&s);
    if (FAILED(hr)) return hr;
    s->AddBuffer(buf.Get());
    s->SetSampleTime(pts);
    *out = s.Detach();
    return S_OK;
}
}  // namespace

bool Startup() {
    std::call_once(g_mf_once, [] { g_mf_ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)); });
    return g_mf_ok;
}

void Shutdown() {
    if (g_mf_ok) MFShutdown();
}

// ---------------------------------------------------------------- decoder

struct H264Decoder::Impl {
    ComPtr<IMFTransform> mft;
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    bool hw = false;
    bool provides_samples = false;
    DWORD out_size = 0;
    uint32_t coded_w = 0, coded_h = 0;
    uint32_t disp_x = 0, disp_y = 0, disp_w = 0, disp_h = 0;
    std::vector<uint8_t> packed;  // used when a buffer has no 2D interface

    bool CreateD3D() {
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                       levels, _countof(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if (FAILED(hr)) return false;
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &manager))) return false;
        return SUCCEEDED(manager->ResetDevice(device.Get(), token));
    }

    bool NegotiateOutput() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
            if (FAILED(hr)) return false;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(mft->SetOutputType(0, t.Get(), 0))) return false;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            // SPS-derived dimensions are peer-controlled even when VideoConfig advertised a safe size.
            if (!w || !h || w > 4096 || h > 4096 || (w & 1) || (h & 1)) return false;
            coded_w = w;
            coded_h = h;
            MFVideoArea area{};
            UINT32 sz = 0;
            if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &sz)) &&
                sz == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
                disp_x = static_cast<uint32_t>(area.OffsetX.value);
                disp_y = static_cast<uint32_t>(area.OffsetY.value);
                disp_w = static_cast<uint32_t>(area.Area.cx);
                disp_h = static_cast<uint32_t>(area.Area.cy);
            } else {
                disp_x = disp_y = 0;
                disp_w = w;
                disp_h = h;
            }
            // Clamp the aperture inside the coded picture, even-aligned for NV12.
            if (uint64_t(disp_x) + disp_w > coded_w || uint64_t(disp_y) + disp_h > coded_h) {
                disp_x = disp_y = 0;
                disp_w = coded_w;
                disp_h = coded_h;
            }
            disp_w &= ~1u;
            disp_h &= ~1u;
            disp_x &= ~1u;
            disp_y &= ~1u;
            MFT_OUTPUT_STREAM_INFO info{};
            mft->GetOutputStreamInfo(0, &info);
            provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            out_size = info.cbSize;
            if (out_size > 64 * 1024 * 1024) return false;
            ICAM_INFO("decoder output NV12 coded %ux%u display %ux%u+%u+%u (%s)", coded_w, coded_h, disp_w, disp_h,
                      disp_x, disp_y, hw ? "hardware" : "software");
            return true;
        }
    }

    bool Deliver(IMFSample* sample, const std::function<void(const Picture&)>& cb) {
        if (coded_w == 0 || coded_h == 0) return true;
        LONGLONG pts = 0;
        sample->GetSampleTime(&pts);
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
        ComPtr<IMF2DBuffer2> b2;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        Picture pic;
        pic.width = disp_w;
        pic.height = disp_h;
        pic.pts_100ns = pts;
        // GPU output: the UV plane follows the texture's full height, which can exceed the coded height.
        uint32_t plane_h = coded_h;
        ComPtr<IMFDXGIBuffer> dxgi;
        if (SUCCEEDED(buf.As(&dxgi))) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&tex)))) {
                D3D11_TEXTURE2D_DESC desc{};
                tex->GetDesc(&desc);
                if (desc.Height < coded_h || desc.Height > 4096) return false;
                plane_h = desc.Height;
            }
        }
        BYTE* start = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length))) {
            const auto base = reinterpret_cast<uintptr_t>(start), scan = reinterpret_cast<uintptr_t>(scan0);
            const size_t offset = scan >= base ? scan - base : SIZE_MAX;
            const uint64_t uv_offset = uint64_t(offset) + uint64_t(plane_h) * (pitch > 0 ? pitch : 0);
            const bool valid = start && scan0 && pitch > 0 && offset <= length && uv_offset <= length &&
                video::RowsFit(length, offset, static_cast<uint32_t>(pitch), coded_w, coded_h) &&
                video::RowsFit(length, static_cast<size_t>(uv_offset), static_cast<uint32_t>(pitch), coded_w, coded_h / 2);
            if (!valid) { b2->Unlock2D(); return false; }
            pic.pitch = static_cast<uint32_t>(pitch);
            pic.y = scan0 + size_t(disp_y) * pitch + disp_x;
            pic.uv = scan0 + size_t(plane_h) * pitch + size_t(disp_y / 2) * pitch + disp_x;
            cb(pic);
            b2->Unlock2D();
            return true;
        }
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
        if (len >= size_t(coded_w) * coded_h * 3 / 2) {
            pic.pitch = coded_w;
            pic.y = p + size_t(disp_y) * coded_w + disp_x;
            pic.uv = p + size_t(coded_h) * coded_w + size_t(disp_y / 2) * coded_w + disp_x;
            cb(pic);
        }
        buf->Unlock();
        return true;
    }

    bool Drain(const std::function<void(const Picture&)>& cb) {
        for (int guard = 0; guard < 16; ++guard) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ComPtr<IMFSample> own;
            if (!provides_samples) {
                ComPtr<IMFMediaBuffer> b;
                DWORD size = out_size ? out_size : coded_w * coded_h * 3 / 2;
                if (size == 0) size = 1920 * 1088 * 3 / 2;
                if (FAILED(MFCreateMemoryBuffer(size, &b)) || FAILED(MFCreateSample(&own))) return false;
                own->AddBuffer(b.Get());
                ob.pSample = own.Get();
            }
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (provides_samples && ob.pSample) ob.pSample->Release();
                if (!NegotiateOutput()) return false;
                continue;
            }
            if (FAILED(hr)) {
                if (provides_samples && ob.pSample) ob.pSample->Release();
                ICAM_WARN("decoder ProcessOutput 0x%08lx", hr);
                return false;
            }
            bool ok = ob.pSample ? Deliver(ob.pSample, cb) : true;
            if (provides_samples && ob.pSample) ob.pSample->Release();
            if (!ok) return false;
        }
        return true;
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() { Close(); }

bool H264Decoder::Open(bool prefer_hardware) {
    Close();
    Startup();
    Impl& d = *impl_;
    HRESULT hr = CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d.mft));
    if (FAILED(hr)) {
        ICAM_ERR("H.264 decoder MFT unavailable: 0x%08lx", hr);
        return false;
    }
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(d.mft->GetAttributes(&attrs))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 aware = 0;
        if (prefer_hardware && SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware && d.CreateD3D()) {
            hr = d.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(d.manager.Get()));
            d.hw = SUCCEEDED(hr);
            if (!d.hw) ICAM_WARN("decoder refused the D3D11 manager (0x%08lx); software decode", hr);
        }
    }
    SetCodecBool(d.mft.Get(), CODECAPI_AVLowLatencyMode, true);
    if (!d.hw) {
        d.manager.Reset();
        d.device.Reset();
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = d.mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        ICAM_ERR("decoder SetInputType 0x%08lx", hr);
        return false;
    }
    if (!d.NegotiateOutput()) {
        ICAM_ERR("decoder offers no NV12 output");
        return false;
    }
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void H264Decoder::Close() {
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (impl_->hw) impl_->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    *impl_ = Impl{};
}

void H264Decoder::Flush() {
    if (impl_->mft) impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

bool H264Decoder::hardware() const { return impl_->hw; }

std::string H264Decoder::description() const { return impl_->hw ? "hardware (D3D11/DXVA)" : "software"; }

bool H264Decoder::Decode(const uint8_t* au, size_t len, int64_t pts_100ns,
                         const std::function<void(const Picture&)>& on_picture) {
    Impl& d = *impl_;
    if (!d.mft || len == 0) return false;
    ComPtr<IMFSample> s;
    if (FAILED(MakeSample(au, len, pts_100ns, &s))) return false;
    HRESULT hr = d.mft->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!d.Drain(on_picture)) return false;
        hr = d.mft->ProcessInput(0, s.Get(), 0);
    }
    if (FAILED(hr)) {
        ICAM_WARN("decoder ProcessInput 0x%08lx", hr);
        return false;
    }
    return d.Drain(on_picture);
}

// ---------------------------------------------------------------- encoder

struct H264Encoder::Impl {
    ComPtr<IMFTransform> mft;
    EncoderConfig cfg;
    bool provides_samples = false;
    DWORD out_size = 0;
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() { Close(); }

bool H264Encoder::Open(const EncoderConfig& cfg) {
    Close();
    Startup();
    Impl& e = *impl_;
    e.cfg = cfg;
    HRESULT hr = CoCreateInstance(CLSID_MSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&e.mft));
    if (FAILED(hr)) return false;
    SetCodecU32(e.mft.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    SetCodecU32(e.mft.Get(), CODECAPI_AVEncCommonMeanBitRate, cfg.bitrate);
    SetCodecU32(e.mft.Get(), CODECAPI_AVEncMPVGOPSize, cfg.gop);
    SetCodecU32(e.mft.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SetCodecBool(e.mft.Get(), CODECAPI_AVLowLatencyMode, true);

    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, cfg.bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = e.mft->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr)) {
        ICAM_ERR("encoder SetOutputType 0x%08lx", hr);
        return false;
    }
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
    hr = e.mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        ICAM_ERR("encoder SetInputType 0x%08lx", hr);
        return false;
    }
    MFT_OUTPUT_STREAM_INFO info{};
    e.mft->GetOutputStreamInfo(0, &info);
    e.provides_samples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    e.out_size = info.cbSize ? info.cbSize : cfg.width * cfg.height * 2;
    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void H264Encoder::Close() {
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    *impl_ = Impl{};
}

bool H264Encoder::Encode(const uint8_t* nv12, int64_t pts_100ns, bool force_key,
                         const std::function<void(const uint8_t*, size_t, bool)>& on_au) {
    Impl& e = *impl_;
    if (!e.mft) return false;
    if (force_key) SetCodecU32(e.mft.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    ComPtr<IMFSample> s;
    if (FAILED(MakeSample(nv12, size_t(e.cfg.width) * e.cfg.height * 3 / 2, pts_100ns, &s))) return false;
    s->SetSampleDuration(10000000ll / e.cfg.fps);
    HRESULT hr = e.mft->ProcessInput(0, s.Get(), 0);
    if (FAILED(hr)) return false;
    for (int guard = 0; guard < 8; ++guard) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!e.provides_samples) {
            ComPtr<IMFMediaBuffer> b;
            if (FAILED(MFCreateMemoryBuffer(e.out_size, &b)) || FAILED(MFCreateSample(&own))) return false;
            own->AddBuffer(b.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        hr = e.mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) return false;
        ComPtr<IMFMediaBuffer> buf;
        if (ob.pSample && SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&buf))) {
            BYTE* p = nullptr;
            DWORD len = 0;
            UINT32 key = 0;
            ob.pSample->GetUINT32(MFSampleExtension_CleanPoint, &key);
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                on_au(p, len, key != 0);
                buf->Unlock();
            }
        }
        if (e.provides_samples && ob.pSample) ob.pSample->Release();
    }
    return true;
}

}  // namespace icam::media
