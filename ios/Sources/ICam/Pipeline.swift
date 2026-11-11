#if os(iOS)
import AVFoundation
import Foundation
import ICamWire
import UIKit

/// Snapshot for the UI (delivered on the main queue).
struct PipelineState: Equatable {
    var listening = false
    var hostName: String?
    var streaming = false
    var paused: String?          // why the camera isn't running although the PC asked for it
    var width = 0, height = 0, fps = 0
    var measuredFps = 0.0
    var mbps = 0.0
    var thermal = 0              // ProcessInfo.ThermalState raw value
    var pressure = 0             // SystemPressureState level index 0...4
    var fpsCap = 0
    var cameraId = ""
    var droppedFrames = 0
    var encoderDrops = 0
    var portrait = false
    var quality: Quality = .auto
    var zoom: CGFloat = 1
    var userPaused = false
    var fillFrame = true
    var mirror = false
    var hostApprovalNeeded = false
    var hostConnectionId = 0
}

typealias Quality = CaptureQuality

/// The protocol session and the capture -> encode -> send path. Protocol state lives on the server's
/// queue; capture callbacks only touch a few atomics before handing frames to VideoToolbox.
final class Pipeline: @unchecked Sendable {
    let server = Server()
    let camera = CameraController()
    var onState: ((PipelineState) -> Void)?

    private var q: DispatchQueue { server.queue }
    private var encoder: H264Encoder?
    private var hostConfigure = Configure()
    private var hostName: String?
    private var consent = ConnectionConsent()
    private var hostApproved: Bool { consent.approved }
    private var connectedAt = Date()
    private var streamingRequested = false
    private var generation: UInt32 = 0
    private var configSent = false
    private var lastSps: [UInt8] = [], lastPps: [UInt8] = []
    private var dropUntilKey = false
    private var lastHostRx = Date()
    private var timer: DispatchSourceTimer?
    private var frameCount = 0, byteCount = 0, lastStats = Date()
    private var state = PipelineState()
    private var deviceState: DeviceState = .idle
    private var message = ""
    private var baseBitrate = 0
    private var size = (0, 0, 0)
    private var previewWanted = false
    private var userPaused = false
    private var epoch = 0  // bumped by every (re)configure or stop; stale async completions are ignored

    // Read from the capture thread.
    private let flags = NSLock()
    private var _encoder: H264Encoder?  // the encoder the capture thread may feed (nil = not streaming)
    private var _congested = false
    private var captureEncoder: H264Encoder? { flags.lock(); defer { flags.unlock() }; return _congested ? nil : _encoder }

    // User choices, persisted.
    private let defaults = UserDefaults.standard
    var selectedCameraId: String {
        get { defaults.string(forKey: "cameraId") ?? CameraController.defaultCamera(.front)?.uniqueID ?? "" }
        set { defaults.set(newValue, forKey: "cameraId") }
    }
    var mirror: Bool {
        get { defaults.bool(forKey: "mirror") }
        set { defaults.set(newValue, forKey: "mirror") }
    }
    var quality: Quality {
        get { Quality(rawValue: defaults.string(forKey: "quality") ?? "") ?? .auto }
        set { defaults.set(newValue.rawValue, forKey: "quality") }
    }
    var fillFrame: Bool {
        get { defaults.object(forKey: "fillFrame") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "fillFrame") }
    }
    /// Zoom per camera (e.g. 2× on the main back camera), applied after each configuration.
    func zoom(for id: String) -> CGFloat { CGFloat(defaults.object(forKey: "zoom." + id) as? Double ?? 1) }
    // Battery, written on the main thread, read on the network queue.
    private var _battery: (level: UInt8, charging: Bool) = (255, false)
    var battery: (level: UInt8, charging: Bool) {
        get { flags.lock(); defer { flags.unlock() }; return _battery }
        set { flags.lock(); _battery = newValue; flags.unlock() }
    }

    func start() {
        server.onConnect = { [weak self] in self?.connected() }
        server.onDisconnect = { [weak self] in self?.disconnected() }
        server.onMessage = { [weak self] m in self?.handle(m) }
        server.onListenerState = { [weak self] s in
            self?.q.async { self?.state.listening = s == "ready"; self?.publish() }
        }
        camera.onFrame = { [weak self] sb in self?.frame(sb) }
        server.onCongestion = { [weak self] c in
            guard let self else { return }
            self.flags.lock(); self._congested = c; self.flags.unlock()
        }
        camera.onInterruption = { [weak self] st, msg in self?.q.async { self?.interruption(st, msg) } }
        camera.onPressure = { [weak self] level in self?.q.async { self?.pressureChanged(level) } }
        NotificationCenter.default.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil,
                                               queue: nil) { [weak self] _ in self?.q.async { self?.applyThermalPolicy() } }
        server.start()
        let t = DispatchSource.makeTimerSource(queue: q)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
        q.async { self.applyThermalPolicy() }
    }

    // MARK: lifecycle hooks from the app

    func appBecameActive() {
        server.ensureListening()
        q.async {
            if !self.userPaused && ((self.streamingRequested && self.hostApproved) || self.previewWanted) {
                self.camera.startRunning()
                self.encoder?.forceKeyframe()
            }
            // Back in front: whatever paused us (background, Control Center, a call) is over.
            if !self.userPaused && self.streamingRequested && (self.deviceState == .background || self.deviceState == .interrupted) {
                self.deviceState = self.encoder != nil ? .streaming : .starting
                self.message = ""
                if self.encoder == nil { self.reconfigure(startStream: true) }
            }
            self.sendStatus()
            self.publish()
        }
    }

    /// Inactive (Control Center, notification shade) doesn't stop capture; real background does, and
    /// AVCaptureSession reports it as an interruption, which is what changes our state.
    func appWillResignActive() {}

    func setPreview(_ on: Bool) {
        q.async {
            self.previewWanted = on
            if on && !self.userPaused && !self.camera.isRunning {
                if self.encoder == nil { self.reconfigure(startStream: false) } else { self.camera.startRunning() }
            } else if !on && !self.streamingRequested {
                self.camera.stopRunning()
            }
        }
    }

    func selectCamera(_ id: String) {
        q.async {
            self.selectedCameraId = id
            if self.streamingRequested || self.previewWanted { self.reconfigure(startStream: self.streamingRequested) }
            self.publish()
        }
    }

    func selectCamera(_ id: String, zoom: CGFloat) {
        defaults.set(Double(zoom), forKey: "zoom." + id)
        q.async {
            if id == self.selectedCameraId {
                self.camera.setZoom(zoom)  // same lens: just zoom, no reconfiguration
            } else {
                self.selectedCameraId = id
                if self.streamingRequested || self.previewWanted { self.reconfigure(startStream: self.streamingRequested) }
            }
            self.publish()
        }
    }

    func setQuality(_ qv: Quality) {
        q.async {
            self.quality = qv
            if self.streamingRequested { self.reconfigure(startStream: true) }
            self.publish()
        }
    }

    func setFillFrame(_ fill: Bool) {
        q.async {
            self.fillFrame = fill
            if self.streamingRequested { self.reconfigure(startStream: true) }
            self.publish()
        }
    }

    func setUserPaused(_ paused: Bool) {
        q.async {
            guard paused != self.userPaused else { return }
            self.userPaused = paused
            if paused {
                self.epoch += 1  // a pending camera configuration cannot resume transmission
                self.setEncoding(nil)
                self.encoder?.invalidate()
                self.encoder = nil
                self.camera.stopRunning()
                self.deviceState = self.streamingRequested ? .interrupted : .idle
                self.message = "Vídeo pausado en el iPhone. Pulsa el botón central para reanudar."
            } else if self.streamingRequested || self.previewWanted {
                self.reconfigure(startStream: self.streamingRequested)
            }
            self.sendStatus()
            self.publish()
        }
    }

    func setMirror(_ on: Bool) {
        q.async {
            self.mirror = on
            // The last explicit choice wins. A stale PC mirror setting cannot hold this button on.
            self.hostConfigure.flags &= ~UInt8(1)
            self.camera.setMirrored(on)
            self.encoder?.forceKeyframe()
            self.publish()
        }
    }

    // MARK: protocol

    /// Consent is per TCP connection. A name supplied by the client is never a trusted identity.
    func approveHost(connectionId: Int) {
        q.async {
            guard self.server.isConnected, self.hostName != nil, self.server.currentConnectionId == connectionId else { return }
            guard self.consent.approve(connectionId) else { return }
            if self.streamingRequested {
                self.applyHostCameraChoice()
                self.reconfigure(startStream: true)
            }
            self.publish()
        }
    }

    func rejectHost(connectionId: Int) {
        q.async {
            if self.server.currentConnectionId == connectionId { self.server.disconnect() }
        }
    }

    private func applyHostCameraChoice() {
        let c = hostConfigure
        if !c.cameraId.isEmpty, CameraController.discover().contains(where: { $0.uniqueID == c.cameraId }) {
            selectedCameraId = c.cameraId
        } else if c.position != .unspecified, let d = CameraController.defaultCamera(c.position) {
            let wanted: AVCaptureDevice.Position = c.position == .back ? .back : .front
            let currentPos = CameraController.discover().first(where: { $0.uniqueID == selectedCameraId })?.position
            if currentPos != wanted { selectedCameraId = d.uniqueID }
        }
    }

    private func connected() {
        consent.begin(server.currentConnectionId)
        connectedAt = Date()
        hostConfigure = Configure()
        hostName = nil
        lastHostRx = Date()
        streamingRequested = false
        let model = Self.modelIdentifier()
        let hello = Hello(isDevice: true, name: "iPhone", software: "icam-ios \(Self.appVersion)", model: model)
        server.send(Codec.encode(.hello(hello)))
        server.send(Codec.encode(.caps(CameraController.caps())))
        deviceState = .idle
        message = ""
        sendStatus()
        publish()
    }

    private func disconnected() {
        consent.end()
        hostName = nil
        streamingRequested = false
        stopStream()
        deviceState = .idle
        publish()
    }

    private func handle(_ m: Message) {
        lastHostRx = Date()
        switch m {
        case .hello(let h):
            hostName = h.name
            if h.minVersion > Wire.version || h.version < Wire.version {
                server.send(Codec.encode(.error(.versionMismatch, "Versión de protocolo incompatible")))
            }
            publish()
        case .configure(let c):
            hostConfigure = c
            guard hostApproved else { return }
            applyHostCameraChoice()
            if streamingRequested { reconfigure(startStream: true) }
        case .start:
            streamingRequested = true
            reconfigure(startStream: true)
        case .stop:
            streamingRequested = false
            stopStream()
            deviceState = .idle
            sendStatus()
        case .requestKeyframe:
            encoder?.forceKeyframe()
        case .ping(let seq, let t):
            server.send(Codec.encode(.pong(seq: seq, echoUs: t, timeUs: Self.nowUs())))
        default:
            break
        }
    }

    /// (Re)configures capture for the host's request and, when streaming, a new encoder.
    private func reconfigure(startStream: Bool) {
        guard !startStream || hostApproved else { return }
        epoch += 1
        let myEpoch = epoch
        if userPaused {
            deviceState = streamingRequested ? .interrupted : .idle
            message = "Vídeo pausado en el iPhone. Pulsa el botón central para reanudar."
            sendStatus()
            publish()
            return
        }
        let c = hostConfigure
        let preset = quality
        let profile = startStream
            ? preset.resolve(hostWidth: Int(c.width), hostHeight: Int(c.height), hostFps: Int(c.fps))
            : CaptureProfile(width: 1280, height: 720, fps: 30)
        let w = profile.width, h = profile.height, fps = max(1, min(profile.fps, 60))
        let req = CaptureRequest(cameraId: selectedCameraId, width: w, height: h, fps: fps, mirror: mirror || c.mirror)
        deviceState = startStream ? .starting : deviceState
        setEncoding(nil)
        camera.apply(req) { [weak self] result in
            guard let self else { return }
            self.q.async { [self] in
                guard myEpoch == self.epoch else { return }  // stopped or reconfigured meanwhile
                switch result {
                case .failure(let e):
                    self.deviceState = .error
                    self.message = e.localizedDescription
                    self.server.send(Codec.encode(.error(.cameraUnavailable, e.localizedDescription)))
                    self.sendStatus()
                case .success:
                    self.camera.startRunning()
                    self.camera.setZoom(self.zoom(for: req.cameraId))
                    guard startStream, self.streamingRequested else { self.publish(); return }
                    self.encoder?.invalidate()
                    // A fixed profile needs its own bitrate: a 1080p host request must not starve a
                    // user-selected 4K stream. The PC scales it to the consumer's negotiated size.
                    self.baseBitrate = preset == .auto && c.bitrate > 0
                        ? Int(c.bitrate) : H264Encoder.defaultBitrate(width: w, height: h, fps: fps)
                    self.generation &+= 1
                    let gen = self.generation
                    self.encoder = H264Encoder(width: w, height: h, fps: fps, bitrate: self.baseBitrate,
                                               fillFrame: self.fillFrame) { [weak self] f in
                        self?.q.async { self?.encoded(f, generation: gen) }
                    }
                    if self.encoder == nil {
                        self.deviceState = .error
                        self.message = "No se pudo crear el codificador H.264"
                        self.sendStatus()
                        return
                    }
                    self.size = (w, h, fps)
                    self.configSent = false
                    self.dropUntilKey = false
                    self.server.setBudget(bitrate: self.baseBitrate)
                    self.applyThermalPolicy()
                    self.deviceState = .streaming
                    self.message = ""
                    self.setEncoding(self.encoder)
                    self.sendStatus()
                }
                self.publish()
            }
        }
    }

    private func stopStream() {
        epoch += 1
        setEncoding(nil)
        encoder?.invalidate()
        encoder = nil
        if !previewWanted { camera.stopRunning() }
    }

    private func setEncoding(_ enc: H264Encoder?) {
        flags.lock(); _encoder = enc; flags.unlock()
    }

    // MARK: media path

    /// Capture thread.
    private func frame(_ sb: CMSampleBuffer) {
        guard let enc = captureEncoder, enc.pending < 3, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sb)
        let host = CMClockGetHostTimeClock()
        let hostPts = camera.session.synchronizationClock.map { CMSyncConvertTime(pts, from: $0, to: host) } ?? pts
        let us = UInt64(max(0, CMTimeGetSeconds(hostPts) * 1_000_000))
        enc.encode(pb, pts: pts, ptsUs: us)
    }

    /// Network queue.
    private func encoded(_ f: EncodedFrame, generation gen: UInt32) {
        guard gen == generation, streamingRequested, hostApproved, !userPaused else { return }
        if f.key, let sps = f.sps, let pps = f.pps, !configSent || sps != lastSps || pps != lastPps {
            let vc = VideoConfig(generation: generation, width: UInt16(size.0), height: UInt16(size.1), fps: UInt16(size.2),
                                 bitrate: UInt32(encoder?.bitrate ?? baseBitrate), sps: sps, pps: pps)
            server.send(Codec.encode(.videoConfig(vc)))
            lastSps = sps
            lastPps = pps
            configSent = true
        }
        guard configSent else { return }
        if dropUntilKey && !f.key { return }
        let bytes = Codec.videoFrame(generation: generation, ptsUs: f.ptsUs, key: f.key, payload: f.annexB)
        if server.send(bytes, video: true) {
            if f.key { dropUntilKey = false }
            frameCount += 1
            byteCount += bytes.count
        } else {
            // The link can't keep up: skip to the next keyframe rather than queue (latency would grow).
            dropUntilKey = true
            encoder?.forceKeyframe()
        }
        flags.lock(); _congested = server.congested; flags.unlock()
    }

    // MARK: status, thermal, interruptions

    private func tick() {
        flags.lock(); _congested = server.congested; flags.unlock()
        let now = Date()
        let secs = now.timeIntervalSince(lastStats)
        state.measuredFps = Double(frameCount) / max(secs, 0.001)
        state.mbps = Double(byteCount) * 8 / 1e6 / max(secs, 0.001)
        frameCount = 0
        byteCount = 0
        lastStats = now
        if server.isConnected {
            if now.timeIntervalSince(lastHostRx) > 6 || (!hostApproved && now.timeIntervalSince(connectedAt) > 30) {
                // The PC stopped talking (agent closed, cable pulled without a TCP reset): close the
                // link so the PC reconnects with a fresh session instead of reusing a stale one.
                lastHostRx = now
                server.disconnect()
            } else {
                if let enc = encoder, enc.consecutiveFailures > 30 {
                    message = "Error del codificador de vídeo; reiniciando"
                    reconfigure(startStream: true)
                }
                sendStatus()
            }
        }
        publish()
    }

    private func sendStatus() {
        if !hostApproved { server.send(Codec.encode(.status(Status()))); return }
        var s = Status()
        s.state = deviceState
        s.thermal = UInt8(ProcessInfo.processInfo.thermalState.rawValue)
        s.pressure = UInt8(state.pressure)
        s.battery = battery.level
        s.charging = battery.charging ? 1 : 0
        s.fpsX10 = UInt16(min(state.measuredFps * 10, 6000))
        s.bitrate = UInt32(min(state.mbps * 1e6, 4e9))
        s.width = UInt16(size.0)
        s.height = UInt16(size.1)
        s.maxFpsPolicy = UInt16(state.fpsCap)
        s.cameraId = selectedCameraId
        s.message = message
        server.send(Codec.encode(.status(s)))
    }

    private func interruption(_ st: DeviceState, _ msg: String) {
        if st == .streaming {
            // Interruption ended.
            if streamingRequested && !userPaused {
                deviceState = .streaming
                message = ""
                camera.startRunning()
                encoder?.forceKeyframe()
            }
        } else if streamingRequested {
            deviceState = st
            message = msg
        }
        sendStatus()
        publish()
    }

    private func pressureChanged(_ level: AVCaptureDevice.SystemPressureState.Level) {
        let order: [AVCaptureDevice.SystemPressureState.Level] = [.nominal, .fair, .serious, .critical, .shutdown]
        state.pressure = order.firstIndex(of: level) ?? 0
        applyThermalPolicy()
    }

    /// Thermal policy: serious → ≤30 fps and 70 % bitrate; critical → ≤20 fps and 50 % bitrate.
    /// iOS itself stops the camera at "shutdown" pressure; we report it.
    private func applyThermalPolicy() {
        let thermal = ProcessInfo.processInfo.thermalState.rawValue  // 0 nominal ... 3 critical
        let level = max(thermal, min(state.pressure, 3))
        state.thermal = thermal
        let (cap, factor): (Int, Double) = level >= 3 ? (20, 0.5) : level == 2 ? (30, 0.7) : (0, 1.0)
        if cap != state.fpsCap {
            state.fpsCap = cap
            camera.setFpsCap(cap)
            message = cap > 0 ? "iPhone caliente: calidad reducida (máx. \(cap) fps)" : ""
        }
        if let enc = encoder, baseBitrate > 0 {
            let target = Int(Double(baseBitrate) * factor)
            if target != enc.bitrate { enc.setBitrate(target) }
        }
        publish()
    }

    private func publish() {
        state.hostName = hostName
        state.hostApprovalNeeded = hostName != nil && !hostApproved
        state.hostConnectionId = server.currentConnectionId
        state.streaming = streamingRequested && deviceState == .streaming
        state.paused = streamingRequested && (deviceState == .background || deviceState == .interrupted || deviceState == .error)
            ? message : nil
        state.width = size.0
        state.height = size.1
        state.fps = size.2
        state.cameraId = selectedCameraId
        state.droppedFrames = server.droppedFrames
        state.encoderDrops = encoder?.droppedByEncoder ?? 0
        state.portrait = camera.capturePortrait
        state.quality = quality
        state.zoom = zoom(for: selectedCameraId)
        state.userPaused = userPaused
        state.fillFrame = fillFrame
        state.mirror = mirror || hostConfigure.mirror
        let s = state
        DispatchQueue.main.async { self.onState?(s) }
    }

    // MARK: helpers

    static func nowUs() -> UInt64 {
        UInt64(max(0, CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock())) * 1_000_000))
    }

    static var appVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    static func modelIdentifier() -> String {
        var info = utsname()
        uname(&info)
        return withUnsafeBytes(of: &info.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }
}

/// UIDevice.name must be read on the main thread; cache it at launch.
enum UIDeviceName {
    nonisolated(unsafe) static var cached = "iPhone"
}
#endif
