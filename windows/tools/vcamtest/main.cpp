// icam-vcamtest: a Media Foundation consumer for the virtual camera, the same path real apps use
// (source reader on the device that Frame Server exposes). Measures what an app actually receives.
//
//   icam-vcamtest [--device "iPhone Camera"] [--direct] [--size 1280x720] [--fps 30] [--seconds 10]
//                 [--snapshot out.bmp] [--yuy2]
//
// --direct loads icam-vcam.dll's source in-process (no Frame Server, no admin), for development.
// "new" frames are counted by the stamp fakephone/vcamctl draw into each frame; the stamp also
// carries the sender's QPC time, so on one PC we get the sender → app latency directly.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "icam/image.h"
#include "icam/shm.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::wstring device = L"iPhone Camera";
    bool direct = false;
    uint32_t width = 1280, height = 720, fps = 30;
    int seconds = 10;
    std::wstring snapshot;
    std::wstring dll;
    bool yuy2 = false;
};

constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
const CLSID kClsid = {0x4883e02e, 0x087a, 0x4fe6, {0x84, 0x68, 0x23, 0x39, 0x75, 0x7d, 0x38, 0x64}};

HRESULT OpenSource(const Options& o, ComPtr<IMFMediaSource>& source) {
    if (o.direct) {
        // Load the DLL ourselves so no COM registration (admin) is needed.
        std::wstring dll = o.dll;
        if (dll.empty()) {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring dir(exe);
            dir = dir.substr(0, dir.find_last_of(L'\\'));
            dll = dir + L"\\icam-vcam.dll";
            if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) dll = dir + L"\\..\\..\\vcam\\icam-vcam.dll";
        }
        HMODULE m = LoadLibraryW(dll.c_str());
        if (!m) return HRESULT_FROM_WIN32(GetLastError());
        using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
        auto get = reinterpret_cast<GetClassObjectFn>(GetProcAddress(m, "DllGetClassObject"));
        if (!get) return E_NOINTERFACE;
        ComPtr<IClassFactory> cf;
        HRESULT hr = get(kClsid, IID_PPV_ARGS(&cf));
        if (FAILED(hr)) return hr;
        ComPtr<IMFActivate> act;
        hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&act));
        if (FAILED(hr)) return hr;
        return act->ActivateObject(IID_PPV_ARGS(&source));
    }
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
    if (FAILED(hr)) return hr;
    hr = MF_E_NOT_FOUND;
    for (UINT32 i = 0; i < n; ++i) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        if (hr == MF_E_NOT_FOUND && name && std::wstring(name).find(o.device) != std::wstring::npos) {
            std::printf("device: %ls\n", name);
            hr = devs[i]->ActivateObject(IID_PPV_ARGS(&source));
        }
        CoTaskMemFree(name);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    return hr;
}

bool WriteBmp(const std::wstring& path, const uint8_t* y, const uint8_t* uv, uint32_t w, uint32_t h, uint32_t pitch) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t c = 0; c < w; ++c) {
            int Y = y[size_t(r) * pitch + c] - 16, U = uv[size_t(r / 2) * pitch + (c & ~1u)] - 128,
                V = uv[size_t(r / 2) * pitch + (c & ~1u) + 1] - 128;
            // BT.709 limited range
            int R = (298 * Y + 459 * V + 128) >> 8, G = (298 * Y - 55 * U - 136 * V + 128) >> 8,
                B = (298 * Y + 541 * U + 128) >> 8;
            uint8_t* p = &rgb[(size_t(h - 1 - r) * w + c) * 3];
            p[0] = (uint8_t)std::clamp(B, 0, 255);
            p[1] = (uint8_t)std::clamp(G, 0, 255);
            p[2] = (uint8_t)std::clamp(R, 0, 255);
        }
    }
    uint32_t row = (w * 3 + 3) & ~3u;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)w;
    ih.biHeight = (LONG)h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> pad(row - w * 3, 0);
    for (uint32_t r = 0; r < h; ++r) {
        fwrite(&rgb[size_t(r) * w * 3], 1, size_t(w) * 3, f);
        if (!pad.empty()) fwrite(pad.data(), 1, pad.size(), f);
    }
    fclose(f);
    return true;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))];
}

int Run(const Options& o) {
    ComPtr<IMFMediaSource> source;
    HRESULT hr = OpenSource(o, source);
    if (FAILED(hr)) {
        std::fprintf(stderr, "could not open \"%ls\": 0x%08lx\n", o.device.c_str(), hr);
        return 1;
    }
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 2);
    ra->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader);
    if (FAILED(hr)) return std::fprintf(stderr, "source reader: 0x%08lx\n", hr), 1;

    // Pick the native type matching the request.
    const GUID want = o.yuy2 ? MFVideoFormat_YUY2 : MFVideoFormat_NV12;
    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(kVideo, i, &t))) break;
        GUID sub{};
        UINT32 w = 0, h = 0, num = 0, den = 1;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        if (sub == want && w == o.width && h == o.height && den && num / den == o.fps) chosen = t;
    }
    if (!chosen) return std::fprintf(stderr, "format %ux%u@%u not offered\n", o.width, o.height, o.fps), 1;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(kVideo, TRUE);
    hr = reader->SetCurrentMediaType(kVideo, nullptr, chosen.Get());
    if (FAILED(hr)) return std::fprintf(stderr, "SetCurrentMediaType: 0x%08lx\n", hr), 1;
    std::printf("reading %ux%u@%u %s for %d s\n", o.width, o.height, o.fps, o.yuy2 ? "YUY2" : "NV12", o.seconds);

    const uint64_t t0 = MFGetSystemTime();
    uint64_t samples = 0, stamped = 0, fresh = 0, repeats = 0, errors = 0;
    int64_t last_counter = -1;
    uint64_t last_new_qpc = 0, last_ts = 0;
    std::vector<double> latencies_ms, gaps_ms, content_gaps_ms;
    uint64_t last_hash = 0, distinct = 0, last_distinct_qpc = 0;
    double max_ts_gap = 0;
    bool snap_done = o.snapshot.empty();
    std::vector<uint8_t> nv12(size_t(o.width) * o.height * 3 / 2);
    while (MFGetSystemTime() - t0 < uint64_t(o.seconds) * 10000000ull) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        hr = reader->ReadSample(kVideo, 0, &idx, &flags, &ts, &s);
        if (FAILED(hr)) {
            ++errors;
            std::fprintf(stderr, "ReadSample: 0x%08lx\n", hr);
            if (errors > 5) break;
            continue;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        const uint64_t now_qpc = icam::shm::QpcNow100ns();
        ++samples;
        if (last_ts && ts > (LONGLONG)last_ts) max_ts_gap = std::max(max_ts_gap, (ts - (LONGLONG)last_ts) / 10000.0);
        last_ts = (uint64_t)ts;

        ComPtr<IMFMediaBuffer> buf;
        s->GetBufferByIndex(0, &buf);
        ComPtr<IMF2DBuffer> b2;
        BYTE* p = nullptr;
        LONG pitch = 0;
        bool locked2d = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch));
        DWORD len = 0;
        if (!locked2d) {
            buf->Lock(&p, nullptr, &len);
            pitch = (LONG)(o.yuy2 ? o.width * 2 : o.width);
        }
        // Normalize to packed NV12 for analysis.
        if (o.yuy2) {
            for (uint32_t r = 0; r < o.height; ++r)
                for (uint32_t c = 0; c < o.width; ++c) nv12[size_t(r) * o.width + c] = p[size_t(r) * pitch + c * 2];
            for (uint32_t r = 0; r < o.height / 2; ++r)
                for (uint32_t c = 0; c < o.width; ++c)
                    nv12[size_t(o.width) * o.height + size_t(r) * o.width + c] = p[size_t(r * 2) * pitch + (c / 2) * 4 + 1 + (c & 1) * 2];
        } else {
            for (uint32_t r = 0; r < o.height; ++r) memcpy(&nv12[size_t(r) * o.width], p + size_t(r) * pitch, o.width);
            for (uint32_t r = 0; r < o.height / 2; ++r)
                memcpy(&nv12[size_t(o.width) * o.height + size_t(r) * o.width], p + size_t(o.height + r) * pitch, o.width);
        }
        if (locked2d) b2->Unlock2D();
        else buf->Unlock();

        // Real camera frames carry no stamp: detect repeats by content (a sparse luma grid; sensor
        // noise makes consecutive real frames differ, a repeated sample is bit-identical).
        uint64_t h = 1469598103934665603ull;
        for (uint32_t gy = 0; gy < 32; ++gy)
            for (uint32_t gx = 0; gx < 32; ++gx) {
                h ^= nv12[size_t(gy * o.height / 32) * o.width + gx * o.width / 32];
                h *= 1099511628211ull;
            }
        if (h != last_hash) {
            ++distinct;
            if (last_distinct_qpc) content_gaps_ms.push_back((now_qpc - last_distinct_qpc) / 10000.0);
            last_distinct_qpc = now_qpc;
        }
        last_hash = h;

        icam::image::Stamp st;
        if (icam::image::ReadStamp(nv12.data(), o.width, o.width, o.height, st)) {
            ++stamped;
            if ((int64_t)st.counter != last_counter) {
                ++fresh;
                uint32_t now_ms = (uint32_t)(now_qpc / 10000);
                double lat = (double)(int32_t)(now_ms - st.qpc_ms);
                if (lat >= 0 && lat < 5000) latencies_ms.push_back(lat);
                if (last_new_qpc) gaps_ms.push_back((now_qpc - last_new_qpc) / 10000.0);
                last_new_qpc = now_qpc;
                last_counter = st.counter;
            } else {
                ++repeats;
            }
        }
        if (!snap_done && MFGetSystemTime() - t0 > 20000000ull) {
            snap_done = WriteBmp(o.snapshot, nv12.data(), nv12.data() + size_t(o.width) * o.height, o.width, o.height, o.width);
            std::printf("snapshot written: %ls\n", o.snapshot.c_str());
        }
    }
    const double secs = (MFGetSystemTime() - t0) / 1e7;
    size_t long_gaps = std::count_if(gaps_ms.begin(), gaps_ms.end(), [&](double g) { return g > 1000.0 / o.fps * 1.6; });
    std::printf("samples=%llu (%.2f fps) stamped=%llu new=%llu (%.2f fps) repeats=%llu errors=%llu\n", samples,
                samples / secs, stamped, fresh, fresh / secs, repeats, errors);
    if (!latencies_ms.empty())
        std::printf("latency sender->app ms: avg=%.1f p50=%.1f p95=%.1f max=%.1f\n",
                    [&] { double s = 0; for (double v : latencies_ms) s += v; return s / latencies_ms.size(); }(),
                    Percentile(latencies_ms, 0.5), Percentile(latencies_ms, 0.95), Percentile(latencies_ms, 1.0));
    if (!gaps_ms.empty())
        std::printf("new-frame gaps ms: p50=%.1f p95=%.1f max=%.1f long(>1.6 interval)=%zu\n", Percentile(gaps_ms, 0.5),
                    Percentile(gaps_ms, 0.95), Percentile(gaps_ms, 1.0), long_gaps);
    std::printf("max timestamp gap ms: %.1f\n", max_ts_gap);
    if (!content_gaps_ms.empty()) {
        size_t long_c = std::count_if(content_gaps_ms.begin(), content_gaps_ms.end(),
                                      [&](double g) { return g > 1000.0 / o.fps * 1.6; });
        std::printf("distinct content: %llu (%.2f fps) gaps ms p50=%.1f p95=%.1f max=%.1f long=%zu\n", distinct,
                    distinct / secs, Percentile(content_gaps_ms, 0.5), Percentile(content_gaps_ms, 0.95),
                    Percentile(content_gaps_ms, 1.0), long_c);
    }
    source->Shutdown();
    return errors ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--device") o.device = next();
        else if (a == L"--direct") o.direct = true;
        else if (a == L"--size") {
            std::wstring s = next();
            swscanf_s(s.c_str(), L"%ux%u", &o.width, &o.height);
        } else if (a == L"--fps") o.fps = _wtoi(next().c_str());
        else if (a == L"--seconds") o.seconds = _wtoi(next().c_str());
        else if (a == L"--snapshot") o.snapshot = next();
        else if (a == L"--yuy2") o.yuy2 = true;
        else if (a == L"--dll") o.dll = next();
        else {
            std::printf("unknown option %ls\n", a.c_str());
            return 2;
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    int rc = Run(o);
    MFShutdown();
    CoUninitialize();
    return rc;
}
