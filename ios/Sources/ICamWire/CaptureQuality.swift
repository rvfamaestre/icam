// Local capture policy, independent of AVFoundation so selection can be tested on Linux too.
// These settings are not protocol fields: VideoConfig reports the resolved stream to the PC.

public enum CaptureQuality: String, CaseIterable, Identifiable, Sendable {
    case auto, saver, fullHD, smoothHD, ultraHD
    public var id: String { rawValue }
    public var label: String {
        switch self {
        case .auto: return "Auto"
        case .saver: return "720p · 30"
        case .fullHD: return "1080p · 30"
        case .smoothHD: return "1080p · 60"
        case .ultraHD: return "4K · 30"
        }
    }
    public var detail: String {
        switch self {
        case .auto: return "Se adapta a lo que pida la app del PC"
        case .saver: return "Menos consumo y calor"
        case .fullHD: return "Recomendada para videollamadas"
        case .smoothHD: return "Más fluidez; aumenta el consumo"
        case .ultraHD: return "Más detalle; aumenta el calor"
        }
    }

    public func resolve(hostWidth: Int, hostHeight: Int, hostFps: Int) -> CaptureProfile {
        switch self {
        case .auto: return CaptureProfile(width: hostWidth, height: hostHeight, fps: hostFps)
        case .saver: return CaptureProfile(width: 1280, height: 720, fps: 30)
        case .fullHD: return CaptureProfile(width: 1920, height: 1080, fps: 30)
        case .smoothHD: return CaptureProfile(width: 1920, height: 1080, fps: 60)
        case .ultraHD: return CaptureProfile(width: 3840, height: 2160, fps: 30)
        }
    }

    public func supported(by formats: [Format]) -> Bool {
        if self == .auto { return true }
        let p = resolve(hostWidth: 1920, hostHeight: 1080, hostFps: 30)
        return formats.contains { Int($0.width) >= p.width && Int($0.height) >= p.height && Int($0.maxFps) >= p.fps }
    }
}

public struct CaptureProfile: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let fps: Int
    public init(width: Int, height: Int, fps: Int) {
        self.width = width
        self.height = height
        self.fps = fps
    }
}
