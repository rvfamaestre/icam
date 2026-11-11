import XCTest
@testable import ICamWire

final class CaptureQualityTests: XCTestCase {
    func testAutomaticFollowsConsumer() {
        XCTAssertEqual(CaptureQuality.auto.resolve(hostWidth: 1280, hostHeight: 720, hostFps: 60),
                       CaptureProfile(width: 1280, height: 720, fps: 60))
    }
    func testManualOverridesConsumerInBothDirections() {
        XCTAssertEqual(CaptureQuality.saver.resolve(hostWidth: 3840, hostHeight: 2160, hostFps: 60),
                       CaptureProfile(width: 1280, height: 720, fps: 30))
        XCTAssertEqual(CaptureQuality.ultraHD.resolve(hostWidth: 640, hostHeight: 480, hostFps: 30),
                       CaptureProfile(width: 3840, height: 2160, fps: 30))
        XCTAssertEqual(CaptureQuality.fullHD.resolve(hostWidth: 1280, hostHeight: 720, hostFps: 60),
                       CaptureProfile(width: 1920, height: 1080, fps: 30))
        XCTAssertEqual(CaptureQuality.smoothHD.resolve(hostWidth: 1920, hostHeight: 1080, hostFps: 30),
                       CaptureProfile(width: 1920, height: 1080, fps: 60))
    }
    func testUnsupportedFormatsAreNotOffered() {
        let hdOnly = [Format(width: 1920, height: 1080, maxFps: 30)]
        XCTAssertTrue(CaptureQuality.auto.supported(by: []))
        XCTAssertTrue(CaptureQuality.fullHD.supported(by: hdOnly))
        XCTAssertFalse(CaptureQuality.smoothHD.supported(by: hdOnly))
        XCTAssertFalse(CaptureQuality.ultraHD.supported(by: hdOnly))
    }
}
