#pragma once

// The agent's core: finds the iPhone through usbmuxd (or a TCP address for fakephone), runs the
// protocol session, decodes H.264 on its own thread and publishes NV12 frames (or status cards) to
// the virtual camera's shared memory. Streaming is demand-driven: the phone's camera only runs while
// an app has "iPhone Camera" open.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "icam/image.h"
#include "icam/net.h"
#include "icam/protocol.h"
#include "icam/shm.h"

namespace icam::agent {

struct Options {
    std::string tcp_host;      // non-empty: connect here instead of usbmuxd (fakephone)
    uint16_t tcp_port = proto::kDevicePort;
    bool always_on = false;    // stream even without a consumer (tests, measurements)
    uint32_t always_w = 1280, always_h = 720, always_fps = 30;
    bool prefer_hw_decode = true;
};

struct Settings {
    proto::Position position = proto::Position::Front;
    bool mirror = false;
    bool mirror_cards = true;  // normal + reflected text: readable in self-view and for remote participants
};

struct Snapshot {
    shm::AgentState state = shm::AgentState::Unknown;
    std::wstring title;    // one line for the tray / status card
    std::wstring detail;
    std::string device;    // "iPhone (iOS app 0.2.1)"
    bool consumer = false;
    uint32_t width = 0, height = 0, fps = 0;
    double fps_received = 0, fps_decoded = 0, mbps = 0;
    double latency_ms = 0;  // phone capture -> frame published (clock-offset corrected)
    uint32_t drops = 0, decode_errors = 0;
    std::string decoder;
    proto::Status phone;
    proto::Position phone_camera = proto::Position::Unspecified;  // camera the phone is using now
};

class Engine {
public:
    explicit Engine(const Options& o, const Settings& s);
    ~Engine();
    void Start();
    void Stop();
    Snapshot GetSnapshot();
    void UpdateSettings(const Settings& s);
    void SelectPosition(proto::Position p);  // tray pick: always sent to the phone
    Settings GetSettings();
    std::function<void()> on_change;  // called from engine threads; keep it cheap (PostMessage)

private:
    struct AccessUnit {
        uint32_t generation = 0;
        uint64_t config_rev = 0;  // which VideoConfig (agent-side counter) this frame belongs to
        uint64_t pts_us = 0;
        bool key = false;
        std::vector<uint8_t> data;
    };

    void SessionThread();
    void DecodeThread();
    void HeartbeatThread();

    bool OpenLink(net::Socket& sock);
    void RunSession(net::Socket& sock);
    void HandleMessage(net::Socket& sock, const proto::Framer::Message& m);
    void EvaluateDemand(net::Socket& sock);
    bool Send(net::Socket& sock, const std::vector<uint8_t>& msg);
    void SetState(shm::AgentState st, const std::wstring& title, const std::wstring& detail);
    void PublishPicture(const uint8_t* y, const uint8_t* uv, uint32_t pitch, uint32_t w, uint32_t h, uint64_t pts_us);
    void WriteCard();
    uint64_t NextFrameNumber();
    void Notify();

    Options opt_;
    std::atomic<bool> stop_{false};
    std::thread session_, decode_, heartbeat_;

    // settings
    std::mutex settings_mu_;
    Settings settings_;
    std::atomic<bool> settings_dirty_{false};
    std::atomic<bool> position_explicit_{false};  // the user picked a camera in the tray: send it once
    std::vector<proto::Camera> caps_;              // session thread only

    // shared memory (opened by the heartbeat thread, used by all)
    std::mutex shm_mu_;
    shm::Objects shm_;
    std::atomic<uint64_t> frame_number_{0};
    std::atomic<uint64_t> last_live_write_ms_{0};
    bool writer_ok_ = true;  // false while another agent owns the ring (shm_mu_)

    // decode queue
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<AccessUnit> queue_;
    proto::VideoConfig config_;          // latest VideoConfig (guarded by q_mu_)
    uint64_t config_rev_ = 0;            // bumped on every VideoConfig, across sessions (q_mu_)
    std::atomic<bool> want_keyframe_{false};
    std::atomic<bool> flush_decoder_{false};

    // consumer request (read from shm by the session thread)
    uint32_t req_w_ = 0, req_h_ = 0, req_fps_ = 0;
    std::atomic<uint32_t> out_w_{0}, out_h_{0};  // size the decode thread must publish

    // session state
    bool streaming_ = false;
    uint32_t cur_w_ = 0, cur_h_ = 0, cur_fps_ = 0;
    uint64_t last_demand_ms_ = 0;
    uint64_t start_sent_ms_ = 0;        // when Start/Configure went out; retried if no video follows
    bool got_video_config_ = false;
    std::string connected_serial_;      // UDID of the phone we last completed a session with
    uint32_t generation_ = 0;
    int64_t clock_offset_us_ = 0;   // device_us - host_us
    int64_t best_rtt_us_ = INT64_MAX;
    std::atomic<int64_t> clock_offset_shared_{0};

    // snapshot
    std::mutex snap_mu_;
    Snapshot snap_;
    // counters (per stats window)
    std::atomic<uint32_t> rx_frames_{0}, rx_bytes_{0}, dec_frames_{0}, drops_{0}, dec_errors_{0};
    std::atomic<int64_t> latency_sum_us_{0};
    std::atomic<uint32_t> latency_n_{0};
};

}  // namespace icam::agent
