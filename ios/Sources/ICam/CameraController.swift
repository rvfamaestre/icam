#if os(iOS)
import AVFoundation
import Foundation
import ICamWire

struct CaptureRequest: Equatable {
    var cameraId: String
    var width: Int
    var height: Int
    var fps: Int
    var mirror: Bool
}

/// Owns the AVCaptureSession. All configuration happens on `sessionQueue`; frames arrive on
/// `videoQueue`. Picks an 8-bit 4:2:0 video-range (SDR, BT.709) format at or above the requested size,
/// with HDR, stabilization and wide colour off, and keeps the picture upright with the phone's
/// orientation (RotationCoordinator).
final class CameraController: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let session = AVCaptureSession()
    let sessionQueue = DispatchQueue(label: "icam.camera")
    let videoQueue = DispatchQueue(label: "icam.video", qos: .userInteractive)
    private let output = AVCaptureVideoDataOutput()
    private var input: AVCaptureDeviceInput?
    private(set) var device: AVCaptureDevice?
    private var rotation: AVCaptureDevice.RotationCoordinator?
    private var rotationObs: NSKeyValueObservation?
    private var wantRunning = false  // what the pipeline asked for; system restarts honour it
    private let rotationStateLock = NSLock()
    private var _capturePortrait = false
    /// Snapshot read by the protocol/UI queue. Capture configuration belongs to sessionQueue.
    private(set) var capturePortrait: Bool {
        get { rotationStateLock.lock(); defer { rotationStateLock.unlock() }; return _capturePortrait }
        set { rotationStateLock.lock(); _capturePortrait = newValue; rotationStateLock.unlock() }
    }
    private var pressureObs: NSKeyValueObservation?
    private var current: CaptureRequest?
    private var fpsCap = 0  // thermal cap, 0 = none

    /// Called on videoQueue for every captured frame.
    var onFrame: ((CMSampleBuffer) -> Void)?
    /// Called on the main queue with a Spanish message when capture stops or resumes for a system reason.
    var onInterruption: ((DeviceState, String) -> Void)?
    var onPressure: ((AVCaptureDevice.SystemPressureState.Level) -> Void)?

    override init() {
        super.init()
        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(interrupted(_:)), name: AVCaptureSession.wasInterruptedNotification, object: session)
        nc.addObserver(self, selector: #selector(interruptionEnded(_:)), name: AVCaptureSession.interruptionEndedNotification, object: session)
        nc.addObserver(self, selector: #selector(runtimeError(_:)), name: AVCaptureSession.runtimeErrorNotification, object: session)
    }

    // MARK: discovery

    static func discover() -> [AVCaptureDevice] {
        AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera],
            mediaType: .video, position: .unspecified).devices
    }

    static func label(_ d: AVCaptureDevice) -> String {
        switch (d.position, d.deviceType) {
        case (.front, _): return "Frontal"
        case (.back, .builtInUltraWideCamera): return "Trasera 0,5×"
        case (.back, .builtInTelephotoCamera): return "Trasera tele"
        default: return "Trasera 1×"
        }
    }

    static func caps() -> [CameraInfo] {
        discover().map { d in
            var best: [String: Format] = [:]
            for f in d.formats where CMFormatDescriptionGetMediaSubType(f.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange {
                let dims = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
                let maxFps = UInt16(f.videoSupportedFrameRateRanges.map(\.maxFrameRate).max() ?? 0)
                let key = "\(dims.width)x\(dims.height)"
                let fmt = Format(width: UInt16(dims.width), height: UInt16(dims.height), maxFps: maxFps,
                                 flags: (f.isVideoHDRSupported ? 1 : 0) | (f.isVideoBinned ? 2 : 0))
                if let old = best[key], old.maxFps >= maxFps { continue }
                best[key] = fmt
            }
            let formats = best.values.sorted { ($0.width, $0.height) < ($1.width, $1.height) }
            return CameraInfo(id: d.uniqueID, name: label(d), position: d.position == .front ? .front : .back, formats: formats)
        }
    }

    static func defaultCamera(_ position: Position) -> AVCaptureDevice? {
        AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: position == .back ? .back : .front)
    }

    /// Smallest 420v format at or above the request that reaches the frame rate; prefers 16:9.
    static func chooseFormat(_ d: AVCaptureDevice, width: Int, height: Int, fps: Int) -> AVCaptureDevice.Format? {
        let candidates = d.formats.filter {
            CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        }
        func dims(_ f: AVCaptureDevice.Format) -> (Int, Int) {
            let x = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return (Int(x.width), Int(x.height))
        }
        func maxFps(_ f: AVCaptureDevice.Format) -> Double { f.videoSupportedFrameRateRanges.map(\.maxFrameRate).max() ?? 0 }
        func score(_ f: AVCaptureDevice.Format) -> (Int, Int, Int) {
            let (w, h) = dims(f)
            let wide = w * 9 == h * 16 ? 0 : 1
            return (wide, w * h, f.isVideoHDRSupported ? 1 : 0)
        }
        let fits = candidates.filter { let (w, h) = dims($0); return w >= width && h >= height && maxFps($0) >= Double(fps) - 0.5 }
        if let f = fits.min(by: { score($0) < score($1) }) { return f }
        // Nothing reaches the rate at that size: biggest that does, else biggest at any rate.
        let atRate = candidates.filter { maxFps($0) >= Double(fps) - 0.5 }
        return (atRate.isEmpty ? candidates : atRate).max { dims($0).0 * dims($0).1 < dims($1).0 * dims($1).1 }
    }

    // MARK: control (call from any thread; work runs on sessionQueue)

    func apply(_ req: CaptureRequest, completion: @escaping (Result<(Int, Int, Int), Error>) -> Void) {
        sessionQueue.async {
            do { completion(.success(try self.configure(req))) } catch { completion(.failure(error)) }
        }
    }

    func startRunning() {
        sessionQueue.async {
            self.wantRunning = true
            if !self.session.isRunning { self.session.startRunning() }
            self.applyCaptureRotation()
        }
    }

    func stopRunning() {
        sessionQueue.async {
            self.wantRunning = false
            if self.session.isRunning { self.session.stopRunning() }
        }
    }

    var isRunning: Bool { session.isRunning }

    /// Thermal policy hook: cap the sensor frame rate (0 removes the cap). Lower sensor rate is what
    /// actually saves heat, unlike dropping frames after capture.
    func setFpsCap(_ cap: Int) {
        sessionQueue.async {
            self.fpsCap = cap
            if let d = self.device, let cur = self.current { self.applyFrameRate(d, fps: cur.fps) }
        }
    }

    func setZoom(_ factor: CGFloat) {
        sessionQueue.async {
            guard let d = self.device, (try? d.lockForConfiguration()) != nil else { return }
            d.videoZoomFactor = max(d.minAvailableVideoZoomFactor, min(factor, min(d.maxAvailableVideoZoomFactor, 6)))
            d.unlockForConfiguration()
        }
    }

    func setMirrored(_ mirrored: Bool) {
        sessionQueue.async {
            self.current?.mirror = mirrored
            guard let connection = self.output.connection(with: .video), connection.isVideoMirroringSupported else { return }
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = mirrored
        }
    }

    struct ConfigError: LocalizedError {
        let errorDescription: String?
    }

    /// Sets a fixed frame rate that the active format actually supports. Assigning a duration outside
    /// the format's ranges raises an Objective-C exception (a crash Swift can't catch), so clamp first.
    private func applyFrameRate(_ d: AVCaptureDevice, fps: Int) {
        let target = Double(fpsCap > 0 ? min(fps, fpsCap) : fps)
        let ranges = d.activeFormat.videoSupportedFrameRateRanges
        guard !ranges.isEmpty else { return }
        let range = ranges.first { $0.minFrameRate <= target && target <= $0.maxFrameRate }
            ?? ranges.min { abs($0.maxFrameRate - target) < abs($1.maxFrameRate - target) }!
        let rate = min(max(target, range.minFrameRate), range.maxFrameRate)
        var dur = CMTime(seconds: 1 / rate, preferredTimescale: 600_000)
        if dur < range.minFrameDuration { dur = range.minFrameDuration }
        if dur > range.maxFrameDuration { dur = range.maxFrameDuration }
        guard (try? d.lockForConfiguration()) != nil else { return }
        d.activeVideoMinFrameDuration = dur
        d.activeVideoMaxFrameDuration = dur
        d.unlockForConfiguration()
    }

    private func configure(_ req: CaptureRequest) throws -> (Int, Int, Int) {
        let all = Self.discover()
        guard let d = all.first(where: { $0.uniqueID == req.cameraId }) ?? Self.defaultCamera(.front) ?? all.first else {
            throw ConfigError(errorDescription: "No hay ninguna cámara disponible")
        }
        guard let format = Self.chooseFormat(d, width: req.width, height: req.height, fps: req.fps) else {
            throw ConfigError(errorDescription: "La cámara no ofrece un formato compatible")
        }
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        session.sessionPreset = .inputPriority
        session.automaticallyConfiguresCaptureDeviceForWideColor = false

        if input?.device != d {
            if let old = input { session.removeInput(old) }
            let newInput = try AVCaptureDeviceInput(device: d)
            guard session.canAddInput(newInput) else { throw ConfigError(errorDescription: "No se puede usar esa cámara") }
            session.addInput(newInput)
            input = newInput
            device = d
            observe(d)
        }
        if !session.outputs.contains(output) {
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            output.alwaysDiscardsLateVideoFrames = true
            output.setSampleBufferDelegate(self, queue: videoQueue)
            guard session.canAddOutput(output) else { throw ConfigError(errorDescription: "No se puede capturar vídeo") }
            session.addOutput(output)
        }

        try d.lockForConfiguration()
        d.activeFormat = format
        if d.activeFormat.supportedColorSpaces.contains(.sRGB) { d.activeColorSpace = .sRGB }  // SDR, BT.709
        d.automaticallyAdjustsVideoHDREnabled = false
        if d.activeFormat.isVideoHDRSupported { d.isVideoHDREnabled = false }
        if d.isFocusModeSupported(.continuousAutoFocus) { d.focusMode = .continuousAutoFocus }
        if d.isExposureModeSupported(.continuousAutoExposure) { d.exposureMode = .continuousAutoExposure }
        if d.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) { d.whiteBalanceMode = .continuousAutoWhiteBalance }
        if d.isSmoothAutoFocusSupported { d.isSmoothAutoFocusEnabled = true }
        d.videoZoomFactor = max(d.minAvailableVideoZoomFactor, 1)
        d.unlockForConfiguration()
        current = req
        applyFrameRate(d, fps: req.fps)

        if let c = output.connection(with: .video) {
            if c.isVideoStabilizationSupported { c.preferredVideoStabilizationMode = .off }  // adds latency
            if c.isVideoMirroringSupported {
                c.automaticallyAdjustsVideoMirroring = false
                c.isVideoMirrored = req.mirror
            }
            applyCaptureRotation()
        }
        let dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
        return (Int(dims.width), Int(dims.height), fpsCap > 0 ? min(req.fps, fpsCap) : req.fps)
    }

    private func observe(_ d: AVCaptureDevice) {
        // Keep capture upright. The encoder independently fits or crops it to the output rectangle.
        // Capture and on-screen preview have independent lifetimes. The UIView owns its own preview
        // coordinator after entering a window, rather than trying to rotate a not-yet-attached layer.
        let coordinator = AVCaptureDevice.RotationCoordinator(device: d, previewLayer: nil)
        rotation = coordinator
        capturePortrait = Int(CameraRotation.snapped(Double(coordinator.videoRotationAngleForHorizonLevelCapture))) % 180 != 0
        rotationObs = coordinator.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { [weak self] c, _ in
            let angle = CGFloat(CameraRotation.snapped(Double(c.videoRotationAngleForHorizonLevelCapture)))
            self?.sessionQueue.async {
                self?.capturePortrait = Int(angle) % 180 != 0
                guard let conn = self?.output.connection(with: .video), conn.isVideoRotationAngleSupported(angle) else { return }
                if conn.videoRotationAngle != angle { conn.videoRotationAngle = angle }
            }
        }
        pressureObs = d.observe(\.systemPressureState, options: [.initial, .new]) { [weak self] dev, _ in
            self?.onPressure?(dev.systemPressureState.level)
        }
    }

    private func applyCaptureRotation() {
        guard let rotation, let c = output.connection(with: .video) else { return }
        let angle = CGFloat(CameraRotation.snapped(Double(rotation.videoRotationAngleForHorizonLevelCapture)))
        capturePortrait = Int(angle) % 180 != 0
        if c.isVideoRotationAngleSupported(angle), c.videoRotationAngle != angle { c.videoRotationAngle = angle }
    }

    // MARK: frames

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        onFrame?(sampleBuffer)
    }

    // MARK: interruptions

    @objc private func interrupted(_ n: Notification) {
        var msg = "La cámara del iPhone se ha detenido."
        var state = DeviceState.interrupted
        if let raw = n.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int,
           let reason = AVCaptureSession.InterruptionReason(rawValue: raw) {
            switch reason {
            case .videoDeviceNotAvailableInBackground:
                msg = "La app iCam está en segundo plano. Ábrela en el iPhone para reanudar."
                state = .background
            case .videoDeviceInUseByAnotherClient: msg = "Otra app está usando la cámara del iPhone."
            case .videoDeviceNotAvailableWithMultipleForegroundApps: msg = "La cámara no funciona con varias apps en pantalla."
            case .videoDeviceNotAvailableDueToSystemPressure: msg = "El iPhone está demasiado caliente: cámara detenida."
            default: break
            }
        }
        DispatchQueue.main.async { self.onInterruption?(state, msg) }
    }

    @objc private func interruptionEnded(_ n: Notification) {
        DispatchQueue.main.async { self.onInterruption?(.streaming, "") }
    }

    @objc private func runtimeError(_ n: Notification) {
        // Media services reset or similar: restart the session if it should be running.
        sessionQueue.asyncAfter(deadline: .now() + 0.5) {
            if self.wantRunning && !self.session.isRunning { self.session.startRunning() }
        }
    }
}
#endif
