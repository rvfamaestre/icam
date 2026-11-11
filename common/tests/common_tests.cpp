// Unit tests for the platform-neutral core: protocol codec + framer, plist, usbmux client against
// a fake usbmuxd, shared-memory ring, image helpers and Annex-B parsing.

#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>
#include <aclapi.h>
#include <sddl.h>

#include "check.h"
#include "icam/annexb.h"
#include "icam/bytes.h"
#include "icam/image.h"
#include "icam/net.h"
#include "icam/plist.h"
#include "icam/protocol.h"
#include "icam/shm.h"
#include "icam/usbmux.h"
#include "icam/video_layout.h"

using namespace icam;

namespace {

proto::Framer::Message RoundTrip(const std::vector<uint8_t>& wire) {
    proto::Framer f;
    REQUIRE(f.Push(wire.data(), wire.size()));
    auto m = f.Pop();
    REQUIRE(m.has_value());
    REQUIRE(!f.Pop().has_value());
    return *m;
}

std::string Hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

// The golden vectors shared with the Swift tests (docs/protocol-vectors-v1.txt).
std::vector<std::pair<std::string, std::vector<uint8_t>>> GoldenMessages() {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> v;
    proto::Hello h;
    h.role = proto::Role::Device;
    h.name = "iPhone de Rafa";
    h.software = "icam-ios 0.1.0";
    h.model = "iPhone17,3";
    v.push_back({"hello_device", proto::Encode(h)});
    proto::Caps c;
    proto::Camera cam;
    cam.id = "com.apple.avfoundation.avcapturedevice.built-in_video:1";
    cam.name = "Front Camera";
    cam.position = proto::Position::Front;
    cam.formats = {{1920, 1080, 60, 0}, {3840, 2160, 60, 1}};
    c.cameras.push_back(cam);
    v.push_back({"caps", proto::Encode(c)});
    proto::Configure cf;
    cf.position = proto::Position::Back;
    cf.width = 1280;
    cf.height = 720;
    cf.fps = 30;
    cf.bitrate = 6000000;
    cf.flags = proto::kCfgMirror;
    cf.keyint = 60;
    v.push_back({"configure", proto::Encode(cf)});
    v.push_back({"start", proto::EncodeEmpty(proto::Type::Start)});
    v.push_back({"stop", proto::EncodeEmpty(proto::Type::Stop)});
    v.push_back({"request_keyframe", proto::EncodeEmpty(proto::Type::RequestKeyframe)});
    proto::VideoConfig vc;
    vc.generation = 7;
    vc.width = 1920;
    vc.height = 1080;
    vc.fps = 30;
    vc.bitrate = 12000000;
    vc.sps = {0x67, 0x64, 0x00, 0x28};
    vc.pps = {0x68, 0xEE, 0x3C, 0x80};
    v.push_back({"video_config", proto::Encode(vc)});
    proto::VideoFrameHeader fh{7, 123456789012ull, proto::kFrameKey};
    const uint8_t au[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
    v.push_back({"video_frame", proto::EncodeVideoFrame(fh, au, sizeof(au))});
    proto::Status st;
    st.state = proto::DeviceState::Streaming;
    st.thermal = 1;
    st.pressure = 1;
    st.battery = 87;
    st.charging = 1;
    st.fps_x10 = 299;
    st.bitrate = 11800000;
    st.width = 1920;
    st.height = 1080;
    st.max_fps_policy = 30;
    st.camera_id = "front";
    st.message = "OK";
    v.push_back({"status", proto::Encode(st)});
    v.push_back({"ping", proto::Encode(proto::Ping{42, 1000})});
    v.push_back({"pong", proto::Encode(proto::Pong{42, 1000, 2000})});
    v.push_back({"error", proto::Encode(proto::Error{proto::ErrorCode::CameraUnavailable, "busy"})});
    return v;
}

}  // namespace

TEST(protocol_hello_roundtrip) {
    proto::Hello h;
    h.role = proto::Role::Device;
    h.name = "iPhone de Rafa";
    h.software = "icam-ios 0.1.0";
    h.model = "iPhone17,3";
    auto m = RoundTrip(proto::Encode(h));
    CHECK(m.header.type == proto::Type::Hello);
    proto::Hello d;
    REQUIRE(proto::Decode(m.payload, d));
    CHECK(d.role == proto::Role::Device);
    CHECK(d.name == h.name);
    CHECK(d.software == h.software);
    CHECK(d.model == h.model);
    CHECK(d.version == proto::kVersion);
}

TEST(protocol_caps_roundtrip) {
    proto::Caps c;
    for (int i = 0; i < 3; ++i) {
        proto::Camera cam;
        cam.id = "cam" + std::to_string(i);
        cam.name = "Camera " + std::to_string(i);
        cam.position = i == 2 ? proto::Position::Front : proto::Position::Back;
        cam.formats = {{1280, 720, 60, 0}, {1920, 1080, 60, 0}, {3840, 2160, 60, 1}};
        c.cameras.push_back(cam);
    }
    auto m = RoundTrip(proto::Encode(c));
    proto::Caps d;
    REQUIRE(proto::Decode(m.payload, d));
    REQUIRE(d.cameras.size() == 3);
    CHECK(d.cameras[2].position == proto::Position::Front);
    CHECK(d.cameras[1].formats[2].width == 3840);
    CHECK(d.cameras[1].formats[2].flags == 1);
}

TEST(protocol_video_frame_roundtrip) {
    std::vector<uint8_t> au(5000);
    for (size_t i = 0; i < au.size(); ++i) au[i] = uint8_t(i * 7);
    proto::VideoFrameHeader h{3, 987654321ull, proto::kFrameKey};
    auto m = RoundTrip(proto::EncodeVideoFrame(h, au.data(), au.size()));
    CHECK(m.header.type == proto::Type::VideoFrame);
    proto::VideoFrameHeader d;
    REQUIRE(proto::DecodeVideoFrameHeader(m.payload, d));
    CHECK(d.generation == 3);
    CHECK(d.pts_us == 987654321ull);
    CHECK(d.flags == proto::kFrameKey);
    CHECK(std::memcmp(m.payload.data() + proto::kVideoFrameHeaderSize, au.data(), au.size()) == 0);
}

TEST(protocol_video_config_validation) {
    proto::VideoConfig vc;
    vc.width = 1921;  // odd: invalid for NV12
    vc.height = 1080;
    vc.sps = {0x67};
    vc.pps = {0x68};
    auto m = RoundTrip(proto::Encode(vc));
    proto::VideoConfig d;
    CHECK(!proto::Decode(m.payload, d));
    vc.width = 1920;
    vc.sps.clear();
    m = RoundTrip(proto::Encode(vc));
    CHECK(!proto::Decode(m.payload, d));
}

TEST(protocol_status_and_ping) {
    proto::Status s;
    s.state = proto::DeviceState::Background;
    s.thermal = 2;
    s.message = "La app está en segundo plano";
    auto m = RoundTrip(proto::Encode(s));
    proto::Status d;
    REQUIRE(proto::Decode(m.payload, d));
    CHECK(d.state == proto::DeviceState::Background);
    CHECK(d.thermal == 2);
    CHECK(d.message == s.message);
    auto p = RoundTrip(proto::Encode(proto::Pong{5, 10, 20}));
    proto::Pong pd;
    REQUIRE(proto::Decode(p.payload, pd));
    CHECK(pd.seq == 5 && pd.echo_time_us == 10 && pd.time_us == 20);
}

TEST(protocol_trailing_fields_ignored) {
    // A newer peer may append fields; older readers must still accept the message.
    auto wire = proto::Encode(proto::Ping{1, 2});
    wire.push_back(0xAB);
    wire.push_back(0xCD);
    // Fix up the length field (offset 8).
    uint32_t len = 0;
    std::memcpy(&len, wire.data() + 8, 4);
    len += 2;
    std::memcpy(wire.data() + 8, &len, 4);
    auto m = RoundTrip(wire);
    proto::Ping d;
    CHECK(proto::Decode(m.payload, d));
    CHECK(d.seq == 1);
}

TEST(protocol_truncated_payloads_rejected) {
    for (const auto& [name, wire] : GoldenMessages()) {
        auto m = RoundTrip(wire);
        // Every strict prefix of a non-empty payload must fail to decode (or be the empty message).
        for (size_t n = 0; n + 1 < m.payload.size(); ++n) {
            std::vector<uint8_t> cut(m.payload.begin(), m.payload.begin() + n);
            bool ok = true;
            switch (m.header.type) {
                case proto::Type::Hello: { proto::Hello x; ok = proto::Decode(cut, x); break; }
                case proto::Type::Caps: { proto::Caps x; ok = proto::Decode(cut, x); break; }
                case proto::Type::Configure: { proto::Configure x; ok = proto::Decode(cut, x); break; }
                case proto::Type::VideoConfig: { proto::VideoConfig x; ok = proto::Decode(cut, x); break; }
                case proto::Type::Status: { proto::Status x; ok = proto::Decode(cut, x); break; }
                case proto::Type::Ping: { proto::Ping x; ok = proto::Decode(cut, x); break; }
                case proto::Type::Pong: { proto::Pong x; ok = proto::Decode(cut, x); break; }
                case proto::Type::Error: { proto::Error x; ok = proto::Decode(cut, x); break; }
                case proto::Type::VideoFrame: {
                    proto::VideoFrameHeader x;
                    ok = n >= proto::kVideoFrameHeaderSize ? true : proto::DecodeVideoFrameHeader(cut, x);
                    if (n >= proto::kVideoFrameHeaderSize) ok = false;  // body is opaque; header covered
                    break;
                }
                default: ok = false;
            }
            // Hello's last field (model) is optional, so a cut right before it decodes.
            if (m.header.type == proto::Type::Hello && ok) continue;
            if (ok) std::printf("  %s decoded from %zu-byte prefix\n", name.c_str(), n);
            CHECK(!ok);
        }
    }
}

TEST(framer_reassembles_any_chunking) {
    std::vector<uint8_t> stream;
    auto golden = GoldenMessages();
    for (const auto& [name, wire] : golden) stream.insert(stream.end(), wire.begin(), wire.end());
    std::mt19937 rng(1234);
    for (int trial = 0; trial < 200; ++trial) {
        proto::Framer f;
        size_t i = 0, got = 0;
        while (i < stream.size()) {
            size_t n = std::min<size_t>(stream.size() - i, 1 + rng() % 40);
            REQUIRE(f.Push(stream.data() + i, n));
            i += n;
            while (auto m = f.Pop()) {
                REQUIRE(got < golden.size());
                CHECK(m->header.length == golden[got].second.size() - proto::kHeaderSize);
                ++got;
            }
        }
        CHECK(got == golden.size());
        CHECK(f.Buffered() == 0);
    }
}

TEST(framer_rejects_bad_magic_and_oversize) {
    proto::Framer f;
    uint8_t junk[12] = {'G', 'E', 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P', '/', '1'};
    CHECK(!f.Push(junk, sizeof(junk)));
    CHECK(!f.Pop().has_value());

    ByteWriter w;
    w.u32(proto::kMagic);
    w.u16(static_cast<uint16_t>(proto::Type::Status));
    w.u16(0);
    w.u32(proto::kMaxControlPayload + 1);  // control messages are capped at 64 KiB
    proto::Framer g;
    CHECK(!g.Push(w.data().data(), w.size()));

    ByteWriter v;
    v.u32(proto::kMagic);
    v.u16(static_cast<uint16_t>(proto::Type::VideoFrame));
    v.u16(0);
    v.u32(proto::kMaxVideoPayload + 1);
    proto::Framer h;
    CHECK(!h.Push(v.data().data(), v.size()));
}

TEST(framer_random_garbage_never_crashes) {
    std::mt19937 rng(99);
    for (int trial = 0; trial < 500; ++trial) {
        std::vector<uint8_t> junk(1 + rng() % 300);
        for (auto& b : junk) b = uint8_t(rng());
        if (trial % 2 == 0 && junk.size() >= 4) std::memcpy(junk.data(), &proto::kMagic, 4);
        proto::Framer f;
        f.Push(junk.data(), junk.size());
        while (auto m = f.Pop()) {
            proto::Caps c;
            proto::Status s;
            proto::Hello h;
            proto::VideoConfig vc;
            proto::Decode(m->payload, c);
            proto::Decode(m->payload, s);
            proto::Decode(m->payload, h);
            proto::Decode(m->payload, vc);
        }
    }
    CHECK(true);
}

TEST(golden_vectors_match_file) {
    // ICAM_VECTORS points at docs/protocol-vectors-v1.txt (set by CMake).
    std::ifstream in(ICAM_VECTORS);
    REQUIRE(in.good());
    std::map<std::string, std::string> file;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name, hex;
        ss >> name >> hex;
        file[name] = hex;
    }
    for (const auto& [name, wire] : GoldenMessages()) {
        auto it = file.find(name);
        if (it == file.end()) {
            std::printf("  missing vector %s %s\n", name.c_str(), Hex(wire).c_str());
            CHECK(false);
            continue;
        }
        if (it->second != Hex(wire)) std::printf("  %s differs:\n    file %s\n    code %s\n", name.c_str(),
                                                 it->second.c_str(), Hex(wire).c_str());
        CHECK(it->second == Hex(wire));
    }
}

TEST(plist_parses_usbmuxd_reply) {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n"
        "<key>DeviceList</key><array><dict><key>DeviceID</key><integer>1</integer>"
        "<key>MessageType</key><string>Attached</string><key>Properties</key><dict>"
        "<key>ConnectionType</key><string>USB</string><key>DeviceID</key><integer>1</integer>"
        "<key>LocationID</key><integer>0</integer><key>ProductID</key><integer>4776</integer>"
        "<key>SerialNumber</key><string>TEST-DEVICE-NOT-A-REAL-UDID</string>"
        "<key>Escaped</key><string>a&amp;b&lt;c</string><key>Flag</key><true/>"
        "</dict></dict></array></dict></plist>";
    plist::Node root;
    REQUIRE(plist::Parse(xml, root));
    const plist::Node* list = root.Get("DeviceList");
    REQUIRE(list && list->kind == plist::Node::Kind::Array && list->array.size() == 1);
    const plist::Node* props = list->array[0].Get("Properties");
    REQUIRE(props);
    CHECK(props->GetString("ConnectionType") == "USB");
    CHECK(props->GetInt("ProductID") == 4776);
    CHECK(props->GetString("Escaped") == "a&b<c");
    CHECK(props->Get("Flag")->boolean);
}

TEST(plist_rejects_malformed) {
    plist::Node n;
    CHECK(!plist::Parse("<plist><dict><key>a</key>", n));
    CHECK(!plist::Parse("<plist><dict><string>x</string></dict></plist>", n));
    std::string deep = "<plist>";
    for (int i = 0; i < 40; ++i) deep += "<array>";
    CHECK(!plist::Parse(deep, n));
}

TEST(plist_writer_escapes) {
    std::string x = plist::DictWriter().String("k", "a<b&c").Integer("n", 51476).Xml();
    plist::Node n;
    REQUIRE(plist::Parse(x, n));
    CHECK(n.GetString("k") == "a<b&c");
    CHECK(n.GetInt("n") == 51476);
}

// Fake usbmuxd: answers ListDevices with one device and Connect with a chosen result, then (for
// result 0) echoes bytes like a tunnel would.
TEST(usbmux_client_against_fake_daemon) {
    net::Socket listener;
    REQUIRE(listener.Listen("127.0.0.1", 0));
    uint16_t port = listener.LocalPort();
    std::thread server([&] {
        for (int i = 0; i < 3; ++i) {
            net::Socket c = listener.Accept(5000);
            if (!c.Valid()) return;
            uint8_t hdr[16];
            if (!c.RecvAll(hdr, 16, 2000)) return;
            uint32_t len, ver, msg, tag;
            std::memcpy(&len, hdr, 4);
            std::memcpy(&ver, hdr + 4, 4);
            std::memcpy(&msg, hdr + 8, 4);
            std::memcpy(&tag, hdr + 12, 4);
            std::string body(len - 16, '\0');
            c.RecvAll(body.data(), body.size(), 2000);
            plist::Node req;
            plist::Parse(body, req);
            std::string reply;
            std::string type = req.GetString("MessageType");
            if (type == "ListDevices") {
                reply = "<plist><dict><key>DeviceList</key><array><dict><key>DeviceID</key><integer>9</integer>"
                        "<key>Properties</key><dict><key>ConnectionType</key><string>USB</string>"
                        "<key>DeviceID</key><integer>9</integer><key>ProductID</key><integer>4776</integer>"
                        "<key>SerialNumber</key><string>ABC</string></dict></dict></array></dict></plist>";
            } else if (type == "Connect") {
                bool good = req.GetInt("PortNumber") == ((proto::kDevicePort >> 8) | ((proto::kDevicePort & 0xFF) << 8));
                int result = req.GetInt("DeviceID") == 9 ? (good ? 0 : 3) : 2;
                reply = "<plist><dict><key>MessageType</key><string>Result</string><key>Number</key><integer>" +
                        std::to_string(result) + "</integer></dict></plist>";
            }
            ByteWriter w;
            w.u32(uint32_t(16 + reply.size()));
            w.u32(1);
            w.u32(8);
            w.u32(tag);
            w.bytes(reply.data(), reply.size());
            c.SendAll(w.data().data(), w.size());
            if (type == "Connect" && reply.find("<integer>0<") != std::string::npos) {
                char buf[64];
                int n = c.RecvSome(buf, sizeof(buf), 2000);
                if (n > 0) c.SendAll(buf, size_t(n));
            }
        }
    });
    std::string env = "127.0.0.1:" + std::to_string(port);
    _putenv_s("ICAM_USBMUXD", env.c_str());
    std::vector<usbmux::Device> devs;
    CHECK(usbmux::ListDevices(devs));
    REQUIRE(devs.size() == 1);
    CHECK(devs[0].id == 9 && devs[0].connection == "USB" && devs[0].serial == "ABC");
    net::Socket t;
    CHECK(usbmux::Connect(1, proto::kDevicePort, t) == usbmux::ConnectResult::BadDevice);
    CHECK(usbmux::Connect(9, proto::kDevicePort, t) == usbmux::ConnectResult::Ok);
    REQUIRE(t.Valid());
    CHECK(t.SendAll("ping", 4));
    char echo[4];
    CHECK(t.RecvAll(echo, 4, 2000) && std::memcmp(echo, "ping", 4) == 0);
    server.join();
    _putenv_s("ICAM_USBMUXD", "");
}

TEST(usbmux_no_service) {
    _putenv_s("ICAM_USBMUXD", "127.0.0.1:1");
    std::vector<usbmux::Device> devs;
    CHECK(!usbmux::ListDevices(devs, 300));
    net::Socket t;
    CHECK(usbmux::Connect(1, 1, t, 300) == usbmux::ConnectResult::NoService);
    _putenv_s("ICAM_USBMUXD", "");
}

TEST(shm_write_read_local_namespace) {
    _putenv_s("ICAM_SHM_NAMESPACE", "Local");
    shm::Objects cam;
    REQUIRE(cam.Create());
    shm::Objects agent;
    REQUIRE(agent.Open());
    const uint32_t w = 640, h = 360;
    image::Nv12 f;
    f.Allocate(w, h);
    for (int n = 1; n <= 5; ++n) {
        image::DrawTestPattern(f, n);
        REQUIRE(shm::WriteFrame(agent, n, w, h, f.y(), w, f.uv(), w, true, 1000 + n));
    }
    CHECK(WaitForSingleObject(cam.frame_ready(), 0) == WAIT_OBJECT_0);
    image::Nv12 out;
    out.Allocate(w, h);
    shm::ReadResult r;
    REQUIRE(shm::ReadLatest(cam, 0, w, h, out.y(), w, out.uv(), w, r));
    CHECK(r.frame_number == 5 && r.live && r.capture_qpc_100ns == 1005);
    CHECK(out.data == f.data);
    CHECK(!shm::ReadLatest(cam, 5, w, h, out.y(), w, out.uv(), w, r));        // nothing newer
    CHECK(!shm::ReadLatest(cam, 0, 1280, 720, out.y(), 1280, out.uv(), 1280, r));  // wrong size
    CHECK(!shm::WriteFrame(agent, 6, 3842, 2160, f.y(), w, f.uv(), w, true, 0));   // too large
    // A writer that died mid-write leaves the slot's sequence odd; the next write must recover it.
    agent.header()->slots[7 % shm::kSlots].seq |= 1;
    image::DrawTestPattern(f, 7);
    REQUIRE(shm::WriteFrame(agent, 7, w, h, f.y(), w, f.uv(), w, true, 7));
    CHECK((agent.header()->slots[7 % shm::kSlots].seq & 1) == 0);
    CHECK(shm::ReadLatest(cam, 5, w, h, out.y(), w, out.uv(), w, r) && r.frame_number == 7);
    // A second Create (e.g. Frame Server restarted while the agent held the mapping) keeps the data.
    shm::Objects cam2;
    REQUIRE(cam2.Create());
    CHECK(cam2.header()->latest_frame == 7);
    _putenv_s("ICAM_SHM_NAMESPACE", "");
}

TEST(shm_rejects_untrusted_objects_and_restricted_tokens) {
    _putenv_s("ICAM_SHM_NAMESPACE", "Local");
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr));
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE hostile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(shm::kTotalBytes), shm::Name(shm::kFramesBase).c_str());
    LocalFree(sd);
    REQUIRE(hostile != nullptr);
    shm::Objects rejected;
    CHECK(!rejected.Create());
    CHECK(!rejected.Valid());
    CloseHandle(hostile);

    shm::Objects cam;
    REQUIRE(cam.Create());
    HANDLE token = nullptr, restricted = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &token));
    BYTE world[SECURITY_MAX_SID_SIZE];
    DWORD bytes = sizeof(world);
    REQUIRE(CreateWellKnownSid(WinWorldSid, nullptr, world, &bytes));
    SID_AND_ATTRIBUTES restrict_to{world, 0};
    const bool created = CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1,
                                                &restrict_to, &restricted) != FALSE;
    CloseHandle(token);
    REQUIRE(created);
    const bool impersonated = ImpersonateLoggedOnUser(restricted) != FALSE;
    bool opened = false;
    if (impersonated) {
        shm::Objects denied;
        opened = denied.Open();
        RevertToSelf();
    }
    CloseHandle(restricted);
    CHECK(impersonated && !opened);
    _putenv_s("ICAM_SHM_NAMESPACE", "");
}

TEST(shm_rejects_invalid_copy_layouts) {
    _putenv_s("ICAM_SHM_NAMESPACE", "Local");
    shm::Objects cam;
    REQUIRE(cam.Create());
    uint8_t sentinel = 7;
    shm::ReadResult result;
    CHECK(!shm::WriteFrame(cam, 1, 640, 360, &sentinel, 1, &sentinel, 1, true, 0));
    CHECK(!shm::ReadLatest(cam, 0, UINT32_MAX, 360, &sentinel, UINT32_MAX, &sentinel, UINT32_MAX, result));
    CHECK(!shm::ReadLatest(cam, 0, 640, 360, &sentinel, 1, &sentinel, 1, result));
    CHECK(sentinel == 7);
    _putenv_s("ICAM_SHM_NAMESPACE", "");
}

TEST(protocol_rejects_hostile_configure_and_timestamps) {
    proto::Configure request, decoded;
    for (auto [w, h, fps, bitrate] : {std::tuple{0, 1080, 30, 0u}, {65534, 1080, 30, 0u},
                                     {1920, 1081, 30, 0u}, {1920, 1080, 0, 0u}, {1920, 1080, 61, 0u},
                                     {1920, 1080, 30, UINT32_MAX}}) {
        request.width = static_cast<uint16_t>(w); request.height = static_cast<uint16_t>(h);
        request.fps = static_cast<uint16_t>(fps); request.bitrate = bitrate;
        CHECK(!proto::Decode(RoundTrip(proto::Encode(request)).payload, decoded));
    }
    proto::VideoFrameHeader bad{1, UINT64_MAX, 1}, out;
    const uint8_t au = 1;
    CHECK(!proto::DecodeVideoFrameHeader(RoundTrip(proto::EncodeVideoFrame(bad, &au, 1)).payload, out));
    proto::Pong pong{1, UINT64_MAX, UINT64_MAX}, decoded_pong;
    CHECK(!proto::Decode(RoundTrip(proto::Encode(pong)).payload, decoded_pong));
    proto::Framer framer;
    CHECK(!framer.Push(nullptr, SIZE_MAX));
    CHECK(!framer.Push(&au, 1));
}

TEST(video_layout_rejects_short_pitch_offsets_and_overflow) {
    CHECK(video::RowsFit(1920 * 1088 * 3 / 2, 0, 1920, 1920, 1080));
    CHECK(video::RowsFit(1920 * 1088 * 3 / 2, 1920 * 1088, 1920, 1920, 540));
    CHECK(!video::RowsFit(100, 0, 1, 640, 1));
    CHECK(!video::RowsFit(100, 100, 640, 640, 1));
    CHECK(!video::RowsFit(100, SIZE_MAX, UINT32_MAX, 640, UINT32_MAX));
    CHECK(!video::RowsFit(100, 0, UINT32_MAX, 640, UINT32_MAX));
    CHECK(!video::RowsFit(1920 * 1080, 32, 1920, 1920, 1080));
}

TEST(image_scale_identity_and_letterbox) {
    image::Nv12 src, dst;
    src.Allocate(64, 48);
    image::Fill(src, 100, 90, 160);
    dst.Allocate(64, 48);
    image::ScaleLetterbox(src.y(), 64, src.uv(), 64, 64, 48, dst.y(), 64, dst.uv(), 64, 64, 48);
    CHECK(dst.data == src.data);
    // 4:3 into 16:9: black bars left and right, picture in the middle.
    image::Nv12 wide;
    wide.Allocate(128, 72);
    image::ScaleLetterbox(src.y(), 64, src.uv(), 64, 64, 48, wide.y(), 128, wide.uv(), 128, 128, 72);
    CHECK(wide.y()[0] == 16);                 // bar
    CHECK(wide.y()[36 * 128 + 64] == 100);    // picture
    CHECK(wide.uv()[18 * 128 + 64] == 90);
    CHECK(wide.uv()[18 * 128 + 65] == 160);
}

TEST(image_stamp_roundtrip_and_after_scaling) {
    for (uint32_t w : {640u, 1280u, 1920u}) {
        uint32_t h = w * 9 / 16;
        image::Nv12 f;
        f.Allocate(w, h);
        image::DrawTestPattern(f, 3);
        image::Stamp s{0x123456, 0xDEADBEEF};
        image::DrawStamp(f.y(), w, w, h, s);
        image::Stamp r;
        REQUIRE(image::ReadStamp(f.y(), w, w, h, r));
        CHECK(r.counter == s.counter && r.qpc_ms == s.qpc_ms);
        // Scaled to another 16:9 size the stamp is still readable.
        image::Nv12 g;
        g.Allocate(1280, 720);
        image::ScaleLetterbox(f.y(), w, f.uv(), w, w, h, g.y(), 1280, g.uv(), 1280, 1280, 720);
        image::Stamp r2;
        CHECK(image::ReadStamp(g.y(), 1280, 1280, 720, r2) && r2.counter == s.counter);
    }
}

TEST(image_pattern_bars_and_motion) {
    const uint8_t bars[8][3] = {{180,128,128},{168,44,136},{145,147,44},{133,63,52},
                              {63,193,204},{51,109,212},{28,212,120},{16,128,128}};
    for (uint32_t w : {640u, 642u, 1920u}) {
        image::Nv12 f;
        const uint32_t h = 360, frame = 17;
        f.Allocate(w, h);
        image::DrawTestPattern(f, frame);
        const uint32_t bw = std::max<uint32_t>(4, w / 40) & ~1u;
        const uint32_t left = ((frame * (w / 120 + 1)) % (w - bw)) & ~1u;
        for (uint32_t y : {0u, h / 2, h - 1}) {
            for (uint32_t x = 0; x < w; ++x) {
                const bool moving = y >= h / 4 && y < h * 3 / 4 && x >= left && x < left + bw;
                CHECK(f.y()[size_t(y) * w + x] == (moving ? 235 : bars[x * 8 / w][0]));
            }
        }
        for (uint32_t y : {0u, h / 4, h / 2 - 1}) {
            for (uint32_t x = 0; x < w; x += 2) {
                const bool moving = y >= h / 8 && y < h * 3 / 8 && x >= left && x < left + bw;
                CHECK(f.uv()[size_t(y) * w + x] == (moving ? 128 : bars[x * 8 / w][1]));
                CHECK(f.uv()[size_t(y) * w + x + 1] == (moving ? 128 : bars[x * 8 / w][2]));
            }
        }
    }
}

TEST(image_card_renders_text) {
    image::Nv12 c;
    image::RenderCard(c, 1280, 720, L"Conecta tu iPhone", L"Abre la app iCam en el iPhone");
    REQUIRE(c.width == 1280 && c.height == 720);
    int bright = 0;
    for (size_t i = 0; i < size_t(1280) * 720; ++i) bright += c.y()[i] > 150;
    CHECK(bright > 1000);  // some glyph pixels
    image::Nv12 again;
    image::RenderCard(again, 1280, 720, L"Conecta tu iPhone", L"Abre la app iCam en el iPhone");
    CHECK(again.data == c.data);
}

TEST(image_card_readable_with_or_without_mirroring) {
    // Either video orientation must contain a complete readable message. Check the actual rendered
    // pixels, including long wrapping instructions, and the cache's orientation key.
    for (auto [w, h] : {std::pair{640u, 360u}, std::pair{1280u, 720u}, std::pair{1920u, 1080u}}) {
        image::Nv12 card, plain;
        const auto title = L"El servicio de Apple no encuentra el iPhone";
        const auto detail = L"Desconecta y reconecta el cable. Si persiste, reinicia Apple Mobile Device Service o Windows.";
        image::RenderCard(card, w, h, title, detail, true);
        image::RenderCard(plain, w, h, title, detail, false);
        CHECK(card.data != plain.data);
        const uint32_t top = h * 34 / 100, bottom = h * 61 / 100, reflected = h * 67 / 100;
        unsigned bright = 0;
        for (uint32_t y = 0; y < bottom - top; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const auto normal = card.y()[size_t(y + top) * w + x];
                CHECK(normal == card.y()[size_t(y + reflected) * w + w - 1 - x]);
                bright += normal > 150;
            }
        }
        CHECK(bright > 300);
        image::Nv12 cached;
        image::RenderCard(cached, w, h, title, detail, true);
        CHECK(cached.data == card.data);
    }
}

TEST(annexb_parsing) {
    const uint8_t au[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 9, 9, 9};
    std::vector<int> types;
    annexb::ForEachNal(au, sizeof(au), [&](const uint8_t* n, size_t) { types.push_back(annexb::NalType(n)); });
    REQUIRE(types.size() == 3);
    CHECK(types[0] == 7 && types[1] == 8 && types[2] == 5);
    CHECK(annexb::ContainsIdr(au, sizeof(au)));
    CHECK(annexb::ContainsParameterSets(au, sizeof(au)));
    const uint8_t avcc[] = {0, 0, 0, 2, 0x41, 7, 0, 0, 0, 1, 0x06};
    std::vector<uint8_t> out;
    CHECK(annexb::AvccToAnnexB(avcc, sizeof(avcc), out));
    CHECK(out.size() == 4 + 2 + 4 + 1);
    const uint8_t bad[] = {0, 0, 0, 9, 0x41};
    out.clear();
    CHECK(!annexb::AvccToAnnexB(bad, sizeof(bad), out));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--emit-vectors") {
        std::printf("# iCam protocol v1 golden vectors: name hex(header+payload). Generated by\n");
        std::printf("# common_tests --emit-vectors; checked by the C++ and Swift test suites.\n");
        for (const auto& [name, wire] : GoldenMessages()) std::printf("%s %s\n", name.c_str(), Hex(wire).c_str());
        return 0;
    }
    return check::RunAll(argc > 1 ? argv[1] : nullptr);
}
