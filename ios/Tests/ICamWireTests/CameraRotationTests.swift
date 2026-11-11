import XCTest
@testable import ICamWire

final class CameraRotationTests: XCTestCase {
    func testSmallSensorChangesKeepTheSameBufferOrientation() {
        for angle in [89.0, 89.99, 90.0, 90.4, 91.0] {
            XCTAssertEqual(CameraRotation.snapped(angle), 90)
        }
        XCTAssertEqual(CameraRotation.snapped(0.3), 0)
        XCTAssertEqual(CameraRotation.snapped(359.8), 0)
    }
    func testFullTurnAndNegativeAnglesAreCanonical() {
        XCTAssertEqual(CameraRotation.snapped(-90), 270)
        XCTAssertEqual(CameraRotation.snapped(360), 0)
        XCTAssertEqual(CameraRotation.snapped(450), 90)
        XCTAssertEqual(CameraRotation.snapped(180), 180)
    }
}
