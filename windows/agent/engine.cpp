#include "engine.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>

#include "icam/annexb.h"
#include "icam/log.h"
#include "icam/usbmux.h"
#include "media.h"

namespace icam::agent {

namespace {

constexpr int kHelloTimeoutMs = 3000;
constexpr int kSilenceTimeoutMs = 4000;     // device sends Status every second
constexpr uint64_t kStopGraceMs = 2000;     // keep streaming this long after the consumer leaves
constexpr uint64_t kVcamStaleMs = 3000;     // consumer heartbeat older than this = no consumer
constexpr size_t kMaxQueuedFrames = 4;      // ~130 ms at 30 fps; beyond that we drop to the next IDR
constexpr uint64_t kCardAfterMs = 700;      // show a status card once live frames stop this long

uint64_t NowMs() { return GetTickCount64(); }
uint64_t NowUs() { return shm::QpcNow100ns() / 10; }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string HostName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n)) return "PC";
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, s.data(), len, nullptr, nullptr);
    return s;
}

}  // namespace

Engine::Engine(const Options& o, const Settings& s) : opt_(o), settings_(s) {
    snap_.state = shm::AgentState::Unknown;
    snap_.title = L"Iniciando…";
}

Engine::~Engine() { Stop(); }

void Engine::Start() {
    stop_ = false;
    heartbeat_ = std::thread([this] { HeartbeatThread(); });
    decode_ = std::thread([this] { DecodeThread(); });
    session_ = std::thread([this] { SessionThread(); });
}

void Engine::Stop() {
    if (stop_.exchange(true) && !session_.joinable()) return;  // already stopped
    q_cv_.notify_all();
    if (session_.joinable()) session_.join();
    if (decode_.joinable()) decode_.join();
    if (heartbeat_.joinable()) heartbeat_.join();
    std::lock_guard<std::mutex> lock(shm_mu_);
    if (shm_.Valid() && shm_.header()->agent_pid == GetCurrentProcessId()) {
        shm_.header()->agent_heartbeat_ms = 0;  // tell the camera right away that we're gone
        shm_.header()->agent_state = static_cast<uint32_t>(shm::AgentState::Unknown);
    }
    shm_.Close();
}

Snapshot Engine::GetSnapshot() {
    std::lock_guard<std::mutex> lock(snap_mu_);
    return snap_;
}

void Engine::UpdateSettings(const Settings& s) {
    {
        std::lock_guard<std::mutex> lock(settings_mu_);
        if (s.position != settings_.position) position_explicit_ = true;
        settings_ = s;
    }
    settings_dirty_ = true;
}

void Engine::SelectPosition(proto::Position p) {
    {
        std::lock_guard<std::mutex> lock(settings_mu_);
        settings_.position = p;
    }
    position_explicit_ = true;
    settings_dirty_ = true;
}

Settings Engine::GetSettings() {
    std::lock_guard<std::mutex> lock(settings_mu_);
    return settings_;
}

void Engine::Notify() {
    if (on_change) on_change();
}

void Engine::SetState(shm::AgentState st, const std::wstring& title, const std::wstring& detail) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(snap_mu_);
        changed = snap_.state != st || snap_.title != title || snap_.detail != detail;
        snap_.state = st;
        snap_.title = title;
        snap_.detail = detail;
    }
    if (changed) {
        ICAM_INFO("state %u: %s — %s", static_cast<unsigned>(st), log::Utf8(title).c_str(), log::Utf8(detail).c_str());
        Notify();
    }
}

uint64_t Engine::NextFrameNumber() { return ++frame_number_; }

// ------------------------------------------------------------------ session

bool Engine::Send(net::Socket& sock, const std::vector<uint8_t>& msg) { return sock.SendAll(msg.data(), msg.size(), 2000); }

bool Engine::OpenLink(net::Socket& sock) {
    if (!opt_.tcp_host.empty()) {
        if (sock.Connect(opt_.tcp_host, opt_.tcp_port, 1000)) return true;
        SetState(shm::AgentState::AppNotOpen, L"Esperando al teléfono simulado",
                 Widen("No responde " + opt_.tcp_host + ":" + std::to_string(opt_.tcp_port)));
        return false;
    }
    std::vector<usbmux::Device> devices;
    if (!usbmux::ListDevices(devices)) {
        SetState(shm::AgentState::NoAppleService, L"Falta el servicio de dispositivos Apple",
                 L"Instala «Dispositivos Apple» (Microsoft Store) o iTunes y vuelve a abrir iCam.");
        return false;
    }
    // Every USB-attached Apple device is a candidate (an iPad without iCam may be listed first); the
    // phone we last streamed from goes first.
    std::vector<usbmux::Device> usb;
    bool network_only = false;
    for (const auto& d : devices) {
        if (d.connection != "USB") {
            network_only = true;
            continue;
        }
        if (d.serial == connected_serial_) usb.insert(usb.begin(), d);
        else usb.push_back(d);
    }
    if (usb.empty()) {
        const auto presence = usbmux::WindowsUsbPresence();
        if (presence == usbmux::UsbPresence::NotStarted)
            SetState(shm::AgentState::NoDevice, L"El dispositivo USB de Apple está detenido",
                     L"Desconecta el cable del PC, vuelve a conectarlo y desbloquea el iPhone. Si sigue igual, reinicia Windows.");
        else if (presence == usbmux::UsbPresence::Started)
            SetState(shm::AgentState::NoDevice, L"El servicio de Apple no encuentra el iPhone",
                     L"Desconecta y reconecta el cable. Si persiste, reinicia Apple Mobile Device Service o Windows.");
        else if (network_only)
            SetState(shm::AgentState::NoDevice, L"Conecta el iPhone por USB",
                     L"El iPhone solo aparece por red. Conéctalo con un cable USB-C de datos, desbloquéalo y pulsa «Confiar».");
        else
            SetState(shm::AgentState::NoDevice, L"Conecta tu iPhone",
                     L"Usa un cable USB-C de datos. Desbloquéalo y pulsa «Confiar» si lo pide.");
        return false;
    }
    usbmux::ConnectResult last = usbmux::ConnectResult::Refused;
    for (const auto& d : usb) {
        last = usbmux::Connect(d.id, proto::kDevicePort, sock);
        if (last == usbmux::ConnectResult::Ok) {
            connected_serial_ = d.serial;
            return true;
        }
    }
    if (last == usbmux::ConnectResult::Refused)
        SetState(shm::AgentState::AppNotOpen, L"Abre iCam en el iPhone",
                 L"El iPhone está conectado. Abre la app iCam y déjala en primer plano.");
    else
        SetState(shm::AgentState::NoDevice, L"Conectando con el iPhone…", Widen(usbmux::ToString(last)));
    return false;
}

void Engine::SessionThread() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stop_) {
        net::Socket sock;
        if (!OpenLink(sock)) {
            for (int i = 0; i < 10 && !stop_; ++i) Sleep(100);
            continue;
        }
        sock.SetNoDelay();
        sock.SetBuffers(4 * 1024 * 1024, 0);
        ICAM_INFO("link open");
        RunSession(sock);
        sock.Close();
        // Session over: reset streaming state; the decode thread drops its queue.
        streaming_ = false;
        cur_w_ = cur_h_ = cur_fps_ = 0;
        {
            std::lock_guard<std::mutex> lock(q_mu_);
            queue_.clear();
            config_ = proto::VideoConfig{};
        }
        flush_decoder_ = true;
        {
            std::lock_guard<std::mutex> lock(snap_mu_);
            snap_.device.clear();
            snap_.phone = proto::Status{};
        }
        ICAM_INFO("link closed");
        for (int i = 0; i < 5 && !stop_; ++i) Sleep(100);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

void Engine::RunSession(net::Socket& sock) {
    proto::Hello hello;
    hello.role = proto::Role::Host;
    hello.name = HostName();
    hello.software = "icam-agent " ICAM_VERSION;
    if (!Send(sock, proto::Encode(hello))) return;

    proto::Framer framer;
    std::vector<uint8_t> buf(256 * 1024);
    bool got_hello = false;
    const uint64_t t0 = NowMs();
    uint64_t last_rx = t0, last_ping = 0, last_stats = t0;
    uint32_t ping_seq = 0;
    best_rtt_us_ = INT64_MAX;
    SetState(shm::AgentState::Connected, L"iPhone conectado", L"Esperando a que una app abra la cámara.");

    while (!stop_) {
        int n = sock.RecvSome(buf.data(), buf.size(), 20);
        const uint64_t now = NowMs();
        if (n < 0) {
            ICAM_WARN("link read failed");
            return;
        }
        if (n > 0) {
            last_rx = now;
            rx_bytes_ += static_cast<uint32_t>(n);
            if (!framer.Push(buf.data(), size_t(n))) {
                ICAM_ERR("protocol stream corrupt; dropping link");
                return;
            }
            while (auto m = framer.Pop()) {
                if (m->header.type == proto::Type::Hello) {
                    proto::Hello h;
                    if (!proto::Decode(m->payload, h) || h.role != proto::Role::Device) return;
                    if (h.min_version > proto::kVersion || h.version < proto::kVersion) {
                        SetState(shm::AgentState::Connected, L"Versión incompatible",
                                 L"Actualiza la app iCam del iPhone y la del PC a la misma versión.");
                        Send(sock, proto::Encode(proto::Error{proto::ErrorCode::VersionMismatch, "version"}));
                        return;
                    }
                    got_hello = true;
                    std::lock_guard<std::mutex> lock(snap_mu_);
                    snap_.device = h.name + " (" + h.model + ", " + h.software + ")";
                    ICAM_INFO("device: %s", snap_.device.c_str());
                } else if (got_hello) {
                    HandleMessage(sock, *m);
                }
            }
        }
        if (!got_hello && now - t0 > kHelloTimeoutMs) {
            ICAM_WARN("no Hello from device");
            return;
        }
        if (now - last_rx > kSilenceTimeoutMs) {
            ICAM_WARN("device silent for %d ms; reconnecting", kSilenceTimeoutMs);
            return;
        }
        if (now - last_ping >= 1000) {
            last_ping = now;
            if (!Send(sock, proto::Encode(proto::Ping{++ping_seq, NowUs()}))) return;
        }
        if (got_hello) EvaluateDemand(sock);
        if (want_keyframe_.exchange(false) && streaming_) {
            if (!Send(sock, proto::EncodeEmpty(proto::Type::RequestKeyframe))) return;
        }
        if (now - last_stats >= 1000) {
            double secs = (now - last_stats) / 1000.0;
            last_stats = now;
            uint32_t rx = rx_frames_.exchange(0), dec = dec_frames_.exchange(0), bytes = rx_bytes_.exchange(0);
            uint32_t ln = latency_n_.exchange(0);
            int64_t lsum = latency_sum_us_.exchange(0);
            {
                std::lock_guard<std::mutex> lock(snap_mu_);
                snap_.fps_received = rx / secs;
                snap_.fps_decoded = dec / secs;
                snap_.mbps = bytes * 8.0 / 1e6 / secs;
                snap_.latency_ms = ln ? (lsum / 1000.0) / ln : 0;
                snap_.drops = drops_.load();
                snap_.decode_errors = dec_errors_.load();
                snap_.width = cur_w_;
                snap_.height = cur_h_;
                snap_.fps = cur_fps_;
            }
            if (streaming_)
                ICAM_INFO("stats rx %.1f fps dec %.1f fps %.2f Mbps latency %.1f ms drops %u errors %u", rx / secs,
                          dec / secs, bytes * 8.0 / 1e6 / secs, ln ? (lsum / 1000.0) / ln : 0.0, drops_.load(),
                          dec_errors_.load());
            Notify();
        }
    }
}

void Engine::EvaluateDemand(net::Socket& sock) {
    const uint64_t now = NowMs();
    bool consumer = false;
    uint32_t w = 0, h = 0, fps = 0;
    {
        std::lock_guard<std::mutex> lock(shm_mu_);
        if (shm_.Valid()) {
            const shm::Header* hd = shm_.header();
            LONG64 hb = hd->vcam_heartbeat_ms;
            consumer = hd->consumer_active && hb && now - static_cast<uint64_t>(hb) < kVcamStaleMs;
            w = hd->req_width;
            h = hd->req_height;
            fps = hd->req_fps;
        }
    }
    if (!consumer && opt_.always_on) {
        consumer = true;
        w = opt_.always_w;
        h = opt_.always_h;
        fps = opt_.always_fps;
    }
    if (w == 0 || h == 0 || w > shm::kMaxWidth || h > shm::kMaxHeight || (w & 1) || (h & 1)) consumer = false;
    if (fps == 0 || fps > 60) fps = 30;
    {
        std::lock_guard<std::mutex> lock(snap_mu_);
        snap_.consumer = consumer;
    }

    if (consumer) {
        last_demand_ms_ = now;
        out_w_ = w;
        out_h_ = h;
        bool dirty = settings_dirty_.exchange(false);
        if (!streaming_ || w != cur_w_ || h != cur_h_ || fps != cur_fps_ || dirty) {
            Settings s = GetSettings();
            proto::Configure c;
            // The phone keeps its own camera choice (its UI is the main control); the tray only
            // overrides it when the user picks a camera there.
            c.position = position_explicit_.exchange(false) ? s.position : proto::Position::Unspecified;
            c.width = static_cast<uint16_t>(w);
            c.height = static_cast<uint16_t>(h);
            c.fps = static_cast<uint16_t>(fps);
            c.flags = s.mirror ? proto::kCfgMirror : 0;
            ICAM_INFO("requesting %ux%u@%u from the phone (camera: %s)", w, h, fps,
                      c.position == proto::Position::Back ? "back" : c.position == proto::Position::Front ? "front" : "phone's choice");
            if (!Send(sock, proto::Encode(c))) return;
            if (!streaming_ && !Send(sock, proto::EncodeEmpty(proto::Type::Start))) return;
            streaming_ = true;
            start_sent_ms_ = now;
            got_video_config_ = false;
            cur_w_ = w;
            cur_h_ = h;
            cur_fps_ = fps;
        }
        // The phone accepted nothing (camera busy, transient failure): ask again every 5 s, unless
        // it told us it is paused (background/interrupted), which resolves on its side.
        else if (!got_video_config_ && now - start_sent_ms_ > 5000) {
            shm::AgentState st;
            {
                std::lock_guard<std::mutex> lock(snap_mu_);
                st = snap_.state;
            }
            if (st != shm::AgentState::PausedOnPhone) {
                ICAM_WARN("no video %llu ms after Start; asking the phone again", (unsigned long long)(now - start_sent_ms_));
                streaming_ = false;  // next pass re-sends Configure + Start
            }
            start_sent_ms_ = now;
        }
    } else if (streaming_ && now - last_demand_ms_ > kStopGraceMs) {
        ICAM_INFO("no consumer; stopping the phone camera");
        Send(sock, proto::EncodeEmpty(proto::Type::Stop));
        streaming_ = false;
        cur_w_ = cur_h_ = cur_fps_ = 0;
        std::lock_guard<std::mutex> lock(q_mu_);
        queue_.clear();
    }
}

void Engine::HandleMessage(net::Socket& sock, const proto::Framer::Message& m) {
    switch (m.header.type) {
        case proto::Type::Caps: {
            proto::Caps caps;
            if (!proto::Decode(m.payload, caps)) break;
            for (const auto& c : caps.cameras) {
                std::string fm;
                for (const auto& f : c.formats) fm += " " + std::to_string(f.width) + "x" + std::to_string(f.height) + "@" + std::to_string(f.max_fps);
                ICAM_INFO("camera %s (%s, pos %u):%s", c.name.c_str(), c.id.c_str(), unsigned(c.position), fm.c_str());
            }
            caps_ = caps.cameras;
            break;
        }
        case proto::Type::Status: {
            proto::Status st;
            if (!proto::Decode(m.payload, st)) break;
            {
                std::lock_guard<std::mutex> lock(snap_mu_);
                snap_.phone = st;
                for (const auto& c : caps_)
                    if (c.id == st.camera_id) snap_.phone_camera = c.position;
            }
            if (st.state == proto::DeviceState::Background || st.state == proto::DeviceState::Interrupted ||
                st.state == proto::DeviceState::Error) {
                std::wstring msg = st.message.empty() ? L"Vuelve a abrir la app iCam en el iPhone." : Widen(st.message);
                SetState(shm::AgentState::PausedOnPhone, L"Cámara en pausa en el iPhone", msg);
            } else if (streaming_) {
                SetState(shm::AgentState::Streaming, L"Transmitiendo",
                         std::to_wstring(cur_w_) + L"×" + std::to_wstring(cur_h_) + L" @ " + std::to_wstring(cur_fps_) + L" fps");
            } else {
                SetState(shm::AgentState::Connected, L"iPhone conectado", L"Esperando a que una app abra la cámara.");
            }
            break;
        }
        case proto::Type::VideoConfig: {
            proto::VideoConfig vc;
            if (!proto::Decode(m.payload, vc)) {
                ICAM_WARN("bad VideoConfig");
                break;
            }
            ICAM_INFO("video config gen %u %ux%u@%u %.1f Mbps", vc.generation, vc.width, vc.height, vc.fps, vc.bitrate / 1e6);
            generation_ = vc.generation;
            got_video_config_ = true;
            std::lock_guard<std::mutex> lock(q_mu_);
            config_ = vc;
            ++config_rev_;  // a relaunched phone app reuses generation numbers; this never repeats
            queue_.clear();
            break;
        }
        case proto::Type::VideoFrame: {
            proto::VideoFrameHeader h;
            if (!proto::DecodeVideoFrameHeader(m.payload, h) || m.payload.size() <= proto::kVideoFrameHeaderSize) break;
            if (!streaming_ || h.generation != generation_) break;  // stale stream
            ++rx_frames_;
            AccessUnit au;
            au.generation = h.generation;
            au.pts_us = h.pts_us;
            au.key = (h.flags & proto::kFrameKey) != 0;
            au.data.assign(m.payload.begin() + proto::kVideoFrameHeaderSize, m.payload.end());
            {
                std::lock_guard<std::mutex> lock(q_mu_);
                au.config_rev = config_rev_;
                if (queue_.size() >= kMaxQueuedFrames) {
                    // Decoder can't keep up: never let latency build. Keep only a keyframe if one arrives.
                    drops_ += static_cast<uint32_t>(queue_.size());
                    queue_.clear();
                    if (!au.key) {
                        ++drops_;
                        want_keyframe_ = true;
                        flush_decoder_ = true;
                        break;
                    }
                }
                queue_.push_back(std::move(au));
            }
            q_cv_.notify_one();
            break;
        }
        case proto::Type::Pong: {
            proto::Pong p;
            if (!proto::Decode(m.payload, p)) break;
            int64_t now = static_cast<int64_t>(NowUs());
            int64_t rtt = now - static_cast<int64_t>(p.echo_time_us);
            if (rtt < 0 || rtt > 2000000) break;
            // Keep the offset measured with the smallest round trip (least queuing), slowly aging it.
            best_rtt_us_ = best_rtt_us_ == INT64_MAX ? rtt : best_rtt_us_ + 2000;
            if (rtt <= best_rtt_us_) {
                best_rtt_us_ = rtt;
                clock_offset_us_ = static_cast<int64_t>(p.time_us) - (static_cast<int64_t>(p.echo_time_us) + rtt / 2);
                clock_offset_shared_ = clock_offset_us_;
            }
            break;
        }
        case proto::Type::Error: {
            proto::Error e;
            if (proto::Decode(m.payload, e)) {
                ICAM_WARN("device error %u: %s", unsigned(e.code), e.message.c_str());
                if (e.code == proto::ErrorCode::CameraUnavailable || e.code == proto::ErrorCode::PermissionDenied ||
                    e.code == proto::ErrorCode::Thermal)
                    SetState(shm::AgentState::PausedOnPhone, L"La cámara del iPhone no está disponible", Widen(e.message));
            }
            break;
        }
        default: break;
    }
    (void)sock;
}

// ------------------------------------------------------------------ decode

void Engine::DecodeThread() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    media::Startup();
    media::H264Decoder decoder;
    bool open = false;
    bool waiting_key = true;
    uint64_t decoder_rev = 0;
    std::vector<uint8_t> sps, pps, data;
    int consecutive_errors = 0;
    bool prefer_hw = opt_.prefer_hw_decode;

    while (!stop_) {
        AccessUnit au;
        {
            std::unique_lock<std::mutex> lock(q_mu_);
            q_cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            if (flush_decoder_.exchange(false)) {
                waiting_key = true;
                if (open) decoder.Flush();
            }
            if (queue_.empty()) continue;
            au = std::move(queue_.front());
            queue_.pop_front();
            if (au.config_rev != decoder_rev && au.config_rev == config_rev_) {
                sps = config_.sps;
                pps = config_.pps;
                decoder_rev = au.config_rev;
                waiting_key = true;
                if (open) decoder.Flush();
            }
        }
        if (au.config_rev != decoder_rev) continue;  // config superseded before we got to it
        if (waiting_key && !au.key) {
            ++drops_;
            want_keyframe_ = true;
            continue;
        }
        if (!open) {
            open = decoder.Open(prefer_hw);
            if (!open && prefer_hw) {
                ICAM_WARN("hardware decoder unavailable; using software");
                prefer_hw = false;
                open = decoder.Open(false);
            }
            if (!open) {
                ICAM_ERR("cannot open the H.264 decoder");
                Sleep(500);
                continue;
            }
            std::lock_guard<std::mutex> lock(snap_mu_);
            snap_.decoder = decoder.description();
            ICAM_INFO("decoder: %s", snap_.decoder.c_str());
        }
        const uint8_t* p = au.data.data();
        size_t len = au.data.size();
        if (au.key && !annexb::ContainsParameterSets(p, len)) {
            data.clear();
            annexb::AppendNal(data, sps.data(), sps.size());
            annexb::AppendNal(data, pps.data(), pps.size());
            data.insert(data.end(), au.data.begin(), au.data.end());
            p = data.data();
            len = data.size();
        }
        bool ok = decoder.Decode(p, len, static_cast<int64_t>(au.pts_us * 10), [&](const media::Picture& pic) {
            ++dec_frames_;
            PublishPicture(pic.y, pic.uv, pic.pitch, pic.width, pic.height, static_cast<uint64_t>(pic.pts_100ns / 10));
        });
        if (ok) {
            waiting_key = false;
            consecutive_errors = 0;
        } else {
            ++dec_errors_;
            waiting_key = true;
            want_keyframe_ = true;
            if (++consecutive_errors >= 3) {
                // Reopen; after repeated failures in hardware, fall back to software.
                ICAM_WARN("decoder failing; reopening (%s)", prefer_hw ? "switching to software" : "software");
                if (decoder.hardware()) prefer_hw = false;
                decoder.Close();
                open = false;
                consecutive_errors = 0;
            }
        }
    }
    decoder.Close();
    if (SUCCEEDED(co)) CoUninitialize();
}

void Engine::PublishPicture(const uint8_t* y, const uint8_t* uv, uint32_t pitch, uint32_t w, uint32_t h, uint64_t pts_us) {
    const uint32_t ow = out_w_, oh = out_h_;
    if (ow == 0 || oh == 0) return;
    const int64_t offset = clock_offset_shared_;
    const int64_t capture_host_us = static_cast<int64_t>(pts_us) - offset;
    const uint64_t capture_qpc = capture_host_us > 0 ? static_cast<uint64_t>(capture_host_us) * 10 : 0;
    std::lock_guard<std::mutex> lock(shm_mu_);
    if (!shm_.Valid() || !writer_ok_) return;
    static thread_local image::Nv12 scaled;
    bool ok;
    if (w == ow && h == oh) {
        ok = shm::WriteFrame(shm_, NextFrameNumber(), ow, oh, y, pitch, uv, pitch, true, capture_qpc);
    } else {
        if (scaled.width != ow || scaled.height != oh) scaled.Allocate(ow, oh);
        image::ScaleLetterbox(y, pitch, uv, pitch, w, h, scaled.y(), ow, scaled.uv(), ow, ow, oh);
        ok = shm::WriteFrame(shm_, NextFrameNumber(), ow, oh, scaled.y(), ow, scaled.uv(), ow, true, capture_qpc);
    }
    if (ok) {
        last_live_write_ms_ = NowMs();
        if (capture_host_us > 0) {
            int64_t lat = static_cast<int64_t>(NowUs()) - capture_host_us;
            if (lat >= 0 && lat < 5000000) {
                latency_sum_us_ += lat;
                ++latency_n_;
            }
        }
    }
}

// ------------------------------------------------------------------ heartbeat + cards

void Engine::WriteCard() {
    // Called with shm_mu_ held.
    shm::Header* h = shm_.header();
    uint32_t w = h->req_width, ht = h->req_height;
    if (!h->consumer_active || w == 0 || ht == 0 || w > shm::kMaxWidth || ht > shm::kMaxHeight || (w & 1) || (ht & 1))
        return;
    Snapshot s;
    {
        std::lock_guard<std::mutex> lock(snap_mu_);
        s = snap_;
    }
    std::wstring title = s.title, detail = s.detail;
    if (s.state == shm::AgentState::Streaming || s.state == shm::AgentState::Connected) {
        title = L"Iniciando la cámara del iPhone…";
        detail = L"Un momento.";
    }
    image::Nv12 card;
    image::RenderCard(card, w, ht, title, detail, GetSettings().mirror_cards);
    shm::WriteFrame(shm_, NextFrameNumber(), w, ht, card.y(), w, card.uv(), w, false, 0);
}

void Engine::HeartbeatThread() {
    uint64_t last_open_try = 0, last_card = 0;
    while (!stop_) {
        const uint64_t now = NowMs();
        {
            std::lock_guard<std::mutex> lock(shm_mu_);
            if (!shm_.Valid() && now - last_open_try >= 500) {
                last_open_try = now;
                if (shm_.Open()) {
                    frame_number_ = static_cast<uint64_t>(shm_.header()->latest_frame) + 1;
                    ICAM_INFO("virtual camera shared memory opened");
                }
            }
            if (shm_.Valid()) {
                shm::Header* h = shm_.header();
                // One writer only: the ring is machine-wide, but each Windows user session can run an
                // agent. If another live agent owns it, stand by instead of interleaving frames.
                const DWORD me = GetCurrentProcessId();
                const LONG64 hb = h->agent_heartbeat_ms;
                const bool other = h->agent_pid != 0 && h->agent_pid != me && hb && now - static_cast<uint64_t>(hb) < 2000;
                if (other != !writer_ok_) {
                    writer_ok_ = !other;
                    if (other) ICAM_WARN("another iCam agent (pid %u) is feeding the camera; standing by", h->agent_pid);
                }
            }
            if (shm_.Valid() && writer_ok_) {
                shm::Header* h = shm_.header();
                h->agent_pid = GetCurrentProcessId();
                {
                    std::lock_guard<std::mutex> slock(snap_mu_);
                    h->agent_state = static_cast<uint32_t>(snap_.state);
                }
                InterlockedExchange64(&h->agent_heartbeat_ms, static_cast<LONG64>(now));
                // While no live frames flow, keep the consumer's picture informative.
                if (now - last_live_write_ms_ > kCardAfterMs && now - last_card >= 300) {
                    last_card = now;
                    WriteCard();
                }
            }
        }
        // Wake early when the camera signals a new request.
        HANDLE ctl = nullptr;
        {
            std::lock_guard<std::mutex> lock(shm_mu_);
            ctl = shm_.Valid() ? shm_.control() : nullptr;
        }
        if (ctl) WaitForSingleObject(ctl, 200);
        else Sleep(200);
    }
}

}  // namespace icam::agent
