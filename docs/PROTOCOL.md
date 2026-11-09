# corevideo-edge control API

`corevideo-edge.exe` is the whole Edge node. It is driven by **JSON lines on
stdin** and answers with **JSON lines on stdout**. Diagnostics go to stderr.
The operator app and `scripts/validate-edge-iso.mjs` use exactly this; so can
any script.

```
corevideo-edge.exe [--config <edge.json>] [--engine <path> | --fake] [--version]
```

`--fake` runs the synthetic meeting engine (`corevideo-zoom-engine-fake.exe`).
Without it the node uses `corevideo-zoom-engine.exe` beside itself.

## Commands (stdin)

Every command may carry an `"id"`; the reply echoes it:
`{"type":"result","id":"7","ok":true}` or
`{"type":"result","id":"7","ok":false,"error":"<operator-readable reason>"}`.

| cmd | fields | notes |
|---|---|---|
| `join` | `meeting` (link or ID), optional `passcode`, `displayName`, `sdkJwt`, `userZak`, `onBehalfToken`, `appPrivilegeToken`, `endOtherMeeting` | Joins capture-only: camera off, own mic muted. Tokens fall back to `COREVIDEO_ZOOM_SDK_JWT`, `COREVIDEO_ZOOM_USER_ZAK`, `COREVIDEO_ZOOM_ON_BEHALF_TOKEN`, `COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN`; with no JWT, `COREVIDEO_ZOOM_PUBLIC_APP_KEY` or Pro's public app key. Tokens are never logged and are dropped once sent to the engine. `endOtherMeeting:true` only after a same-account collision, and only because the operator asked. |
| `leave` | | Stops a take first (files finalize). |
| `subscribe` | `participantId`, `on` (default true) | 1080p video + isolated audio. Refused past the cap, refused for this node itself, refused to turn off a source whose ISO is recording. |
| `unsubscribe-slot` | `slot` | Frees a slot held for a guest who left. |
| `iso` | `slot`, `on` | Whether the subscribed source gets an ISO. Fixed while recording. |
| `set-record-folder` | `path` | Not while recording. |
| `set-file-prefix` | `prefix` | Not while recording. |
| `record-start` | | Refused (with the reason) for: no folder, uncreatable/unwritable folder, under 10 minutes of disk for this load, no ISO sources, a selected ISO guest who has left. All files open or none do. |
| `record-stop` | | Finalizes every file in parallel, rewrites `manifest.json`. |
| `engine` | `on` (default true) | Engine On: `true` asks Zoom for raw media (and asks the host for record permission when this account lacks it); `false` releases it. Refused off while recording. Applies when next in a meeting if sent before joining. The routing is re-sent automatically when raw media returns. |
| `set-feeds` | `mode` (`off`/`ndi`/`srt`), `srtHost`, `srtBasePort` (10000), `srtLatencyMs` (120), `srtPassphrase` | Live feeds of every routed slot to CoreVideo Pro. NDI source `CoreVideo Edge <node> S<n>`; SRT caller to `srtHost:srtBasePort+n-1` (Pro's SRT input listeners). The passphrase is never logged or published. Refused with the reason when the NDI runtime or FFmpeg is missing. |
| `set-record-format` | `height` (1080 or 720), `fps` (0 = as Zoom delivers, 30, 60) | 16:9 at that height. Constant 30/60 snaps every frame to the grid; repeats and skips are counted. Not while recording. |
| `state` | | Publish a state line now. |
| `quit` | | Stops any take, leaves, exits. End of stdin does the same. |

## State (stdout)

`{"type":"state", ...}` on every change (at most 4/s, 2/s while recording).
**A key is absent until the node has observed its value.** Nothing is
estimated or defaulted for display.

```jsonc
{
  "type": "state", "seq": 41, "version": "0.1.0", "engineKind": "zoom" | "fake",
  "phase": "idle" | "starting-engine" | "authenticating" | "joining" | "in-meeting" | "leaving" | "failed",
  "error": "...",                     // why the last join failed / the meeting ended
  "canEndOtherSession": true,         // only after a same-account collision
  "meetingId": "81234567890",
  "displayName": "CoreVideo Edge - EDGE-01",
  "maxSubscriptions": 6,
  "recordFolder": "D:\\EdgeISO", "filePrefix": "edge01",
  "configErrors": ["..."], "configWarning": "...", "engineWarnings": ["..."],
  "engineSilentMs": 12000,            // engine heartbeat missing > 10 s
  "rawMedia": { "active": true, "reason": "..." },   // as reported by the engine
  "participants": [                   // absent until the first roster
    { "id": 16778240, "name": "Alice", "videoOn": true, "talking": false, "muted": true,
      "host": false, "self": true,    // self: this node
      "slot": 1, "iso": true,         // only when subscribed
      "received": { "width": 1920, "height": 1080, "msSinceFrame": 400, "receiving": true } }
  ],
  "subscriptions": [ { "slot": 1, "participantId": 16778240, "name": "Alice", "present": true, "iso": true, "fromConfig": false } ],
  "recording": {
    "active": true, "startedUtc": "2026-11-08T22:01:05.123Z", "elapsedMs": 61000,
    "sessionDir": "D:\\EdgeISO\\edge01-20261108-220105",
    "warning": "...",                 // disk, raw media not granted, take stopped by the meeting
    "streams": [ { "index": 1, "slot": 1, "name": "Alice", "participantId": 16778240,
      "path": "...\\ISO-01-Alice.mp4", "state": "opening|recording|finalizing|finalized|failed",
      "videoFrames": 1830, "videoLost": 0, "videoDropped": 0, "heldRepeats": 0, "encoderBacklogPeak": 1,
      "audioSamples": 2880000, "silenceSamples": 96000,
      "audioLostPackets": 0, "encoder": "hardware|software",
      "width": 1920, "height": 1080,  // last frame actually received
      "msSinceVideo": 20, "msSinceAudio": 15, "warning": "...",
      "bytes": 412345678 } ]           // on-disk size, present once the file is finalized
  }
}
```

- `received` comes from the engine's frame beacons (about 1/s per source):
  the size Zoom is actually delivering, not the size requested.
- `videoLost` counts ISO arrivals the engine's 4-frame ring overwrote before
  Edge read them. Zero is the expectation; anything else is a defect.
- `videoDropped` counts frames the writer refused because the encoder already
  held more than 60 unwritten samples (a hard memory bound). Zero is the
  expectation; anything else also shows as the stream's `warning`.
  `encoderBacklogPeak` is the most samples the encoder ever held unwritten.
- `heldRepeats` counts re-queues of the last frame while a camera was off or
  stalled (once a second, so the video track keeps advancing). They are not
  frames and are not in `videoFrames`.
- After `record-stop`, `recording.streams` keeps the finished take's final
  health until the next take starts.

## New state fields (v2)

- `engineOn` (bool): the operator's Engine On choice. `rawMedia.active` is what
  the engine reports; the two differ while the host has not granted yet.
- `movingRooms: true` while Zoom is moving this node between breakout rooms (or
  reconnecting). The take, the engine and the routing slots are kept; guests
  re-attach when the new room's roster arrives.
- `recordFormat`: `{ "width": 1920, "height": 1080, "fps": 0 }`.
- `previewDir`: folder holding one preview file per routed slot (below).
- `participants[].received.fps`: frames per second measured over the last
  second from the preview reader (absent until measured).
- `participants[].received.audioDb`: peak level of that guest's isolated audio
  over the last ~100 ms, dBFS (absent until any audio has been measured).
- `recording.streams[]`: `cadenceRepeats`, `cadenceSkipped` (constant-rate
  formats only).

## Feeds state

`feeds: { "mode": "off|ndi|srt", "srtHost", "srtBasePort", "srtLatencyMs",
"srtEncrypted": bool, "slots": [ { "slot", "target" (NDI name or SRT URL with
the passphrase redacted), "state": "starting|waiting|sending|failed",
"framesSent", "framesSkipped" (delivered by Zoom, superseded before they could
be sent), "audioSeconds", "receivers" (NDI: connected receivers, measured by
the sender), "connected" (SRT: Pro's listener reached), "error" } ] }`.

SRT feeds are 1920x1080 H.264 (NVENC, else x264) at a constant 30 fps plus
AAC 48 kHz stereo in MPEG-TS, studio range. NDI feeds pass the source size and
rate through as I420 (studio range) with planar float audio, plus per-frame
metadata `<corevideo_edge guest="..." zoom_id="..."/>`.

## Preview frames (for the Monitor)

The node publishes a small live picture of every routed slot so the operator
app can draw the Monitor without touching Zoom or the engine. One file per
slot, memory-mapped, portable (Windows file mapping / POSIX `mmap`):

`<previewDir>/slot-<N>.cvp` (N = 1..8), created when the slot is routed,
deleted when it is unrouted. Little-endian, fixed size:

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | magic `CVEP` |
| 4 | u32 | version = 1 |
| 8 | u32 | `seq`: odd while the node is writing; read when even and unchanged across the read |
| 12 | u32 | width (480) |
| 16 | u32 | height (270) |
| 20 | u32 | stride in bytes (width x 4) |
| 24 | u32 | source width (what Zoom delivered; 0 until the first frame) |
| 28 | u32 | source height |
| 32 | u32 | frames received since the slot was routed |
| 36 | i32 | measured fps x 100 over the last second (-1 until measured) |
| 40 | i32 | audio peak dBFS x 100 over the last ~100 ms (INT32_MIN until any audio) |
| 44 | u32 | participant id currently bound to the slot |
| 48 | i64 | node steady clock ms when this header was last written (heartbeat, ~15 Hz) |
| 56 | i64 | node steady clock ms of the last NEW frame (0 = none yet) |
| 64 | u8[stride x height] | picture, BGRA8 premultiplied (A = 255), letterboxed into 16:9 with black |

The app shows a frame only after the first frame arrived (`frames > 0`), and
computes "last frame N s ago" as `heartbeat - lastFrame`. If the heartbeat
itself stops advancing for > 2 s, the node is not publishing and the tile says
so. Nothing in this file is estimated.

## Engine side

Edge speaks CoreVideo Pro's engine protocol unchanged (`native/zoom-engine`,
`shared/engine-ipc.h`) plus one join field, `"capture_only":true`
(see `native/zoom-engine/README.md`). Per subscribed slot it sends
`subscribe` (resolution 2 = 1080p), `subscribe_audio` with
`isolate_audio:true`, and during a take `iso_video` with the take's ring epoch.
