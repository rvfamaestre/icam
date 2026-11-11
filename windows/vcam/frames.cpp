// FrameProvider: the camera's only link to the agent. Reads frames from shared memory, publishes the
// consumer's requested format, and falls back to a locally rendered placeholder when the agent is
// not running. Everything here is bounded and non-blocking.

#include "vcam.h"

#include <cstring>

namespace icam::vcam {

namespace {
constexpr ULONGLONG kAgentStaleMs = 2000;
constexpr ULONGLONG kShmRetryMs = 1000;
}  // namespace

void FrameProvider::EnsureShm() {
    if (shm_ok_) return;
    ULONGLONG now = GetTickCount64();
    if (last_shm_try_ && now - last_shm_try_ < kShmRetryMs) return;
    last_shm_try_ = now;
    shm_ok_ = shm_.Create() || shm_.Open();
    if (shm_ok_) {
        Trace("shared memory ready");
        if (active_) Publish();
    } else {
        Trace("shared memory unavailable (error %lu)", shm_.last_error());
    }
}

void FrameProvider::Start(uint32_t width, uint32_t height, uint32_t fps) {
    std::lock_guard<std::mutex> lock(mu_);
    width_ = width;
    height_ = height;
    fps_ = fps;
    have_frame_ = false;
    current_.Allocate(width, height);
    incoming_.Allocate(width, height);
    placeholder_kind_ = -1;
    if (owner_ == 0) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        owner_ = (static_cast<uint64_t>(GetCurrentProcessId()) << 40) ^ static_cast<uint64_t>(c.QuadPart) ^
                 reinterpret_cast<uintptr_t>(this);
    }
    last_shm_try_ = 0;
    active_ = true;
    if (shm_ok_) Publish();
    else EnsureShm();  // publishes on success
}

void FrameProvider::Publish() {
    shm::Header* h = shm_.header();
    h->req_width = width_;
    h->req_height = height_;
    h->req_fps = fps_;
    h->vcam_pid = GetCurrentProcessId();
    InterlockedExchange64(&h->vcam_heartbeat_ms, static_cast<LONG64>(GetTickCount64()));
    InterlockedExchange64(&h->consumer_owner, static_cast<LONG64>(owner_));
    MemoryBarrier();
    InterlockedExchange(&h->consumer_active, 1);
    InterlockedIncrement(&h->request_seq);
    SetEvent(shm_.control());
    last_frame_ = static_cast<uint64_t>(h->latest_frame);  // only frames written after this point count
}

void FrameProvider::Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) return;  // idempotent (Stop then Shutdown)
    active_ = false;
    // Only withdraw the demand we published: a newer stream (another source instance) may own it now.
    if (shm_ok_ && static_cast<uint64_t>(shm_.header()->consumer_owner) == owner_) {
        shm::Header* h = shm_.header();
        InterlockedExchange(&h->consumer_active, 0);
        InterlockedIncrement(&h->request_seq);
        SetEvent(shm_.control());
    }
    have_frame_ = false;
}

HANDLE FrameProvider::FrameReadyEvent() const { return shm_ok_ ? shm_.frame_ready() : nullptr; }

bool FrameProvider::AgentAlive() const {
    if (!shm_ok_) return false;
    const shm::Header* h = shm_.header();
    LONG64 hb = h->agent_heartbeat_ms;
    return hb != 0 && GetTickCount64() - static_cast<ULONGLONG>(hb) < kAgentStaleMs;
}

bool FrameProvider::HasFresh() {
    std::lock_guard<std::mutex> lock(mu_);
    EnsureShm();
    if (!shm_ok_) return false;
    const shm::Header* h = shm_.header();
    uint64_t n = static_cast<uint64_t>(h->latest_frame);
    if (n == 0 || n == last_frame_) return false;
    const shm::Slot& s = h->slots[n % shm::kSlots];
    return s.width == width_ && s.height == height_;
}

void FrameProvider::Fill(uint8_t* y, uint32_t y_pitch, uint8_t* uv, uint32_t uv_pitch, bool& fresh, bool& live) {
    std::lock_guard<std::mutex> lock(mu_);
    fresh = false;
    live = false;
    EnsureShm();
    const bool agent = AgentAlive();
    if (agent) {
        shm::ReadResult r;
        // Read into a scratch frame and keep it only if the seqlock says it wasn't torn.
        if (shm::ReadLatest(shm_, last_frame_, width_, height_, incoming_.y(), width_, incoming_.uv(), width_, r)) {
            std::swap(current_, incoming_);
            last_frame_ = r.frame_number;
            have_frame_ = true;
            fresh = true;
            live = r.live;
        }
    }
    const image::Nv12* src = nullptr;
    if (agent && have_frame_) {
        src = &current_;
        if (!fresh) live = true;  // repeat of the last frame
    } else {
        // 0: agent not running, 1: agent running but no frame of this size yet.
        int kind = agent ? 1 : 0;
        if (kind != placeholder_kind_ || placeholder_.width != width_ || placeholder_.height != height_) {
            if (kind == 0)
                image::RenderCard(placeholder_, width_, height_, L"iPhone Camera",
                                  L"La aplicación iCam no se está ejecutando en este PC. Ábrela desde el menú Inicio.",
                                  true);  // both normal and reflected text
            else
                image::RenderCard(placeholder_, width_, height_, L"Conectando con el iPhone…",
                                  L"Conecta el iPhone por USB y abre la app iCam.", true);
            placeholder_kind_ = kind;
        }
        src = &placeholder_;
    }
    for (uint32_t r = 0; r < height_; ++r)
        std::memcpy(y + size_t(r) * y_pitch, src->y() + size_t(r) * width_, width_);
    for (uint32_t r = 0; r < height_ / 2; ++r)
        std::memcpy(uv + size_t(r) * uv_pitch, src->uv() + size_t(r) * width_, width_);
}

void FrameProvider::Heartbeat(bool repeated, bool placeholder) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!shm_ok_) return;
    shm::Header* h = shm_.header();
    InterlockedExchange64(&h->vcam_heartbeat_ms, static_cast<LONG64>(GetTickCount64()));
    InterlockedIncrement64(&h->vcam_samples);
    if (repeated) InterlockedIncrement64(&h->vcam_repeats);
    if (placeholder) InterlockedIncrement64(&h->vcam_placeholders);
}

}  // namespace icam::vcam
