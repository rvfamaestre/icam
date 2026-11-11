# iCam protocol v1

One TCP stream between the PC agent (host) and the iPhone app (device). The phone listens on
**127.0.0.1:47800**; the PC reaches it through usbmuxd (`Connect` with the port byte-swapped) over the
USB cable. Nothing listens on Wi-Fi or cellular. Code: `common/include/icam/protocol.h` (C++) and
`ios/Sources/ICamWire/Wire.swift` (Swift). Both are tested against `docs/protocol-vectors-v1.txt`.

## Framing

Every message: 12-byte header + payload. Little-endian.

| Offset | Field | Notes |
|---|---|---|
| 0 | `magic` u32 | `0x4D414349` ("ICAM" on the wire) |
| 4 | `type` u16 | see below |
| 6 | `flags` u16 | 0 |
| 8 | `length` u32 | payload bytes; ≤ 64 KiB for control, ≤ 16 MiB for `VideoFrame` |

A bad magic or an oversize length closes the connection. Unknown types are skipped (forward
compatibility). Readers ignore trailing payload bytes, so fields can be appended without a version
bump. Strings and blobs are `u16 length + bytes` (UTF-8 for strings).

## Messages

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | Hello | both | `version u16, min_version u16, role u8 (0 host, 1 device), reserved u8, name str, software str, model str` |
| 0x02 | Caps | device → host | `count u16`, then per camera `id str, name str, position u8 (1 back, 2 front), nformats u16, {w u16, h u16, max_fps u16, flags u8}…` |
| 0x03 | Configure | host → device | `camera_id str ("" = keep), position u8 (0 = keep phone's choice), w u16, h u16, fps u16, bitrate u32 (0 = auto), flags u8 (bit0 mirror), keyint u16` |
| 0x04 | Start | host → device | — |
| 0x05 | Stop | host → device | — |
| 0x06 | RequestKeyframe | host → device | — |
| 0x10 | VideoConfig | device → host | `generation u32, codec u8 (1 = H.264), w u16, h u16, fps u16, color u8 (1 = BT.709 limited), bitrate u32, sps blob, pps blob` |
| 0x11 | VideoFrame | device → host | `generation u32, pts_us u64 (capture time, device host clock), flags u8 (bit0 key)`, then one Annex-B access unit |
| 0x20 | Status | device → host | `state u8, thermal u8, pressure u8, battery u8, charging u8, fps_x10 u16, bitrate u32, w u16, h u16, max_fps_policy u16, camera_id str, message str` |
| 0x30 | Ping | host → device | `seq u32, time_us u64` |
| 0x31 | Pong | device → host | `seq u32, echo_time_us u64, time_us u64` |
| 0x7F | Error | both | `code u16, message str` |

`state`: 0 idle, 1 starting, 2 streaming, 3 background, 4 interrupted, 5 error.

## Session

1. Host connects; both sides send `Hello`. Device also sends `Caps` and `Status`.
   In 0.2.1 the device requires a compatible host Hello before accepting commands and explicit
   approval on the phone for each connection before transmitting video. A pending Start/Configure
   is applied after approval. Reconnects reset consent and pending configuration. Hello is not
   cryptographic authentication; the displayed host name is unverified. Pending approval expires
   after 30 s, pre-Hello connections after 3 s; disconnects impose a 2 s accept cooldown.
2. When an app opens the virtual camera, the host sends `Configure` (the consumer's size and rate)
   and `Start`. The phone encodes **at the requested size** (VideoToolbox scales/letterboxes), so the
   PC normally doesn't scale.
3. Device sends `VideoConfig` (new `generation`) then `VideoFrame`s starting with a keyframe. Frames
   from an old generation are dropped by the host.
4. `Stop` 2 s after the last consumer leaves. The phone camera is off whenever no app uses it.
5. Liveness: host pings every second; device sends `Status` every second. Either side drops the link
   after ~4–6 s of silence and the host reconnects.

## Latency rules

- Device: frames are refused once ~250 ms of video is in flight in the socket; after a refusal it
  skips to the next (forced) keyframe. Frames are also skipped before encoding while congested.
- Host: at most 4 frames queued for decode; beyond that the queue is flushed, frames are dropped to
  the next IDR and a keyframe is requested.
- Clock offset: from Ping/Pong with the smallest round trip; used only for latency statistics.

## Input limits

Configure accepts even dimensions 2–3840 by 2–2160, 1–60 fps and a bitrate up to 80 Mbps (0 = auto).
The phone limits inbound payloads to 64 KiB, at most 120 known messages per second and a bounded
outbound queue. Both framers bound buffered bytes. The host validates timestamps before signed
arithmetic and checks decoder-reported buffer geometry independently of VideoConfig.
