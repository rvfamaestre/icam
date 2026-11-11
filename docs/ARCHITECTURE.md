# Architecture

```
iPhone 16 (iOS app "iCam")                         Windows 11 PC
┌───────────────────────────────┐                  ┌──────────────────────────────────────────────────────┐
│ AVCaptureSession (420v, SDR)  │                  │ icam-agent.exe (your session, tray)                  │
│  → VTCompressionSession H.264 │   USB-C cable    │  usbmuxd client → protocol session → MF H.264 decode │
│  → Annex-B + protocol v1      │  (USB 2.0 on     │  (D3D11/DXVA, software fallback) → scale if needed   │
│ NWListener 127.0.0.1:47800 ◀──┼── iPhone 16) ────┤  → NV12 into Global\ICam.Frames.v3 (3-slot seqlock) │
└───────────────────────────────┘  Apple Mobile    │                         │                            │
                                   Device Service  │ Frame Server service    ▼                            │
                                   127.0.0.1:27015 │  icam-vcam.dll: MF media source "iPhone Camera"      │
                                                   │  NV12/YUY2 samples, own pacing, placeholder cards    │
                                                   │        → Teams / Camera app / browsers               │
                                                   └──────────────────────────────────────────────────────┘
```

| Component | Where | Job |
|---|---|---|
| `ios/` (Swift, SwiftPM, built with xtool) | iPhone | Capture, hardware H.264, loopback server, thermal policy, minimal UI |
| `common/` (C++20, no MF/COM) | PC | Protocol codec + framer, usbmuxd client + plist, sockets, shared-memory layout, NV12 helpers, log |
| `windows/agent` → `icam-agent.exe` | PC, user session | Find iPhone, session, demand-driven streaming, decode, publish frames or status cards, tray |
| `windows/vcam` → `icam-vcam.dll` | PC, inside Frame Server (LocalService, session 0) | Media Foundation source; reads shared memory only; never blocks |
| `windows/media` | PC | MF H.264 decoder (agent) and encoder (fakephone) |
| `windows/tools` | PC | `icam-vcamctl` (register/remove/list/status/feed), `icam-vcamtest` (MF consumer, fps/latency), `icam-fakephone` (iPhone simulator with fault injection) |

Why two processes on the PC: the camera DLL runs inside a shared system service. Keeping USB, network
and decoding in a normal user process isolates crashes (the camera falls back to a placeholder, Frame
Server keeps running) and lets the agent have a UI. Cost: one NV12 copy through shared memory.

Demand-driven: the camera DLL publishes `consumer_active` and the consumer's size/rate in shared
memory; the agent only asks the phone to stream while that is set (plus 2 s grace). The phone camera,
encoder and USB traffic are idle otherwise.

Security in 0.2.1: the persistent Windows camera is registered with `CurrentUser` access. The
Global mapping/events allow only the SID stored by the installer in HKLM, SYSTEM and LocalService.
Both sides validate the opened handles' ACL and owner before use. The phone requires a valid
host handshake and explicit consent for each connection before encoding or sending video.
Consent expires on disconnect; client-provided names are unverified. See [SECURITY.md](SECURITY.md).

Formats: the camera offers NV12 at 1080p30 (default), 720p30, 480p30, 360p30, 1080p60, 720p60,
1440p30, 2160p30 and YUY2 at 1080p30/720p30/480p30. The phone encodes at exactly the size the app
chose in Auto mode. A manual quality profile can override the size and FPS sent by the phone. The
agent then scales to the consumer's negotiated size. Capture runs at the smallest native format at
or above the selected profile. A centred crop fills the output by default, with letterboxing as an
option. Preview and capture each have their own rotation coordinator.

Wire format: [PROTOCOL.md](PROTOCOL.md). Setup: [BUILD.md](BUILD.md).
