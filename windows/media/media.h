#pragma once

// Media Foundation H.264 decode (agent) and encode (fakephone). Both synchronous MFTs driven from a
// single thread; no sample queues of their own.

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace icam::media {

bool Startup();   // MFStartup once per process
void Shutdown();

// A decoded NV12 picture, valid only during the callback.
struct Picture {
    const uint8_t* y = nullptr;
    const uint8_t* uv = nullptr;
    uint32_t pitch = 0;   // same for both planes
    uint32_t width = 0;   // display size (cropped to the aperture)
    uint32_t height = 0;
    int64_t pts_100ns = 0;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // prefer_hardware: try DXVA (D3D11) first; falls back to software if unavailable.
    bool Open(bool prefer_hardware);
    void Close();
    // Feeds one Annex-B access unit; calls on_picture for every picture that comes out.
    // Returns false on a decoder error (caller should request a keyframe).
    bool Decode(const uint8_t* au, size_t len, int64_t pts_100ns, const std::function<void(const Picture&)>& on_picture);
    // Drops any buffered state (on reconfiguration or after errors).
    void Flush();
    bool hardware() const;
    std::string description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct EncoderConfig {
    uint32_t width = 1920, height = 1080, fps = 30;
    uint32_t bitrate = 8000000;
    uint32_t gop = 60;
};

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    bool Open(const EncoderConfig& cfg);
    void Close();
    // Encodes one packed NV12 frame. Calls on_au(annexb, len, keyframe) for each produced unit.
    bool Encode(const uint8_t* nv12, int64_t pts_100ns, bool force_key,
                const std::function<void(const uint8_t*, size_t, bool)>& on_au);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace icam::media
