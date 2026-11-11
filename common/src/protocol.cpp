#include "icam/protocol.h"

#include <cstring>

namespace icam::proto {

const char* TypeName(Type t) {
    switch (t) {
        case Type::Hello: return "Hello";
        case Type::Caps: return "Caps";
        case Type::Configure: return "Configure";
        case Type::Start: return "Start";
        case Type::Stop: return "Stop";
        case Type::RequestKeyframe: return "RequestKeyframe";
        case Type::VideoConfig: return "VideoConfig";
        case Type::VideoFrame: return "VideoFrame";
        case Type::Status: return "Status";
        case Type::Ping: return "Ping";
        case Type::Pong: return "Pong";
        case Type::Error: return "Error";
    }
    return "Unknown";
}

uint32_t MaxPayload(Type t) {
    return t == Type::VideoFrame ? kMaxVideoPayload : kMaxControlPayload;
}

namespace {

std::vector<uint8_t> Finish(Type t, const ByteWriter& payload) {
    ByteWriter w;
    w.u32(kMagic);
    w.u16(static_cast<uint16_t>(t));
    w.u16(0);
    w.u32(static_cast<uint32_t>(payload.size()));
    w.bytes(payload.data().data(), payload.size());
    return std::move(w.data());
}

}  // namespace

std::vector<uint8_t> Encode(const Hello& m) {
    ByteWriter w;
    w.u16(m.version);
    w.u16(m.min_version);
    w.u8(static_cast<uint8_t>(m.role));
    w.u8(0);
    w.str(m.name);
    w.str(m.software);
    w.str(m.model);
    return Finish(Type::Hello, w);
}

std::vector<uint8_t> Encode(const Caps& m) {
    ByteWriter w;
    w.u16(static_cast<uint16_t>(m.cameras.size()));
    for (const auto& c : m.cameras) {
        w.str(c.id);
        w.str(c.name);
        w.u8(static_cast<uint8_t>(c.position));
        w.u16(static_cast<uint16_t>(c.formats.size()));
        for (const auto& f : c.formats) {
            w.u16(f.width);
            w.u16(f.height);
            w.u16(f.max_fps);
            w.u8(f.flags);
        }
    }
    return Finish(Type::Caps, w);
}

std::vector<uint8_t> Encode(const Configure& m) {
    ByteWriter w;
    w.str(m.camera_id);
    w.u8(static_cast<uint8_t>(m.position));
    w.u16(m.width);
    w.u16(m.height);
    w.u16(m.fps);
    w.u32(m.bitrate);
    w.u8(m.flags);
    w.u16(m.keyint);
    return Finish(Type::Configure, w);
}

std::vector<uint8_t> Encode(const VideoConfig& m) {
    ByteWriter w;
    w.u32(m.generation);
    w.u8(static_cast<uint8_t>(m.codec));
    w.u16(m.width);
    w.u16(m.height);
    w.u16(m.fps);
    w.u8(m.color);
    w.u32(m.bitrate);
    w.blob16(m.sps);
    w.blob16(m.pps);
    return Finish(Type::VideoConfig, w);
}

std::vector<uint8_t> Encode(const Status& m) {
    ByteWriter w;
    w.u8(static_cast<uint8_t>(m.state));
    w.u8(m.thermal);
    w.u8(m.pressure);
    w.u8(m.battery);
    w.u8(m.charging);
    w.u16(m.fps_x10);
    w.u32(m.bitrate);
    w.u16(m.width);
    w.u16(m.height);
    w.u16(m.max_fps_policy);
    w.str(m.camera_id);
    w.str(m.message);
    return Finish(Type::Status, w);
}

std::vector<uint8_t> Encode(const Ping& m) {
    ByteWriter w;
    w.u32(m.seq);
    w.u64(m.time_us);
    return Finish(Type::Ping, w);
}

std::vector<uint8_t> Encode(const Pong& m) {
    ByteWriter w;
    w.u32(m.seq);
    w.u64(m.echo_time_us);
    w.u64(m.time_us);
    return Finish(Type::Pong, w);
}

std::vector<uint8_t> Encode(const Error& m) {
    ByteWriter w;
    w.u16(static_cast<uint16_t>(m.code));
    w.str(m.message);
    return Finish(Type::Error, w);
}

std::vector<uint8_t> EncodeEmpty(Type t) {
    ByteWriter w;
    return Finish(t, w);
}

std::vector<uint8_t> EncodeVideoFrame(const VideoFrameHeader& h, const uint8_t* au, size_t au_len) {
    ByteWriter w;
    w.u32(kMagic);
    w.u16(static_cast<uint16_t>(Type::VideoFrame));
    w.u16(0);
    w.u32(static_cast<uint32_t>(kVideoFrameHeaderSize + au_len));
    w.u32(h.generation);
    w.u64(h.pts_us);
    w.u8(h.flags);
    w.bytes(au, au_len);
    return std::move(w.data());
}

bool Decode(const std::vector<uint8_t>& p, Hello& m) {
    ByteReader r(p);
    uint8_t role = 0, reserved = 0;
    if (!r.u16(m.version) || !r.u16(m.min_version) || !r.u8(role) || !r.u8(reserved)) return false;
    if (role > 1) return false;
    m.role = static_cast<Role>(role);
    if (!r.str(m.name) || !r.str(m.software)) return false;
    m.model.clear();
    if (r.remaining() > 0 && !r.str(m.model)) return false;
    return true;
}

bool Decode(const std::vector<uint8_t>& p, Caps& m) {
    ByteReader r(p);
    uint16_t ncam = 0;
    if (!r.u16(ncam) || ncam > 16) return false;
    m.cameras.clear();
    m.cameras.reserve(ncam);
    for (uint16_t i = 0; i < ncam; ++i) {
        Camera c;
        uint8_t pos = 0;
        uint16_t nfmt = 0;
        if (!r.str(c.id) || !r.str(c.name) || !r.u8(pos) || !r.u16(nfmt)) return false;
        if (pos > 2 || nfmt > 512) return false;
        c.position = static_cast<Position>(pos);
        c.formats.reserve(nfmt);
        for (uint16_t j = 0; j < nfmt; ++j) {
            Format f;
            if (!r.u16(f.width) || !r.u16(f.height) || !r.u16(f.max_fps) || !r.u8(f.flags)) return false;
            c.formats.push_back(f);
        }
        m.cameras.push_back(std::move(c));
    }
    return true;
}

bool Decode(const std::vector<uint8_t>& p, Configure& m) {
    ByteReader r(p);
    uint8_t pos = 0;
    if (!r.str(m.camera_id) || !r.u8(pos) || !r.u16(m.width) || !r.u16(m.height) || !r.u16(m.fps) ||
        !r.u32(m.bitrate) || !r.u8(m.flags) || !r.u16(m.keyint))
        return false;
    if (pos > 2) return false;
    m.position = static_cast<Position>(pos);
    if (m.width < 2 || m.width > 3840 || m.height < 2 || m.height > 2160 ||
        (m.width & 1) || (m.height & 1) || m.fps == 0 || m.fps > 60 || m.bitrate > 80000000) return false;
    return true;
}

bool Decode(const std::vector<uint8_t>& p, VideoConfig& m) {
    ByteReader r(p);
    uint8_t codec = 0;
    if (!r.u32(m.generation) || !r.u8(codec) || !r.u16(m.width) || !r.u16(m.height) || !r.u16(m.fps) ||
        !r.u8(m.color) || !r.u32(m.bitrate) || !r.blob16(m.sps) || !r.blob16(m.pps))
        return false;
    if (codec != static_cast<uint8_t>(Codec::H264)) return false;
    m.codec = Codec::H264;
    // Sizes must be even (NV12) and within what we support.
    if (m.width == 0 || m.height == 0 || m.width > 4096 || m.height > 4096 || (m.width & 1) || (m.height & 1))
        return false;
    if (m.sps.empty() || m.pps.empty()) return false;
    return true;
}

bool Decode(const std::vector<uint8_t>& p, Status& m) {
    ByteReader r(p);
    uint8_t state = 0;
    if (!r.u8(state) || !r.u8(m.thermal) || !r.u8(m.pressure) || !r.u8(m.battery) || !r.u8(m.charging) ||
        !r.u16(m.fps_x10) || !r.u32(m.bitrate) || !r.u16(m.width) || !r.u16(m.height) ||
        !r.u16(m.max_fps_policy) || !r.str(m.camera_id) || !r.str(m.message))
        return false;
    if (state > static_cast<uint8_t>(DeviceState::Error)) return false;
    m.state = static_cast<DeviceState>(state);
    return true;
}

bool Decode(const std::vector<uint8_t>& p, Ping& m) {
    ByteReader r(p);
    return r.u32(m.seq) && r.u64(m.time_us);
}

bool Decode(const std::vector<uint8_t>& p, Pong& m) {
    ByteReader r(p);
    return r.u32(m.seq) && r.u64(m.echo_time_us) && r.u64(m.time_us) &&
           m.echo_time_us <= INT64_MAX / 10 && m.time_us <= INT64_MAX / 10;
}

bool Decode(const std::vector<uint8_t>& p, Error& m) {
    ByteReader r(p);
    uint16_t code = 0;
    if (!r.u16(code) || !r.str(m.message)) return false;
    m.code = static_cast<ErrorCode>(code);
    return true;
}

bool DecodeVideoFrameHeader(const std::vector<uint8_t>& p, VideoFrameHeader& h) {
    ByteReader r(p);
    return r.u32(h.generation) && r.u64(h.pts_us) && r.u8(h.flags) && h.pts_us <= INT64_MAX / 10;
}

bool ParseHeader(const uint8_t* p, Header& h) {
    ByteReader r(p, kHeaderSize);
    uint32_t magic = 0;
    uint16_t type = 0;
    r.u32(magic);
    r.u16(type);
    r.u16(h.flags);
    r.u32(h.length);
    if (magic != kMagic) return false;
    h.type = static_cast<Type>(type);
    return h.length <= MaxPayload(h.type);
}

bool Framer::Push(const uint8_t* p, size_t n) {
    if (corrupt_) return false;
    // Network readers drain after each <=256 KiB chunk. Bound storage even if a caller fails to drain.
    constexpr size_t limit = kHeaderSize + kMaxVideoPayload + 1024 * 1024;
    if (n > limit || Buffered() > limit - n) { corrupt_ = true; return false; }
    if (n == 0) return true;
    if (!p) { corrupt_ = true; return false; }
    // Compact once the consumed prefix dominates, so the buffer doesn't grow without bound.
    if (off_ > 0 && off_ >= buf_.size() / 2) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(off_));
        off_ = 0;
    }
    buf_.insert(buf_.end(), p, p + n);
    // Validate the pending header as early as possible.
    if (Buffered() >= kHeaderSize) {
        Header h;
        if (!ParseHeader(buf_.data() + off_, h)) {
            corrupt_ = true;
            return false;
        }
    }
    return true;
}

std::optional<Framer::Message> Framer::Pop() {
    if (corrupt_ || Buffered() < kHeaderSize) return std::nullopt;
    Header h;
    if (!ParseHeader(buf_.data() + off_, h)) {
        corrupt_ = true;
        return std::nullopt;
    }
    if (Buffered() < kHeaderSize + h.length) return std::nullopt;
    Message m;
    m.header = h;
    const uint8_t* start = buf_.data() + off_ + kHeaderSize;
    m.payload.assign(start, start + h.length);
    off_ += kHeaderSize + h.length;
    if (off_ == buf_.size()) {
        buf_.clear();
        off_ = 0;
    } else if (Buffered() >= kHeaderSize) {
        Header next;
        if (!ParseHeader(buf_.data() + off_, next)) corrupt_ = true;
    }
    return m;
}

}  // namespace icam::proto
