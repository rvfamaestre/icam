#pragma once

// iCam wire protocol v1 (see docs/PROTOCOL.md). One TCP stream between the PC (host) and the
// iPhone app (device), tunnelled through usbmuxd. Every message is a 12-byte header followed by
// `length` payload bytes. All integers are little-endian. Readers ignore trailing payload bytes
// they don't understand, so fields can be appended without a version bump.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "icam/bytes.h"

namespace icam::proto {

constexpr uint32_t kMagic = 0x4D414349;  // "ICAM" as bytes on the wire
constexpr uint16_t kVersion = 1;
constexpr uint16_t kDevicePort = 47800;  // device listens on 127.0.0.1:47800
constexpr size_t kHeaderSize = 12;
constexpr uint32_t kMaxControlPayload = 64 * 1024;
constexpr uint32_t kMaxVideoPayload = 16 * 1024 * 1024;

enum class Type : uint16_t {
    Hello = 0x01,
    Caps = 0x02,
    Configure = 0x03,
    Start = 0x04,
    Stop = 0x05,
    RequestKeyframe = 0x06,
    VideoConfig = 0x10,
    VideoFrame = 0x11,
    Status = 0x20,
    Ping = 0x30,
    Pong = 0x31,
    Error = 0x7F,
};

const char* TypeName(Type t);
uint32_t MaxPayload(Type t);

struct Header {
    Type type{};
    uint16_t flags = 0;
    uint32_t length = 0;
};

enum class Role : uint8_t { Host = 0, Device = 1 };

struct Hello {
    uint16_t version = kVersion;
    uint16_t min_version = kVersion;
    Role role = Role::Host;
    std::string name;       // "Rafa's PC", "iPhone de Rafa"
    std::string software;   // "icam-agent 0.1.0"
    std::string model;      // device only: "iPhone17,3"
};

enum class Position : uint8_t { Unspecified = 0, Back = 1, Front = 2 };

struct Format {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t max_fps = 0;
    uint8_t flags = 0;  // bit0: HDR-capable format (we never enable HDR), bit1: binned
};

struct Camera {
    std::string id;     // AVCaptureDevice.uniqueID
    std::string name;   // "Back Camera", "Front Camera"
    Position position = Position::Unspecified;
    std::vector<Format> formats;  // de-duplicated (w,h) with the best max_fps
};

struct Caps {
    std::vector<Camera> cameras;
};

enum ConfigureFlags : uint8_t {
    kCfgMirror = 1 << 0,
    kCfgStabilization = 1 << 1,  // off by default: adds latency
};

struct Configure {
    std::string camera_id;  // empty: pick by position
    Position position = Position::Front;
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint16_t fps = 30;
    uint32_t bitrate = 0;      // bps; 0 = device default for the size/fps
    uint8_t flags = 0;
    uint16_t keyint = 0;       // frames between IDRs; 0 = 2 s
};

enum class Codec : uint8_t { H264 = 1 };

struct VideoConfig {
    uint32_t generation = 0;  // increments whenever the stream is reconfigured
    Codec codec = Codec::H264;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 0;
    uint8_t color = 1;  // 1 = BT.709 limited (video) range, SDR
    uint32_t bitrate = 0;
    std::vector<uint8_t> sps;  // raw NAL, no start code
    std::vector<uint8_t> pps;
};

enum FrameFlags : uint8_t { kFrameKey = 1 << 0 };

struct VideoFrameHeader {
    uint32_t generation = 0;
    uint64_t pts_us = 0;  // capture time, device monotonic clock
    uint8_t flags = 0;
};
constexpr size_t kVideoFrameHeaderSize = 13;

enum class DeviceState : uint8_t {
    Idle = 0,         // connected, not streaming
    Starting = 1,
    Streaming = 2,
    Background = 3,   // app not in foreground: iOS stopped the camera
    Interrupted = 4,  // camera in use by another app / phone call / multitasking
    Error = 5,
};

struct Status {
    DeviceState state = DeviceState::Idle;
    uint8_t thermal = 0;          // ProcessInfo.ThermalState 0..3
    uint8_t pressure = 0;         // AVCaptureDevice.SystemPressureState.Level 0..4
    uint8_t battery = 255;        // percent, 255 unknown
    uint8_t charging = 0;
    uint16_t fps_x10 = 0;         // frames actually encoded per second ×10
    uint32_t bitrate = 0;         // bps actually produced
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t max_fps_policy = 0;  // fps cap imposed by the thermal policy (0 = none)
    std::string camera_id;
    std::string message;          // human-readable reason (Spanish UI strings come from here)
};

struct Ping {
    uint32_t seq = 0;
    uint64_t time_us = 0;
};

struct Pong {
    uint32_t seq = 0;
    uint64_t echo_time_us = 0;
    uint64_t time_us = 0;
};

enum class ErrorCode : uint16_t {
    Unknown = 0,
    VersionMismatch = 1,
    CameraUnavailable = 2,
    BadConfigure = 3,
    PermissionDenied = 4,
    Thermal = 5,
};

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
};

// --- encoding: each returns a complete message (header + payload) ---
std::vector<uint8_t> Encode(const Hello& m);
std::vector<uint8_t> Encode(const Caps& m);
std::vector<uint8_t> Encode(const Configure& m);
std::vector<uint8_t> Encode(const VideoConfig& m);
std::vector<uint8_t> Encode(const Status& m);
std::vector<uint8_t> Encode(const Ping& m);
std::vector<uint8_t> Encode(const Pong& m);
std::vector<uint8_t> Encode(const Error& m);
std::vector<uint8_t> EncodeEmpty(Type t);  // Start, Stop, RequestKeyframe
std::vector<uint8_t> EncodeVideoFrame(const VideoFrameHeader& h, const uint8_t* au, size_t au_len);

// --- decoding: payload only (header already stripped); false on malformed input ---
bool Decode(const std::vector<uint8_t>& p, Hello& m);
bool Decode(const std::vector<uint8_t>& p, Caps& m);
bool Decode(const std::vector<uint8_t>& p, Configure& m);
bool Decode(const std::vector<uint8_t>& p, VideoConfig& m);
bool Decode(const std::vector<uint8_t>& p, Status& m);
bool Decode(const std::vector<uint8_t>& p, Ping& m);
bool Decode(const std::vector<uint8_t>& p, Pong& m);
bool Decode(const std::vector<uint8_t>& p, Error& m);
bool DecodeVideoFrameHeader(const std::vector<uint8_t>& p, VideoFrameHeader& h);

bool ParseHeader(const uint8_t* p, Header& h);  // validates magic and size limit

// Reassembles messages from an arbitrary chunking of the byte stream.
class Framer {
public:
    struct Message {
        Header header;
        std::vector<uint8_t> payload;
    };
    // Appends bytes. Returns false if the stream is corrupt (bad magic, oversize message); the
    // connection must then be dropped.
    bool Push(const uint8_t* p, size_t n);
    // Pops the next complete message, if any.
    std::optional<Message> Pop();
    size_t Buffered() const { return buf_.size() - off_; }

private:
    std::vector<uint8_t> buf_;
    size_t off_ = 0;
    bool corrupt_ = false;
};

}  // namespace icam::proto
