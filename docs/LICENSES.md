# Dependencies and licences

iCam's application code is by Rafael Maestre López. No third-party source is vendored.

## Runtime dependencies

| Dependency | Where | Licence / terms | Notes |
|---|---|---|---|
| Windows 11 Media Foundation, Frame Server, D3D11, Winsock | PC | part of Windows | `MFCreateVirtualCamera` needs Windows 11 (build 22000+) |
| Apple Mobile Device Support (Apple Mobile Device Service) | PC | Apple, installed with iTunes / Apple Devices / iCloud | Provides usbmuxd on 127.0.0.1:27015; not redistributed by us |
| AVFoundation, VideoToolbox, Network, SwiftUI | iPhone | part of iOS | public APIs only |

## Build-time tools

| Tool | Version | Licence | Integrity |
|---|---|---|---|
| Visual Studio Build Tools 2022 (MSVC 14.44), Windows SDK 10.0.26100 | — | Microsoft | already installed |
| CMake 3.29 / Ninja 1.12 | — | BSD-3 / Apache-2.0 | already installed |
| Swift toolchain (swiftly) | 6.4.2 | Apache-2.0 | from download.swift.org (HTTPS) |
| xtool | 1.24.0 | MIT | SHA-256 `dbfd9d72…dab59f` checked against the GitHub release digest |
| libimobiledevice-utils, usbmuxd, socat (Ubuntu packages) | Ubuntu 24.04 | LGPL-2.1 / GPL-3.0 / GPL-2.0 | dev tools in WSL only, not shipped |
| Xcode.xip (for the iOS SDK) | 27.1 | Apple Xcode licence | downloaded by the user with their Apple ID; not redistributed |

## References studied (not copied)

| Project | Licence | What we learned |
|---|---|---|
| microsoft/Windows-Camera `Samples/VirtualCamera` | MIT | media source/stream contract, allocator usage, stream attributes |
| smourier/VCamSample | MIT | HKLM registration requirement, Frame Server access rights, Teams remote-video caveat |
| kshitizwagle/tethercam | GPL-3.0 | usbmuxd on Windows, Global shm DACL, pacing pitfall (R10), 4K60 thermal data, LLRC throughput cap |
| MyNamesEMurray/LensLink | GPL-2.0-or-later | usbmuxd on Windows (27015), iOS capture/encode approach |
| xtool-org/xtool | MIT | SwiftPM-based iOS build without a Mac |
