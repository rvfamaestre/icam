#if os(iOS)
import AVFoundation
import ICamWire
import SwiftUI
import UIKit

@main
struct ICamApp: App {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        let hostConnectionId = model.state.hostConnectionId
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
                .statusBarHidden(true)
                .alert("¿Permitir vídeo por USB?", isPresented: Binding(
                    get: { model.state.hostApprovalNeeded }, set: { _ in })) {
                    Button("Permitir esta conexión") { model.pipeline.approveHost(connectionId: hostConnectionId) }
                    Button("Rechazar", role: .cancel) { model.pipeline.rejectHost(connectionId: hostConnectionId) }
                } message: {
                    Text("Un cliente solicita acceso: \(model.state.hostName ?? "PC"). El nombre lo indica el cliente y no verifica su identidad. Permite la conexión solo si acabas de iniciar iCam en tu PC.")
                }
        }
        .onChange(of: phase) { _, p in model.phaseChanged(p) }
    }
}

/// A lens choice as the iPhone Camera app shows it: "0,5", "1×", "2".
struct LensOption: Identifiable, Equatable {
    let cameraId: String
    let zoom: CGFloat
    let label: String
    var id: String { "\(cameraId)@\(zoom)" }
}

@MainActor
final class AppModel: ObservableObject {
    @Published var state = PipelineState()
    @Published var cameras: [CameraInfo] = []
    @Published var mirror = false
    @Published var preview = true
    @Published var dim = false
    @Published var permission: AVAuthorizationStatus = AVCaptureDevice.authorizationStatus(for: .video)
    let pipeline = Pipeline()
    private var savedBrightness: CGFloat?
    private var started = false
    private var batteryTimer: Timer?

    init() {
        UIDeviceName.cached = UIDevice.current.name
        mirror = pipeline.mirror
        preview = UserDefaults.standard.object(forKey: "preview") as? Bool ?? true
        pipeline.onState = { [weak self] s in
            self?.state = s
            self?.mirror = s.mirror
        }
        UIDevice.current.isBatteryMonitoringEnabled = true
        refreshBattery()
        batteryTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshBattery() }
        }
        requestCameraAndStart()
    }

    // MARK: cameras

    var frontCamera: CameraInfo? { cameras.first { $0.position == .front } }
    var currentCamera: CameraInfo? { cameras.first { $0.id == state.cameraId } }
    var isFront: Bool { currentCamera?.position == .front }

    /// Back-camera lens choices in iPhone order: ultra wide (0,5), wide (1×), wide with 2× zoom.
    var backLenses: [LensOption] {
        var out: [LensOption] = []
        if let uw = cameras.first(where: { $0.position == .back && $0.name.contains("0,5") }) {
            out.append(LensOption(cameraId: uw.id, zoom: 1, label: "0,5"))
        }
        if let wide = cameras.first(where: { $0.position == .back && $0.name.contains("1×") }) {
            out.append(LensOption(cameraId: wide.id, zoom: 1, label: "1"))
            out.append(LensOption(cameraId: wide.id, zoom: 2, label: "2"))
        }
        if let tele = cameras.first(where: { $0.position == .back && $0.name.contains("tele") }) {
            out.append(LensOption(cameraId: tele.id, zoom: 1, label: "Tele"))
        }
        return out
    }

    func isSelected(_ lens: LensOption) -> Bool {
        lens.cameraId == state.cameraId && abs(lens.zoom - state.zoom) < 0.01
    }

    func select(_ lens: LensOption) {
        Haptics.tap()
        UserDefaults.standard.set(lens.cameraId, forKey: "lastBackCamera")
        UserDefaults.standard.set(Double(lens.zoom), forKey: "lastBackZoom")
        pipeline.selectCamera(lens.cameraId, zoom: lens.zoom)
    }

    /// The round arrows button: front <-> the back lens used last.
    func flipCamera() {
        Haptics.tap()
        if isFront {
            let d = UserDefaults.standard
            let id = d.string(forKey: "lastBackCamera") ?? backLenses.first(where: { $0.label == "1" })?.cameraId
            let zoom = CGFloat(d.object(forKey: "lastBackZoom") as? Double ?? 1)
            if let id { pipeline.selectCamera(id, zoom: zoom) }
        } else if let front = frontCamera {
            pipeline.selectCamera(front.id, zoom: 1)
        }
    }

    // MARK: lifecycle

    func requestCameraAndStart() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            permission = .authorized
            begin()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { ok in
                Task { @MainActor in
                    self.permission = ok ? .authorized : .denied
                    if ok { self.begin() }
                }
            }
        default:
            permission = AVCaptureDevice.authorizationStatus(for: .video)
        }
    }

    private func begin() {
        guard !started else { return }
        started = true
        cameras = CameraController.caps()
        pipeline.start()
        pipeline.setPreview(preview)
        UIApplication.shared.isIdleTimerDisabled = true  // a locked phone stops the camera
    }

    func phaseChanged(_ p: ScenePhase) {
        switch p {
        case .active:
            // Permission may have been granted in Settings while we were away.
            if !started && AVCaptureDevice.authorizationStatus(for: .video) == .authorized {
                permission = .authorized
                begin()
            }
            UIApplication.shared.isIdleTimerDisabled = true
            if dim { applyDim(true) }
            pipeline.appBecameActive()
        case .inactive, .background:
            pipeline.appWillResignActive()
            restoreBrightness()
        @unknown default: break
        }
    }

    // MARK: options

    func toggleMirror() {
        Haptics.tap()
        mirror.toggle()
        pipeline.setMirror(mirror)
    }

    func togglePreview() {
        Haptics.tap()
        preview.toggle()
        UserDefaults.standard.set(preview, forKey: "preview")
        pipeline.setPreview(preview)
    }

    func toggleDim() {
        Haptics.tap()
        dim.toggle()
        applyDim(dim)
    }

    func setQuality(_ q: Quality) {
        Haptics.tap()
        pipeline.setQuality(q)
    }

    func togglePause() {
        Haptics.tap()
        pipeline.setUserPaused(!state.userPaused)
    }

    func toggleFraming() {
        Haptics.tap()
        pipeline.setFillFrame(!state.fillFrame)
    }

    private func applyDim(_ on: Bool) {
        if on {
            if savedBrightness == nil { savedBrightness = UIScreen.main.brightness }
            UIScreen.main.brightness = 0.05
        } else {
            restoreBrightness()
        }
    }

    private func restoreBrightness() {
        if let b = savedBrightness {
            UIScreen.main.brightness = b
            savedBrightness = nil
        }
    }

    private func refreshBattery() {
        let d = UIDevice.current
        let level: UInt8 = d.batteryLevel < 0 ? 255 : UInt8(d.batteryLevel * 100)
        pipeline.battery = (level, d.batteryState == .charging || d.batteryState == .full)
    }
}

enum Haptics {
    @MainActor static func tap() { UIImpactFeedbackGenerator(style: .light).impactOccurred() }
}

/// Live camera preview backed by the capture session's preview layer.
struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession
    let cameraId: String
    let fillFrame: Bool
    let mirrored: Bool

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
        var cameraId = ""
        var mirrored = false
        private var rotation: AVCaptureDevice.RotationCoordinator?
        private var observation: NSKeyValueObservation?

        override func didMoveToWindow() {
            super.didMoveToWindow()
            refreshRotation()
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            refreshRotation()
        }

        func refreshRotation() {
            guard window != nil else { return }
            if rotation?.device?.uniqueID != cameraId,
               let device = CameraController.discover().first(where: { $0.uniqueID == cameraId }) {
                observation = nil
                let coordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: previewLayer)
                rotation = coordinator
                observation = coordinator.observe(\.videoRotationAngleForHorizonLevelPreview, options: [.initial, .new]) {
                    [weak self] _, _ in
                    // AVFoundation invokes this observer on the main queue.
                    self?.applyRotation()
                }
            }
            // The capture connection may not exist at the first KVO notification. Reapply when it
            // becomes available, on layout, on camera changes and after returning to the app.
            applyRotation()
        }

        private func applyRotation() {
            guard let rotation, let connection = previewLayer.connection else { return }
            let angle = CGFloat(CameraRotation.snapped(Double(rotation.videoRotationAngleForHorizonLevelPreview)))
            if connection.isVideoRotationAngleSupported(angle), connection.videoRotationAngle != angle {
                connection.videoRotationAngle = angle
            }
            if connection.isVideoMirroringSupported {
                connection.automaticallyAdjustsVideoMirroring = false
                if connection.isVideoMirrored != mirrored { connection.isVideoMirrored = mirrored }
            }
        }
    }

    func makeUIView(context: Context) -> PreviewView {
        let v = PreviewView()
        v.previewLayer.session = session
        v.cameraId = cameraId
        v.mirrored = mirrored
        v.previewLayer.videoGravity = fillFrame ? .resizeAspectFill : .resizeAspect
        v.backgroundColor = .black
        return v
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {
        uiView.cameraId = cameraId
        uiView.mirrored = mirrored
        uiView.previewLayer.videoGravity = fillFrame ? .resizeAspectFill : .resizeAspect
        uiView.refreshRotation()
    }
}
#endif
