// swift-tools-version: 6.0
// iCam iPhone app as a SwiftPM package, built and installed from Windows (WSL) with xtool:
//   cd ios && xtool dev build        (see docs/IOS.md)
// ICamWire (protocol) is plain Swift and also builds/tests on Linux: `swift test` in WSL.
import PackageDescription

let package = Package(
    name: "ICam",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "ICam", targets: ["ICam"]),
    ],
    targets: [
        .target(name: "ICamWire"),
        .target(
            name: "ICam",
            dependencies: ["ICamWire"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(name: "ICamWireTests", dependencies: ["ICamWire"]),
    ]
)
