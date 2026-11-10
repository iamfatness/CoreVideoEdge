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
| `join` | `meeting` (link or ID), optional `passcode`, `displayName`, `sdkJwt`, `userZak`, `onBehalfToken`, `appPrivilegeToken`, `endOtherMeeting` | Joins capture-only: camera off, own mic muted (Comms opens the meeting mic only behind a silent virtual mic; see below). Tokens fall back to `COREVIDEO_ZOOM_SDK_JWT`, `COREVIDEO_ZOOM_USER_ZAK`, `COREVIDEO_ZOOM_ON_BEHALF_TOKEN`, `COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN`; with no JWT, `COREVIDEO_ZOOM_PUBLIC_APP_KEY` or Pro's public app key. Tokens are never logged and are dropped once sent to the engine. `endOtherMeeting:true` only after a same-account collision, and only because the operator asked. |
| `leave` | | Stops a take first (files finalize). |
| `subscribe` | `participantId`, `on` (default true), `slot` (optional, 1..cap; omitted = lowest free), `resolution` (optional: 1080 default, 720, 360), `fps` (optional: 0 = as Zoom sends (default), 30, 60) | Video at the requested Zoom feed + isolated audio. A named `slot` is a crosspoint take: a guest routed elsewhere MOVES there (keeping their resolution, rate and ISO arm), and whoever held the slot is REPLACED. Refused past the cap, for an out-of-cap slot, for other resolutions/rates, for this node itself, and when the move or replacement would touch a slot whose ISO is recording (or to turn off such a source). |
| `set-resolution` | `slot`, `resolution` (1080, 720, 360) | Re-subscribes that slot at the new Zoom feed (an unsubscribe then a subscribe). Refused while that slot's ISO is recording. |
| `set-fps` | `slot`, `fps` (0, 30, 60) | The slot's output rate: its ISO file and its SRT feed (0 paces SRT at 30). Zoom cannot be asked for a frame rate; 0 records as the guest sends. Refused while that slot's ISO is recording; an SRT feed restarts at the new rate. |
| `unsubscribe-slot` | `slot` | Frees a slot (typically one held for a guest who left). Refused while that slot's ISO is recording. |
| `iso` | `slot`, `on` | Whether the subscribed source gets an ISO. Fixed while recording. |
| `set-record-folder` | `path` | Not while recording. |
| `set-file-prefix` | `prefix` | Not while recording. |
| `record-start` | | Refused (with the reason) for: no folder, uncreatable/unwritable folder, under 10 minutes of disk for this load, no ISO sources, a selected ISO guest who has left. All files open or none do. |
| `record-stop` | | Finalizes every file in parallel, rewrites `manifest.json`. |
| `engine` | `on` (default true) | Engine On: `true` asks Zoom for raw media (and asks the host for record permission when this account lacks it); `false` releases it. Refused off while recording. Applies when next in a meeting if sent before joining. The routing is re-sent automatically when raw media returns. |
| `set-feeds` | `mode` (`off`/`ndi`/`srt`), `srtHost`, `srtBasePort` (10000), `srtLatencyMs` (120), `srtPassphrase` | Live feeds of every routed slot to CoreVideo Pro. NDI source `CoreVideo Edge <node> S<n>`; SRT caller to `srtHost:srtBasePort+n-1` (Pro's SRT input listeners). The passphrase is never logged or published. Refused with the reason when the NDI runtime or FFmpeg is missing. |
| `set-record-format` | `height` (1080 or 720) | 16:9 at that height. Not while recording. The frame rate is per slot (`set-fps`); a non-zero `fps` here is refused. Constant 30/60 snaps every frame to the grid; repeats and skips are counted. |
| `comms-key` | `slot` (0..15) + `on`, or `all:true` + `on` (ALL CALL); optional `leaseMs` (0..60000) | Talkback: hold a channel's talk key. With `leaseMs` the hold drops by itself unless re-sent within that time (re-send while held). See [Comms](#comms-zoom-talkback). |
| `comms-release-all` | | Every held key and ALL CALL up; latches stay. Never refused. Also sent by the node itself before it quits. |
| `comms-latch` | same two shapes as `comms-key` | Latch a channel (or every channel) keyed. |
| `comms-assign` | `slot` (0..15), `participantId`, `on` | Put a person on / take them off a talkback channel. |
| `comms-mic` | `device` (name; `""` = system default) | The station microphone. |
| `comms-sidetone-out` | `device` (name; `""` = system default) | Where the operator hears their own voice. |
| `comms-sidetone` | `on` | Sidetone on/off. |
| `comms-tone` | `on` | 700 Hz test tone REPLACING the mic inside the live chain. |
| `comms-gain` | `db` (whole number, -20..20) | Mic input gain. |
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
  "rawMedia": { "active": true, "reason": "..." },   // as reported by the engine; during a breakout move the node sets {active:false, reason:"moving_rooms"} until the new room re-arms
  "participants": [                   // absent until the first roster
    { "id": 16778240, "name": "Alice", "videoOn": true, "talking": false, "muted": true,
      "host": false, "self": true,    // self: this node
      "slot": 1, "iso": true,         // only when subscribed
      "received": { "width": 1920, "height": 1080, "msSinceFrame": 400, "receiving": true } }
  ],
  "subscriptions": [ { "slot": 1, "participantId": 16778240, "name": "Alice", "present": true, "iso": true, "fromConfig": false,
                       "resolution": 1080,                          // the Zoom feed this slot asks for
                       "fps": 0,                                    // ISO + SRT rate: 0 = as Zoom sends, 30, 60
                       "zoom": { "granted": 720, "refused": 1080, "refusedCode": 12 } } ],  // what the SDK accepted
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
- `recordFormat`: `{ "width": 1920, "height": 1080 }` (the rate is per slot: `subscriptions[].fps`).
- `subscriptions[].zoom`: what the SDK's `subscribe()` accepted, from the
  engine's `video_subscribe` lines. `granted` is the tier it accepted; the
  engine falls back 1080 -> 720 -> 360 when a tier is refused, and then
  `refused`/`refusedCode` name the last refusal. `failed: true` = no tier was
  accepted. Absent until the engine reports. A granted 1080 with
  `received.height` 720 means Zoom is SENDING 720p on an accepted 1080p
  subscription (the guest's camera, client or HD settings, or Zoom's choice),
  not that Edge asked for less. Each outcome is also logged as
  `[zoom-subscribe]` in the node log.
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

SRT feeds are 1920x1080 H.264 (NVENC, else x264 when NVENC is missing or out
of encode sessions) at a constant 30 fps plus AAC 48 kHz stereo in MPEG-TS,
studio range. A guest with their camera off is sent as black (after 1 s) so
their audio still reaches Pro. The SRT passphrase may use letters, digits and
`- _ . ~ ! * + , : @` (it travels in the SRT URL). NDI feeds pass the source size and
rate through as I420 (studio range) with planar float audio, plus per-frame
metadata `<corevideo_edge guest="..." zoom_id="..."/>`.

## Comms (Zoom talkback)

The talkback station of ZComms, inside this node's Zoom engine (design:
`docs/superpowers/specs/2026-11-09-comms-design.md`). 16 Zoom talkback
channels in the meeting; each eligible guest is auto-placed on their own
channel, so a channel's key talks to a named person; ALL CALL keys them all.
It needs this node's Zoom account to be **host or co-host**, and guests on
the Zoom desktop/mobile client (the web client cannot receive talkback). Video,
ISO and feeds never depend on it.

**Commands** (table above). All are refused with `"Join a meeting first."`
outside a meeting, and a key/latch ON is refused with `"Comms is not
available: <reason>"` while `comms.available` is false. Releasing a key or a
latch (`on:false`) is never refused. `comms-assign` with `on:true` is refused
for someone not in `comms.roster` and for a `talkback:false` person, with the
reason (web client, rejoin on the desktop or mobile app). Anything the node
cannot judge is the engine's call and shows up in the state (`log`, cells).

**State.** `comms` is absent until the engine reports, and removed whenever
the engine stops (leave, failure). Up to 10 updates/s from the engine, only
while something changed; the node publishes it with its normal state cadence.

```jsonc
"comms": {
  "available": true, "reason": null,      // reason: why not, operator text (null when available)
  "lamps": {"link":true,"meetingMic":true,"channel":true,"tx":false},
  "room": null,                           // station's breakout room name, null = main (always null this slice)
  "channels": [ {"slot":0,"label":"Ana","listeners":1,"ready":true,"keyed":false,"latched":false,
                 "room":null,"reachOk":1,"reachDark":0} /* ... 16 entries, slot 0..15 */ ],
  "roster": [ {"participantId":16778240,"name":"Ana","talkback":true,"room":null,"channels":[0]},
              {"participantId":16779264,"name":"Wes","talkback":false,"room":null,"channels":[]} ],
  "mic": {"device":"Microphone (USB Audio)","devices":["Microphone (USB Audio)","Headset Mic"],
          "peakDbfs":-31,"gainDb":0,"sidetone":true,
          "sidetoneDevice":"Headphones (USB Audio)","sidetoneDevices":["Headphones (USB Audio)","Speakers"],"tone":false},
  "stats": {"framesSent":5120,"channelSends":880,"underruns":0,"sendErrors":0},
  "log": ["21:40:02 requesting 16 talkback channels", "21:40:03 station up -- 16 channels",
          "21:40:08 Ana -> CH 1", "21:40:15 audio flowing into CH 1"]   // latest last, up to 50
}
```

- `reason` (when `available` is false), most fundamental first:
  `"Not in a meeting."`, `"Talkback is unavailable: the Zoom SDK returned no
  talkback controller."`, `"This meeting does not support Zoom talkback."`,
  `"Make this account host or co-host to bring talkback up."`,
  `"Zoom refused talkback: <SDK text verbatim>"`, or a `"Talkback is silent:
  ..."` sentence when the mic rule keeps this node's meeting mic muted (no
  raw-data licence, the virtual mic never initialised or was dropped). While channels are still
  forming `available` is true and `channels[].ready` false.
- `lamps.link`: in the meeting. `lamps.meetingMic`: this node's meeting audio
  is open (talkback is ACCEPTED but SILENT while it is closed: ZComms delivery
  law 1). `lamps.channel`: any channel ready. `lamps.tx`: a keyed channel has
  at least one confirmed listener.
- `channels[].label`: the person's name when exactly one (eligible) person is
  on or assigned to the channel; `null` for a group or an empty channel.
  `listeners`: CONFIRMED members (Zoom's join callback, or ALREADY_EXIST).
  `keyed`: the engine's key mask (held, ALL CALL or latched) -- a cell never
  shows on air unless this is true. `reachOk`/`reachDark`: members reachable
  from the station's room / present in another room (this slice is not
  breakout-aware: `reachDark` is 0 and `room` null).
- `roster`: everyone in the meeting except this node. `talkback:false` = the
  participant's client cannot receive talkback (web client); they are never
  invited. `channels`: the slots they are on, confirmed OR assigned and not
  yet confirmed (so an assignment shows at once, ahead of Zoom's answer).
- `mic.peakDbfs`: capture peak, whole dBFS, pre-PTT (it moves while not
  talking so gain can be set); `null` while the mic chain is not running
  (no channels yet, or the device cannot be opened -- the reason is in `log`).
  `devices`/`sidetoneDevices`: as enumerated (every 5 s). `device`: the
  device in use, or the one asked for while it is not open.
- `stats.framesSent`: 20 ms frames the TX pacer handed to the channel bank;
  `channelSends`: frames Zoom ACCEPTED into a channel (keyed channels only);
  `underruns`: pacer ticks with no frame; `sendErrors`: refused sends. Counters
  are what the engine knows, not what reached ears.

A held key ends when the app releases it, when its `leaseMs` runs out, or when
the node's control client goes away: stdin closing (the app quit or died)
quits the node, which sends `comms_release_all` and then stops the engine.

**The station's microphone.** Comms opens this node's meeting mic (talkback
is silent while it is muted) only behind a never-fed virtual mic, so the room
hears nothing, and only for a capture-only join with the raw-data licence.
The mic is unmuted only AFTER Zoom confirms the virtual mic is initialised. If
that never happens (5 s), or the virtual mic is ever dropped, the mic stays /
goes muted for that meeting: `available` is false with `reason` saying why
(e.g. `"Talkback is silent: the silent virtual mic never initialised ..."`),
`lamps.meetingMic` is dark, and the event is in `log`. The real microphone is
never open to the room.

**Engine lines** (between the node and its engine; apps never send these --
the node relays): P2E `comms_key {"slot":0..15|"all","on":bool[,"lease_ms"]}`,
`comms_release_all {}`,
`comms_latch` (same), `comms_assign {"slot","participant_id","on"}`,
`comms_mic {"device"}`, `comms_sidetone_out {"device"}`, `comms_sidetone
{"on"}`, `comms_tone {"on"}`, `comms_gain {"db"}`; E2P `comms_state` with the
object above in snake_case (`meeting_mic`, `reach_ok`, `participant_id`,
`peak_dbfs`, `gain_db`, `sidetone_device(s)`, `frames_sent`, `channel_sends`,
`send_errors`, plus `"cmd":"comms_state"`). The node only re-keys it.

## Preview frames (for the Monitor)

The node publishes a small live picture of every routed slot so the operator
app can draw the Monitor without touching Zoom or the engine. One file per
slot, memory-mapped, portable (Windows file mapping / POSIX `mmap`):

`<previewDir>/slot-<N>.cvp` (N = 1..8), created when the slot is routed,
deleted when it is unrouted (best effort: on Windows a file a reader still has
mapped is removed once the reader lets go; readers must treat a heartbeat that
stops as "not publishing"). Little-endian, fixed size:

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

Edge speaks CoreVideo Pro's engine protocol (`native/zoom-engine`,
`shared/engine-ipc.h`) plus one join field, `"capture_only":true`, and the
`comms_*` lines above (see `native/zoom-engine/README.md` deltas 1, 7, 8). Per subscribed slot it sends
`subscribe` (resolution 2 = 1080p, 1 = 720p, 0 = 360p, from the slot's
`resolution`), `subscribe_audio` with
`isolate_audio:true`, and during a take `iso_video` with the take's ring epoch.
