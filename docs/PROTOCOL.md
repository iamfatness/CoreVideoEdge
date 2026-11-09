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
      "videoFrames": 1830, "videoLost": 0, "audioSamples": 2880000, "silenceSamples": 96000,
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
- After `record-stop`, `recording.streams` keeps the finished take's final
  health until the next take starts.

## Engine side

Edge speaks CoreVideo Pro's engine protocol unchanged (`native/zoom-engine`,
`shared/engine-ipc.h`) plus one join field, `"capture_only":true`
(see `native/zoom-engine/README.md`). Per subscribed slot it sends
`subscribe` (resolution 2 = 1080p), `subscribe_audio` with
`isolate_audio:true`, and during a take `iso_video` with the take's ring epoch.
