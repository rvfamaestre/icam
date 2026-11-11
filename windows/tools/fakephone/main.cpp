// icam-fakephone: plays the iPhone side of the iCam protocol over plain TCP, streaming an H.264 test
// pattern (Media Foundation software encoder) with a stamp in every frame. Lets the agent, the
// virtual camera and Teams be exercised without a phone.
//
//   icam-fakephone [--port 38780] [--stall-at N --stall-ms MS] [--drop-at N] [--background-at N]
//                  [--max-fps F]
//
// Fault injection (frame numbers count from the first frame of each stream):
//   --stall-at N   stop sending for --stall-ms after frame N (network stall)
//   --drop-at N    close the connection after frame N (unplug)
//   --background-at N  report Background state and stop frames after frame N (app sent to background)

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>

#include "icam/image.h"
#include "icam/log.h"
#include "icam/net.h"
#include "icam/protocol.h"
#include "icam/shm.h"
#include "media.h"

using namespace icam;

namespace {

struct Options {
    // Not proto::kDevicePort (47800): WSL mirrored networking reserves 44620-48715 on Windows.
    uint16_t port = 38780;
    int stall_at = -1, stall_ms = 1500, drop_at = -1, background_at = -1;
    uint32_t max_fps = 60;
};

uint64_t NowUs() { return shm::QpcNow100ns() / 10; }

void ExtractParameterSets(const uint8_t* au, size_t len, proto::VideoConfig& vc) {
    // Walk start codes; take SPS (7) and PPS (8).
    size_t i = 0, start = SIZE_MAX;
    auto flush = [&](size_t end) {
        if (start == SIZE_MAX || end <= start) return;
        while (end > start && au[end - 1] == 0) --end;
        int type = au[start] & 0x1F;
        if (type == 7) vc.sps.assign(au + start, au + end);
        if (type == 8) vc.pps.assign(au + start, au + end);
    };
    while (i + 3 <= len) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            flush(i);
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    flush(len);
}

class Session {
public:
    Session(net::Socket s, const Options& o) : sock_(std::move(s)), opt_(o) {}

    void Run() {
        sock_.SetNoDelay();
        proto::Hello hello;
        hello.role = proto::Role::Device;
        hello.name = "fakephone";
        hello.software = "icam-fakephone " ICAM_VERSION;
        hello.model = "Fake1,1";
        Send(proto::Encode(hello));
        proto::Caps caps;
        for (auto pos : {proto::Position::Back, proto::Position::Front}) {
            proto::Camera c;
            c.id = pos == proto::Position::Back ? "fake-back" : "fake-front";
            c.name = pos == proto::Position::Back ? "Back Camera" : "Front Camera";
            c.position = pos;
            c.formats = {{640, 480, 30, 0}, {1280, 720, 60, 0}, {1920, 1080, 60, 0}, {3840, 2160, 30, 0}};
            caps.cameras.push_back(c);
        }
        Send(proto::Encode(caps));
        SendStatus("Ready");

        uint8_t buf[64 * 1024];
        uint64_t next_frame_us = 0, last_status_us = NowUs();
        while (alive_) {
            int timeout = 20;
            if (streaming_) {
                int64_t wait = (int64_t)(next_frame_us - NowUs()) / 1000;
                timeout = wait < 0 ? 0 : (int)std::min<int64_t>(wait, 20);
            }
            int n = sock_.RecvSome(buf, sizeof(buf), timeout);
            if (n < 0) break;
            if (n > 0) {
                if (!framer_.Push(buf, size_t(n))) break;
                while (auto m = framer_.Pop()) Handle(*m);
            }
            uint64_t now = NowUs();
            if (streaming_ && now >= next_frame_us) {
                if (stall_until_us_ && now < stall_until_us_) {
                    // stalled: produce nothing
                } else {
                    stall_until_us_ = 0;
                    SendFrame();
                }
                next_frame_us += 1000000ull / cfg_.fps;
                if (next_frame_us < now) next_frame_us = now;
            }
            if (start_requested_) {
                start_requested_ = false;
                StartStream();
                next_frame_us = NowUs();
            }
            if (now - last_status_us >= 1000000) {
                last_status_us = now;
                SendStatus(background_ ? "La app está en segundo plano" : "OK");
                fps_count_ = 0;
            }
        }
        std::printf("session ended\n");
    }

private:
    void Send(const std::vector<uint8_t>& m) {
        if (!sock_.SendAll(m.data(), m.size(), 3000)) alive_ = false;
    }

    void SendStatus(const char* msg) {
        proto::Status st;
        st.state = background_ ? proto::DeviceState::Background
                               : (streaming_ ? proto::DeviceState::Streaming : proto::DeviceState::Idle);
        st.thermal = 0;
        st.battery = 100;
        st.charging = 1;
        st.fps_x10 = static_cast<uint16_t>(fps_count_ * 10);
        st.bitrate = cfg_.bitrate;
        st.width = cfg_.width;
        st.height = cfg_.height;
        st.camera_id = cfg_.position == proto::Position::Front ? "fake-front" : "fake-back";
        st.message = msg;
        Send(proto::Encode(st));
    }

    void Handle(const proto::Framer::Message& m) {
        switch (m.header.type) {
            case proto::Type::Hello: {
                proto::Hello h;
                if (proto::Decode(m.payload, h)) std::printf("host: %s (%s)\n", h.name.c_str(), h.software.c_str());
                break;
            }
            case proto::Type::Configure: {
                proto::Configure c;
                if (!proto::Decode(m.payload, c)) {
                    alive_ = false;
                    break;
                }
                c.fps = std::min<uint16_t>(c.fps ? c.fps : 30, static_cast<uint16_t>(opt_.max_fps));
                if (c.bitrate == 0) c.bitrate = c.width * c.height * c.fps / 6;  // ~10 Mbps at 1080p30
                bool changed = c.width != cfg_.width || c.height != cfg_.height || c.fps != cfg_.fps;
                cfg_ = c;
                std::printf("configure %ux%u@%u %s\n", c.width, c.height, c.fps,
                            c.position == proto::Position::Front ? "front" : "back");
                if (streaming_ && changed) start_requested_ = true;
                break;
            }
            case proto::Type::Start: start_requested_ = true; break;
            case proto::Type::Stop:
                streaming_ = false;
                std::printf("stop\n");
                break;
            case proto::Type::RequestKeyframe: force_key_ = true; break;
            case proto::Type::Ping: {
                proto::Ping p;
                if (proto::Decode(m.payload, p)) Send(proto::Encode(proto::Pong{p.seq, p.time_us, NowUs()}));
                break;
            }
            default: break;
        }
    }

    void StartStream() {
        media::EncoderConfig ec{cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate, static_cast<uint32_t>(cfg_.fps * 2)};
        if (!encoder_.Open(ec)) {
            std::printf("encoder open failed for %ux%u\n", cfg_.width, cfg_.height);
            proto::Error e{proto::ErrorCode::BadConfigure, "encoder"};
            Send(proto::Encode(e));
            return;
        }
        frame_.Allocate(cfg_.width, cfg_.height);
        ++generation_;
        sent_config_ = false;
        force_key_ = true;
        streaming_ = true;
        background_ = false;
        stream_frames_ = 0;
        std::printf("start %ux%u@%u gen %u\n", cfg_.width, cfg_.height, cfg_.fps, generation_);
    }

    void SendFrame() {
        if (background_) return;
        if (opt_.drop_at >= 0 && stream_frames_ == opt_.drop_at) {
            std::printf("fault: dropping connection\n");
            alive_ = false;
            return;
        }
        if (opt_.stall_at >= 0 && stream_frames_ == opt_.stall_at && !stalled_once_) {
            stalled_once_ = true;
            stall_until_us_ = NowUs() + uint64_t(opt_.stall_ms) * 1000;
            std::printf("fault: stalling %d ms\n", opt_.stall_ms);
            return;
        }
        if (opt_.background_at >= 0 && stream_frames_ == opt_.background_at) {
            std::printf("fault: background\n");
            background_ = true;
            SendStatus("La app está en segundo plano");
            return;
        }
        image::DrawTestPattern(frame_, counter_);
        image::Stamp st{counter_ & 0xFFFFFF, static_cast<uint32_t>(shm::QpcNow100ns() / 10000)};
        image::DrawStamp(frame_.y(), frame_.width, frame_.width, frame_.height, st);
        ++counter_;
        const uint64_t pts = NowUs();
        bool key = force_key_;
        force_key_ = false;
        encoder_.Encode(frame_.data.data(), int64_t(pts * 10), key, [&](const uint8_t* au, size_t len, bool is_key) {
            if (is_key && !sent_config_) {
                proto::VideoConfig vc;
                vc.generation = generation_;
                vc.width = static_cast<uint16_t>(cfg_.width);
                vc.height = static_cast<uint16_t>(cfg_.height);
                vc.fps = cfg_.fps;
                vc.bitrate = cfg_.bitrate;
                ExtractParameterSets(au, len, vc);
                if (!vc.sps.empty() && !vc.pps.empty()) {
                    Send(proto::Encode(vc));
                    sent_config_ = true;
                }
            }
            if (!sent_config_) return;  // decoder can't start before a keyframe with parameter sets
            proto::VideoFrameHeader h{generation_, pts, static_cast<uint8_t>(is_key ? proto::kFrameKey : 0)};
            Send(proto::EncodeVideoFrame(h, au, len));
        });
        ++stream_frames_;
        ++fps_count_;
    }

    net::Socket sock_;
    Options opt_;
    proto::Framer framer_;
    proto::Configure cfg_;
    media::H264Encoder encoder_;
    image::Nv12 frame_;
    bool alive_ = true, streaming_ = false, start_requested_ = false, force_key_ = false, sent_config_ = false;
    bool background_ = false, stalled_once_ = false;
    uint64_t stall_until_us_ = 0;
    uint32_t generation_ = 0, counter_ = 0, fps_count_ = 0;
    int stream_frames_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::atoi(argv[++i]) : 0; };
        if (a == "--port") o.port = static_cast<uint16_t>(next());
        else if (a == "--stall-at") o.stall_at = next();
        else if (a == "--stall-ms") o.stall_ms = next();
        else if (a == "--drop-at") o.drop_at = next();
        else if (a == "--background-at") o.background_at = next();
        else if (a == "--max-fps") o.max_fps = static_cast<uint32_t>(next());
        else {
            std::printf("unknown option %s\n", a.c_str());
            return 2;
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    media::Startup();
    log::Init(L"", true);
    net::Socket listener;
    // Retry for a while: the bind has been seen to fail transiently right after rebuilds (cause not
    // yet identified; the error code is printed so it can be).
    for (int attempt = 0; attempt < 20 && !listener.Listen("127.0.0.1", o.port); ++attempt) {
        std::printf("bind 127.0.0.1:%u failed (WSA error %d), retrying\n", o.port, listener.last_error());
        std::fflush(stdout);
        Sleep(500);
    }
    if (!listener.Valid()) {
        std::printf("cannot listen on 127.0.0.1:%u (WSA error %d)\n", o.port, listener.last_error());
        return 1;
    }
    std::printf("fakephone listening on 127.0.0.1:%u\n", listener.LocalPort());
    std::fflush(stdout);
    for (;;) {
        net::Socket c = listener.Accept(1000);
        if (!c.Valid()) continue;
        std::printf("host connected\n");
        std::fflush(stdout);
        Session(std::move(c), o).Run();
        std::fflush(stdout);
    }
}
