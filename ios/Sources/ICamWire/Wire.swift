// iCam wire protocol v1, Swift side. Mirrors common/include/icam/protocol.h; both are checked against
// docs/protocol-vectors-v1.txt. Pure Swift (no Foundation) so it builds and tests on Linux too.

public enum Wire {
    public static let magic: UInt32 = 0x4D41_4349  // "ICAM"
    public static let version: UInt16 = 1
    public static let devicePort: UInt16 = 47800
    public static let headerSize = 12
    public static let maxControlPayload = 64 * 1024
    public static let maxVideoPayload = 16 * 1024 * 1024
    public static let videoFrameHeaderSize = 13
}

public enum MsgType: UInt16, Sendable {
    case hello = 0x01, caps = 0x02, configure = 0x03, start = 0x04, stop = 0x05, requestKeyframe = 0x06
    case videoConfig = 0x10, videoFrame = 0x11
    case status = 0x20
    case ping = 0x30, pong = 0x31
    case error = 0x7F

    var maxPayload: Int { self == .videoFrame ? Wire.maxVideoPayload : Wire.maxControlPayload }
}

// MARK: - byte helpers

public struct ByteWriter {
    public private(set) var bytes: [UInt8] = []
    public init() {}
    public mutating func u8(_ v: UInt8) { bytes.append(v) }
    public mutating func u16(_ v: UInt16) { bytes.append(UInt8(v & 0xFF)); bytes.append(UInt8(v >> 8)) }
    public mutating func u32(_ v: UInt32) { for i in 0..<4 { bytes.append(UInt8((v >> (8 * UInt32(i))) & 0xFF)) } }
    public mutating func u64(_ v: UInt64) { for i in 0..<8 { bytes.append(UInt8((v >> (8 * UInt64(i))) & 0xFF)) } }
    public mutating func raw<C: Collection>(_ b: C) where C.Element == UInt8 { bytes.append(contentsOf: b) }
    public mutating func str(_ s: String) {
        let u = Array(s.utf8.prefix(0xFFFF))
        u16(UInt16(u.count))
        bytes.append(contentsOf: u)
    }
    public mutating func blob16(_ b: [UInt8]) {
        let n = min(b.count, 0xFFFF)
        u16(UInt16(n))
        bytes.append(contentsOf: b.prefix(n))
    }
}

public struct ByteReader {
    let b: [UInt8]
    var off = 0
    public private(set) var ok = true
    public init(_ b: [UInt8]) { self.b = b }
    public var remaining: Int { ok ? b.count - off : 0 }

    mutating func take(_ n: Int) -> ArraySlice<UInt8>? {
        guard ok, n >= 0, b.count - off >= n else { ok = false; return nil }
        defer { off += n }
        return b[off..<off + n]
    }
    public mutating func u8() -> UInt8? { take(1)?.first }
    public mutating func u16() -> UInt16? {
        guard let s = take(2) else { return nil }
        return UInt16(s[s.startIndex]) | UInt16(s[s.startIndex + 1]) << 8
    }
    public mutating func u32() -> UInt32? {
        guard let s = take(4) else { return nil }
        var v: UInt32 = 0
        for (i, x) in s.enumerated() { v |= UInt32(x) << (8 * UInt32(i)) }
        return v
    }
    public mutating func u64() -> UInt64? {
        guard let s = take(8) else { return nil }
        var v: UInt64 = 0
        for (i, x) in s.enumerated() { v |= UInt64(x) << (8 * UInt64(i)) }
        return v
    }
    public mutating func str() -> String? {
        guard let n = u16(), let s = take(Int(n)) else { return nil }
        return String(decoding: s, as: UTF8.self)
    }
    public mutating func blob16() -> [UInt8]? {
        guard let n = u16(), let s = take(Int(n)) else { return nil }
        return Array(s)
    }
}

// MARK: - messages

public enum Position: UInt8, Sendable { case unspecified = 0, back = 1, front = 2 }

public struct Hello: Equatable, Sendable {
    public var version: UInt16 = Wire.version
    public var minVersion: UInt16 = Wire.version
    public var isDevice: Bool
    public var name: String
    public var software: String
    public var model: String
    public init(isDevice: Bool, name: String, software: String, model: String = "") {
        self.isDevice = isDevice; self.name = name; self.software = software; self.model = model
    }
}

public struct Format: Equatable, Sendable {
    public var width: UInt16, height: UInt16, maxFps: UInt16, flags: UInt8
    public init(width: UInt16, height: UInt16, maxFps: UInt16, flags: UInt8 = 0) {
        self.width = width; self.height = height; self.maxFps = maxFps; self.flags = flags
    }
}

public struct CameraInfo: Equatable, Sendable {
    public var id: String, name: String, position: Position, formats: [Format]
    public init(id: String, name: String, position: Position, formats: [Format]) {
        self.id = id; self.name = name; self.position = position; self.formats = formats
    }
}

public struct Configure: Equatable, Sendable {
    public var cameraId = ""
    public var position: Position = .front
    public var width: UInt16 = 1920, height: UInt16 = 1080, fps: UInt16 = 30
    public var bitrate: UInt32 = 0
    public var flags: UInt8 = 0
    public var keyint: UInt16 = 0
    public var mirror: Bool { flags & 1 != 0 }
    public init() {}
}

public struct VideoConfig: Equatable, Sendable {
    public var generation: UInt32, width: UInt16, height: UInt16, fps: UInt16
    public var color: UInt8 = 1
    public var bitrate: UInt32
    public var sps: [UInt8], pps: [UInt8]
    public init(generation: UInt32, width: UInt16, height: UInt16, fps: UInt16, bitrate: UInt32, sps: [UInt8], pps: [UInt8]) {
        self.generation = generation; self.width = width; self.height = height; self.fps = fps
        self.bitrate = bitrate; self.sps = sps; self.pps = pps
    }
}

public enum DeviceState: UInt8, Sendable { case idle = 0, starting, streaming, background, interrupted, error }

public struct Status: Equatable, Sendable {
    public var state: DeviceState = .idle
    public var thermal: UInt8 = 0, pressure: UInt8 = 0, battery: UInt8 = 255, charging: UInt8 = 0
    public var fpsX10: UInt16 = 0
    public var bitrate: UInt32 = 0
    public var width: UInt16 = 0, height: UInt16 = 0
    public var maxFpsPolicy: UInt16 = 0
    public var cameraId = ""
    public var message = ""
    public init() {}
}

public enum ErrorCode: UInt16, Sendable {
    case unknown = 0, versionMismatch, cameraUnavailable, badConfigure, permissionDenied, thermal
}

public enum Message: Equatable, Sendable {
    case hello(Hello)
    case caps([CameraInfo])
    case configure(Configure)
    case start, stop, requestKeyframe
    case videoConfig(VideoConfig)
    case videoFrame(generation: UInt32, ptsUs: UInt64, key: Bool, data: [UInt8])
    case status(Status)
    case ping(seq: UInt32, timeUs: UInt64)
    case pong(seq: UInt32, echoUs: UInt64, timeUs: UInt64)
    case error(ErrorCode, String)
}

// MARK: - encoding

public enum Codec {
    static func finish(_ t: MsgType, _ p: ByteWriter) -> [UInt8] {
        var w = ByteWriter()
        w.u32(Wire.magic)
        w.u16(t.rawValue)
        w.u16(0)
        w.u32(UInt32(p.bytes.count))
        w.raw(p.bytes)
        return w.bytes
    }

    public static func encode(_ m: Message) -> [UInt8] {
        var w = ByteWriter()
        switch m {
        case .hello(let h):
            w.u16(h.version); w.u16(h.minVersion); w.u8(h.isDevice ? 1 : 0); w.u8(0)
            w.str(h.name); w.str(h.software); w.str(h.model)
            return finish(.hello, w)
        case .caps(let cams):
            w.u16(UInt16(cams.count))
            for c in cams {
                w.str(c.id); w.str(c.name); w.u8(c.position.rawValue); w.u16(UInt16(c.formats.count))
                for f in c.formats { w.u16(f.width); w.u16(f.height); w.u16(f.maxFps); w.u8(f.flags) }
            }
            return finish(.caps, w)
        case .configure(let c):
            w.str(c.cameraId); w.u8(c.position.rawValue); w.u16(c.width); w.u16(c.height); w.u16(c.fps)
            w.u32(c.bitrate); w.u8(c.flags); w.u16(c.keyint)
            return finish(.configure, w)
        case .start: return finish(.start, w)
        case .stop: return finish(.stop, w)
        case .requestKeyframe: return finish(.requestKeyframe, w)
        case .videoConfig(let v):
            w.u32(v.generation); w.u8(1); w.u16(v.width); w.u16(v.height); w.u16(v.fps); w.u8(v.color)
            w.u32(v.bitrate); w.blob16(v.sps); w.blob16(v.pps)
            return finish(.videoConfig, w)
        case .videoFrame(let gen, let pts, let key, let data):
            return videoFrame(generation: gen, ptsUs: pts, key: key, payload: data)
        case .status(let s):
            w.u8(s.state.rawValue); w.u8(s.thermal); w.u8(s.pressure); w.u8(s.battery); w.u8(s.charging)
            w.u16(s.fpsX10); w.u32(s.bitrate); w.u16(s.width); w.u16(s.height); w.u16(s.maxFpsPolicy)
            w.str(s.cameraId); w.str(s.message)
            return finish(.status, w)
        case .ping(let seq, let t):
            w.u32(seq); w.u64(t)
            return finish(.ping, w)
        case .pong(let seq, let echo, let t):
            w.u32(seq); w.u64(echo); w.u64(t)
            return finish(.pong, w)
        case .error(let code, let msg):
            w.u16(code.rawValue); w.str(msg)
            return finish(.error, w)
        }
    }

    /// Video frames are the hot path: one allocation, header written in place.
    public static func videoFrame<C: Collection>(generation: UInt32, ptsUs: UInt64, key: Bool, payload: C) -> [UInt8]
    where C.Element == UInt8 {
        var w = ByteWriter()
        w.u32(Wire.magic)
        w.u16(MsgType.videoFrame.rawValue)
        w.u16(0)
        w.u32(UInt32(Wire.videoFrameHeaderSize + payload.count))
        w.u32(generation)
        w.u64(ptsUs)
        w.u8(key ? 1 : 0)
        w.raw(payload)
        return w.bytes
    }

    // MARK: decoding (payload without header)

    public static func decode(_ type: MsgType, _ p: [UInt8]) -> Message? {
        var r = ByteReader(p)
        switch type {
        case .hello:
            guard let v = r.u16(), let mv = r.u16(), let role = r.u8(), r.u8() != nil, role <= 1,
                  let name = r.str(), let sw = r.str() else { return nil }
            var h = Hello(isDevice: role == 1, name: name, software: sw)
            h.version = v; h.minVersion = mv
            if r.remaining > 0 { guard let model = r.str() else { return nil }; h.model = model }
            return .hello(h)
        case .caps:
            guard let n = r.u16(), n <= 16 else { return nil }
            var cams: [CameraInfo] = []
            for _ in 0..<n {
                guard let id = r.str(), let name = r.str(), let pos = r.u8().flatMap(Position.init(rawValue:)),
                      let nf = r.u16(), nf <= 512 else { return nil }
                var fs: [Format] = []
                for _ in 0..<nf {
                    guard let w = r.u16(), let h = r.u16(), let f = r.u16(), let fl = r.u8() else { return nil }
                    fs.append(Format(width: w, height: h, maxFps: f, flags: fl))
                }
                cams.append(CameraInfo(id: id, name: name, position: pos, formats: fs))
            }
            return .caps(cams)
        case .configure:
            var c = Configure()
            guard let id = r.str(), let pos = r.u8().flatMap(Position.init(rawValue:)), let w = r.u16(), let h = r.u16(),
                  let f = r.u16(), let br = r.u32(), let fl = r.u8(), let ki = r.u16() else { return nil }
            c.cameraId = id; c.position = pos; c.width = w; c.height = h; c.fps = f; c.bitrate = br; c.flags = fl; c.keyint = ki
            guard w >= 2, w <= 3840, h >= 2, h <= 2160, w % 2 == 0, h % 2 == 0,
                  f >= 1, f <= 60, br <= 80_000_000 else { return nil }
            return .configure(c)
        case .start: return .start
        case .stop: return .stop
        case .requestKeyframe: return .requestKeyframe
        case .videoConfig:
            guard let g = r.u32(), r.u8() == 1, let w = r.u16(), let h = r.u16(), let f = r.u16(), let col = r.u8(),
                  let br = r.u32(), let sps = r.blob16(), let pps = r.blob16() else { return nil }
            var v = VideoConfig(generation: g, width: w, height: h, fps: f, bitrate: br, sps: sps, pps: pps)
            v.color = col
            return .videoConfig(v)
        case .videoFrame:
            guard let g = r.u32(), let pts = r.u64(), let fl = r.u8() else { return nil }
            return .videoFrame(generation: g, ptsUs: pts, key: fl & 1 != 0, data: Array(p[Wire.videoFrameHeaderSize...]))
        case .status:
            var s = Status()
            guard let st = r.u8().flatMap(DeviceState.init(rawValue:)), let th = r.u8(), let pr = r.u8(), let bat = r.u8(),
                  let ch = r.u8(), let fps = r.u16(), let br = r.u32(), let w = r.u16(), let h = r.u16(),
                  let mf = r.u16(), let cam = r.str(), let msg = r.str() else { return nil }
            s.state = st; s.thermal = th; s.pressure = pr; s.battery = bat; s.charging = ch; s.fpsX10 = fps
            s.bitrate = br; s.width = w; s.height = h; s.maxFpsPolicy = mf; s.cameraId = cam; s.message = msg
            return .status(s)
        case .ping:
            guard let s = r.u32(), let t = r.u64() else { return nil }
            return .ping(seq: s, timeUs: t)
        case .pong:
            guard let s = r.u32(), let e = r.u64(), let t = r.u64() else { return nil }
            return .pong(seq: s, echoUs: e, timeUs: t)
        case .error:
            guard let c = r.u16(), let m = r.str() else { return nil }
            return .error(ErrorCode(rawValue: c) ?? .unknown, m)
        }
    }
}

// MARK: - stream framing

/// Reassembles messages from arbitrary chunks. `push` returns false when the stream is corrupt
/// (bad magic, unknown type, oversize); the connection must then be closed.
public struct Framer {
    var buf: [UInt8] = []
    var off = 0
    private let maxPayload: Int
    public private(set) var corrupt = false
    public init(maxPayload: Int = Wire.maxVideoPayload) { self.maxPayload = max(0, min(maxPayload, Wire.maxVideoPayload)) }
    public var buffered: Int { buf.count - off }

    public mutating func push<C: Collection>(_ data: C) -> Bool where C.Element == UInt8 {
        let limit = Wire.headerSize + maxPayload + 1024 * 1024
        guard !corrupt, data.count <= limit, buffered <= limit - data.count else { corrupt = true; return false }
        if off > 0 && off >= buf.count / 2 { buf.removeFirst(off); off = 0 }
        buf.append(contentsOf: data)
        if !headerValid() { corrupt = true }
        return !corrupt
    }

    /// .some(nil): not enough bytes yet; nil: corrupt. Unknown types are allowed (newer peer) with the
    /// control-message size limit, and skipped by pop().
    func header() -> (UInt16, Int)?? {
        guard buffered >= Wire.headerSize else { return .some(nil) }
        var r = ByteReader(Array(buf[off..<off + Wire.headerSize]))
        guard let magic = r.u32(), magic == Wire.magic, let t = r.u16(), r.u16() != nil, let len = r.u32() else { return nil }
        let limit = MsgType(rawValue: t)?.maxPayload ?? Wire.maxControlPayload
        guard Int(len) <= min(limit, maxPayload) else { return nil }
        return .some((t, Int(len)))
    }

    func headerValid() -> Bool { header() != nil }

    /// Next complete known message as (type, payload); nil if none is complete yet.
    public mutating func pop() -> (MsgType, [UInt8])? {
        while true {
            guard !corrupt else { return nil }
            guard let h = header() else { corrupt = true; return nil }
            guard let (raw, len) = h, buffered >= Wire.headerSize + len else { return nil }
            let start = off + Wire.headerSize
            let payload = Array(buf[start..<start + len])
            off = start + len
            if off == buf.count { buf.removeAll(keepingCapacity: true); off = 0 }
            if let type = MsgType(rawValue: raw) { return (type, payload) }
        }
    }
}

/// Validates the host's protocol handshake and message direction. USB pairing remains the trust
/// boundary; a Hello message is compatibility validation, not cryptographic authentication.
public struct HostSession {
    public private(set) var ready = false
    public init() {}
    public mutating func accept(_ message: Message) -> Bool {
        if case .hello(let h) = message {
            guard !ready, !h.isDevice, h.minVersion <= h.version,
                  h.minVersion <= Wire.version, h.version >= Wire.version else { return false }
            ready = true
            return true
        }
        guard ready else { return false }
        switch message {
        case .configure, .start, .stop, .requestKeyframe, .ping: return true
        default: return false
        }
    }
}

/// Consent belongs to one socket generation; it cannot carry over to a replacement connection.
public struct ConnectionConsent {
    public private(set) var connectionId: Int?
    public private(set) var approved = false
    public init() {}
    public mutating func begin(_ id: Int) { connectionId = id; approved = false }
    @discardableResult
    public mutating func approve(_ id: Int) -> Bool {
        guard connectionId == id else { return false }
        approved = true
        return true
    }
    public mutating func end() { connectionId = nil; approved = false }
}
