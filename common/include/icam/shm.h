#pragma once

// Shared-memory frame exchange between the agent (user session) and the virtual camera DLL
// (inside the Frame Server service, session 0, LocalService).
//
// The DLL creates the objects in the Global\ namespace (LocalService holds SeCreateGlobalPrivilege;
// a normal user process does not) with a DACL limited to the installed user's SID and the services.
// HKLM\Software\iCam\AgentUserSid is set by the installer. Missing configuration fails closed.
// ICAM_SHM_NAMESPACE=Local uses the current user's SID for isolated in-process tests.
//
// Frames are NV12, tightly packed (pitch == width), in a 3-slot ring guarded by per-slot seqlocks:
// the writer never waits for the reader and the reader never blocks the writer.

#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace icam::shm {

constexpr uint32_t kMagic = 0x4D534349;  // "ICSM"
constexpr uint32_t kVersion = 2;         // 2: consumer_owner, offsets computed locally
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint32_t kSlots = 3;
constexpr uint32_t kMaxWidth = 3840;
constexpr uint32_t kMaxHeight = 2160;
constexpr uint64_t kSlotBytes = (uint64_t(kMaxWidth) * kMaxHeight * 3 / 2 + 4095) & ~uint64_t(4095);
constexpr uint64_t kTotalBytes = kHeaderBytes + kSlots * kSlotBytes;

// What the agent is doing, so the DLL can show a sensible placeholder if the agent stops writing.
enum class AgentState : uint32_t {
    Unknown = 0,
    NoAppleService = 1,  // usbmuxd unreachable
    NoDevice = 2,        // no iPhone attached
    AppNotOpen = 3,      // iPhone attached, app not listening (closed / background / locked)
    Connected = 4,       // session up, not streaming
    Streaming = 5,
    PausedOnPhone = 6,   // phone stopped the camera (background, interruption, thermal)
};

struct Slot {
    volatile LONG64 seq;        // seqlock: odd while being written
    uint64_t frame_number;      // monotonically increasing per agent run
    uint32_t width;
    uint32_t height;
    uint32_t live;              // 1 = camera frame, 0 = status card rendered by the agent
    uint32_t reserved;
    uint64_t capture_qpc_100ns; // phone capture time mapped to this PC's QPC clock (0 if unknown)
    uint64_t write_qpc_100ns;   // when the agent finished writing the slot
    uint64_t data_offset;       // from the start of the mapping
};

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t slot_count;
    uint64_t slot_bytes;
    uint64_t total_bytes;

    // --- written by the virtual camera ---
    volatile LONG consumer_active;  // 1 while a client is streaming from the camera
    volatile LONG request_seq;      // bumped whenever the requested format changes
    uint32_t req_width;
    uint32_t req_height;
    uint32_t req_fps;
    uint32_t vcam_pid;
    volatile LONG64 vcam_heartbeat_ms;  // GetTickCount64 while streaming

    // --- written by the agent ---
    uint32_t agent_pid;
    uint32_t agent_state;               // AgentState
    volatile LONG64 agent_heartbeat_ms; // GetTickCount64, at least every 500 ms
    volatile LONG64 latest_frame;       // frame_number of the newest complete slot (0 = none)

    // --- diagnostics (either side) ---
    volatile LONG64 vcam_samples;       // samples delivered
    volatile LONG64 vcam_repeats;       // samples that repeated the previous frame
    volatile LONG64 vcam_placeholders;  // samples that showed the DLL's own placeholder

    Slot slots[kSlots];

    // --- appended in v2 ---
    volatile LONG64 consumer_owner;     // random id of the camera stream that set consumer_active
};
static_assert(sizeof(Header) <= kHeaderBytes, "header must fit its reserved page");

std::wstring Name(const wchar_t* base);  // L"Global\\ICam.Frames.v1" etc.
inline const wchar_t* kFramesBase = L"ICam.Frames.v3";
inline const wchar_t* kFrameReadyBase = L"ICam.FrameReady.v3";
inline const wchar_t* kControlBase = L"ICam.Control.v3";

// RAII bundle of the mapping and the two events.
class Objects {
public:
    Objects() = default;
    ~Objects() { Close(); }
    Objects(const Objects&) = delete;
    Objects& operator=(const Objects&) = delete;

    // Virtual camera side: create (or open existing) with the cross-session DACL.
    bool Create();
    // Agent side: open existing objects. Fails quietly if the camera hasn't created them yet.
    bool Open();
    void Close();
    bool Valid() const { return header_ != nullptr; }

    Header* header() const { return header_; }
    uint8_t* SlotData(uint32_t i) const;
    HANDLE frame_ready() const { return frame_ready_; }  // agent → camera
    HANDLE control() const { return control_; }          // camera → agent
    DWORD last_error() const { return last_error_; }

private:
    bool Map(HANDLE mapping, bool init);
    HANDLE mapping_ = nullptr;
    HANDLE frame_ready_ = nullptr;
    HANDLE control_ = nullptr;
    Header* header_ = nullptr;
    DWORD last_error_ = 0;
};

// Validates the header's constants. Slot data offsets are never read from shared memory (another
// process could change them); they are computed from these constants.
bool HeaderLooksValid(const Header* h);
constexpr uint64_t SlotOffset(uint32_t i) { return kHeaderBytes + uint64_t(i % kSlots) * kSlotBytes; }

// Writer (agent): copies an NV12 frame into the next slot and publishes it.
// y/uv may have arbitrary pitches; the slot stores them packed.
bool WriteFrame(Objects& o, uint64_t frame_number, uint32_t width, uint32_t height, const uint8_t* y,
                uint32_t y_pitch, const uint8_t* uv, uint32_t uv_pitch, bool live, uint64_t capture_qpc_100ns);

// Reader (camera): copies the newest frame if it is newer than `last_frame` and has the expected
// size. Returns true and updates last_frame/live on success.
struct ReadResult {
    uint64_t frame_number = 0;
    bool live = false;
    uint64_t capture_qpc_100ns = 0;
    uint64_t write_qpc_100ns = 0;
};
bool ReadLatest(const Objects& o, uint64_t last_frame, uint32_t width, uint32_t height, uint8_t* y,
                uint32_t y_pitch, uint8_t* uv, uint32_t uv_pitch, ReadResult& out);

uint64_t QpcNow100ns();  // same timeline as MFGetSystemTime

}  // namespace icam::shm
