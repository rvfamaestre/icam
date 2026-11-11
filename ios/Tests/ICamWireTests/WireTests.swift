// Checks the Swift codec against the golden vectors the C++ codec generates
// (docs/protocol-vectors-v1.txt), plus framing and malformed-input behaviour.

import Foundation
import XCTest

@testable import ICamWire

final class WireTests: XCTestCase {
    static func vectors() throws -> [String: [UInt8]] {
        let here = URL(fileURLWithPath: #filePath)
        let file = here.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("docs/protocol-vectors-v1.txt")
        let text = try String(contentsOf: file, encoding: .utf8)
        var out: [String: [UInt8]] = [:]
        for line in text.split(whereSeparator: \.isNewline) where !line.hasPrefix("#") {
            let parts = line.split(separator: " ")
            guard parts.count == 2 else { continue }
            var bytes: [UInt8] = []
            var it = parts[1].makeIterator()
            while let hi = it.next(), let lo = it.next() { bytes.append(UInt8(String([hi, lo]), radix: 16)!) }
            out[String(parts[0])] = bytes
        }
        return out
    }

    /// The same messages common_tests.cpp encodes in GoldenMessages().
    static func golden() -> [(String, Message)] {
        var cfg = Configure()
        cfg.position = .back; cfg.width = 1280; cfg.height = 720; cfg.fps = 30; cfg.bitrate = 6_000_000; cfg.flags = 1; cfg.keyint = 60
        var st = Status()
        st.state = .streaming; st.thermal = 1; st.pressure = 1; st.battery = 87; st.charging = 1; st.fpsX10 = 299
        st.bitrate = 11_800_000; st.width = 1920; st.height = 1080; st.maxFpsPolicy = 30; st.cameraId = "front"; st.message = "OK"
        return [
            ("hello_device", .hello(Hello(isDevice: true, name: "iPhone de Rafa", software: "icam-ios 0.1.0", model: "iPhone17,3"))),
            ("caps", .caps([CameraInfo(id: "com.apple.avfoundation.avcapturedevice.built-in_video:1", name: "Front Camera",
                                       position: .front, formats: [Format(width: 1920, height: 1080, maxFps: 60),
                                                                   Format(width: 3840, height: 2160, maxFps: 60, flags: 1)])])),
            ("configure", .configure(cfg)),
            ("start", .start),
            ("stop", .stop),
            ("request_keyframe", .requestKeyframe),
            ("video_config", .videoConfig(VideoConfig(generation: 7, width: 1920, height: 1080, fps: 30, bitrate: 12_000_000,
                                                      sps: [0x67, 0x64, 0x00, 0x28], pps: [0x68, 0xEE, 0x3C, 0x80]))),
            ("video_frame", .videoFrame(generation: 7, ptsUs: 123_456_789_012, key: true, data: [0, 0, 0, 1, 0x65, 0x88, 0x84])),
            ("status", .status(st)),
            ("ping", .ping(seq: 42, timeUs: 1000)),
            ("pong", .pong(seq: 42, echoUs: 1000, timeUs: 2000)),
            ("error", .error(.cameraUnavailable, "busy")),
        ]
    }

    func testEncodeMatchesGoldenVectors() throws {
        let file = try Self.vectors()
        for (name, msg) in Self.golden() {
            XCTAssertEqual(Codec.encode(msg), file[name], "vector \(name)")
        }
    }

    func testDecodeGoldenVectors() throws {
        let file = try Self.vectors()
        for (name, msg) in Self.golden() {
            var f = Framer()
            XCTAssertTrue(f.push(file[name]!))
            let (type, payload) = try XCTUnwrap(f.pop(), name)
            XCTAssertEqual(Codec.decode(type, payload), msg, name)
        }
    }

    func testFramerHandlesAnyChunking() throws {
        var stream: [UInt8] = []
        for (_, m) in Self.golden() { stream += Codec.encode(m) }
        var rng = SystemRandomNumberGenerator()
        for _ in 0..<100 {
            var f = Framer()
            var i = 0, got = 0
            while i < stream.count {
                let n = min(stream.count - i, Int.random(in: 1...40, using: &rng))
                XCTAssertTrue(f.push(stream[i..<i + n]))
                i += n
                while f.pop() != nil { got += 1 }
            }
            XCTAssertEqual(got, Self.golden().count)
            XCTAssertEqual(f.buffered, 0)
        }
    }

    func testRejectsGarbageAndOversize() {
        var f = Framer()
        XCTAssertFalse(f.push(Array("GET / HTTP/1.1".utf8)))
        var w = ByteWriter()
        w.u32(Wire.magic); w.u16(MsgType.status.rawValue); w.u16(0); w.u32(UInt32(Wire.maxControlPayload + 1))
        var g = Framer()
        XCTAssertFalse(g.push(w.bytes))
    }

    func testUnknownTypeIsSkipped() {
        var w = ByteWriter()
        w.u32(Wire.magic); w.u16(0x55); w.u16(0); w.u32(2); w.u8(1); w.u8(2)
        var f = Framer()
        XCTAssertTrue(f.push(w.bytes + Codec.encode(.start)))
        XCTAssertEqual(f.pop()?.0, .start)
    }

    func testHostRequiresCompatibleHelloAndDirection() {
        var session = HostSession()
        XCTAssertFalse(session.accept(.start))
        XCTAssertFalse(session.accept(.hello(Hello(isDevice: true, name: "device", software: "test"))))
        var future = Hello(isDevice: false, name: "host", software: "test")
        future.minVersion = 2; future.version = 2
        XCTAssertFalse(session.accept(.hello(future)))
        let host = Hello(isDevice: false, name: "host", software: "test")
        XCTAssertTrue(session.accept(.hello(host)))
        XCTAssertTrue(session.accept(.configure(Configure())))
        XCTAssertTrue(session.accept(.start))
        XCTAssertFalse(session.accept(.hello(host)))
        XCTAssertFalse(session.accept(.status(Status())))
    }

    func testConfigureRejectsResourceAbuse() {
        for value: UInt16 in [0, 1, 3842, 65534] {
            var c = Configure(); c.width = value
            let payload = Array(Codec.encode(.configure(c)).dropFirst(Wire.headerSize))
            XCTAssertNil(Codec.decode(.configure, payload))
        }
        var c = Configure(); c.bitrate = UInt32.max
        XCTAssertNil(Codec.decode(.configure, Array(Codec.encode(.configure(c)).dropFirst(Wire.headerSize))))
        c = Configure(); c.fps = 61
        XCTAssertNil(Codec.decode(.configure, Array(Codec.encode(.configure(c)).dropFirst(Wire.headerSize))))
    }

    func testConsentDoesNotSurviveDisconnectOrReplacement() {
        var consent = ConnectionConsent()
        XCTAssertFalse(consent.approve(1))
        consent.begin(1)
        XCTAssertFalse(consent.approved)
        XCTAssertTrue(consent.approve(1))
        XCTAssertTrue(consent.approved)
        consent.end()
        XCTAssertFalse(consent.approved)
        XCTAssertFalse(consent.approve(1))
        consent.begin(2)
        XCTAssertFalse(consent.approve(1))
        XCTAssertFalse(consent.approved)
        XCTAssertTrue(consent.approve(2))
        consent.begin(3)
        XCTAssertFalse(consent.approved)
    }

    func testHostFramerCapsVideoAndLatchesCorruption() {
        var w = ByteWriter()
        w.u32(Wire.magic); w.u16(MsgType.videoFrame.rawValue); w.u16(0)
        w.u32(UInt32(Wire.maxControlPayload + 1))
        var f = Framer(maxPayload: Wire.maxControlPayload)
        XCTAssertFalse(f.push(w.bytes))
        XCTAssertTrue(f.corrupt)
        XCTAssertFalse(f.push(Codec.encode(.start)))
        XCTAssertNil(f.pop())
        var g = Framer()
        XCTAssertTrue(g.push(Codec.encode(.start) + Array("GET / HTTP/1.1".utf8)))
        XCTAssertEqual(g.pop()?.0, .start)
        XCTAssertNil(g.pop())
        XCTAssertTrue(g.corrupt)
    }

    func testTruncatedPayloadsFail() {
        for (name, msg) in Self.golden() {
            let wire = Codec.encode(msg)
            let type = MsgType(rawValue: UInt16(wire[4]) | UInt16(wire[5]) << 8)!
            let payload = Array(wire[Wire.headerSize...])
            if payload.isEmpty || type == .videoFrame { continue }
            for n in 0..<payload.count - 1 {
                let d = Codec.decode(type, Array(payload[0..<n]))
                if type == .hello, d != nil { continue }  // model is optional
                XCTAssertNil(d, "\(name) decoded from \(n)-byte prefix")
            }
        }
    }
}
