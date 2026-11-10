# Vendored Zoom capture engine

This is the proven standalone Zoom Meeting SDK capture engine vendored from prior
CoreVideo work. It is a standalone child process built here as
`corevideo-zoom-engine` that:

- initializes the Zoom Meeting SDK and authenticates (JWT or public app key),
- joins a meeting,
- subscribes to **per-participant raw I420 video** (and audio / screen share),
- writes frames to shared memory via a lock-free double-buffer
  (`shared/engine-ipc.h` `ShmFrameHeader`: `sequence/width/height/y_len`,
  followed by Y + U + V planes),
- speaks a line-delimited JSON IPC protocol (`init` / `join` / `subscribe` /
  `leave` …) over a pipe/socket.

## Provenance

Copied from `iamfatness/corevideo` with only include-path rewrites from
`../../src/engine-ipc.h` to `engine-ipc.h` so the flattened `shared/` header can
be used. The engine has no external switcher UI dependencies, so no other edits
were needed.

Source files:
- `engine/` from the prior capture engine (`main`, `engine-video`,
  `engine-audio`, `engine-share`, `engine-writer.h`)
- `shared/engine-ipc.h` from the prior capture IPC header
- `engine/main-macos.mm` — the **macOS engine**, re-forked 2026-08-02 from the
  CoreVideo OBS plugin's mac port (its `engine/src/main-macos.mm`, live-verified
  there against real meetings). The macOS Meeting SDK is Objective-C-only, so
  this is a self-contained ObjC++ rewrite of all four Windows files speaking
  the SAME wire protocol. Adapted to Pro's protocol on the way in: 128-slot
  audio SHM ring (`ShmAudioRingHeader`), beacon-cadence frame/audio pipe
  events, `--ipc-token` instance-spliced socket/SHM names, `raw_media_status`
  events, `{stage,msg,code,reason}` join failures, mono raw audio, 32-source
  caps. Its header comment lists the deltas; keep it in sync with the Windows
  engine when the protocol moves.

## Building (dev machine only)

Requires the Zoom Meeting SDK. Excluded from the default in-container stub build.

```
cmake -S native -B native/build \
  -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON \
  -DCOREVIDEO_BUILD_ZOOM_ENGINE=ON \
  -DZOOM_SDK_DIR=/path/to/zoom-sdk
cmake --build native/build --target corevideo-zoom-engine
```

`COREVIDEO_BUILD_ZOOM_ENGINE` is intentionally separate from the other native
dev-adapter gates. Media Foundation, RTMP, D3D11, DeckLink, and AJA builds must
still configure without a Zoom SDK unless they explicitly enable either this
helper target or `COREVIDEO_WITH_ZOOM`.

### macOS

On macOS, `ZOOM_SDK_DIR` is the directory **containing** `ZoomSDK.framework`
and the target builds `main-macos.mm`. The SDK is not self-contained — it loads
its runtime bundles through the main bundle's `Contents/Frameworks`, so a bare
binary auths `Failed(1)` synchronously. Wrap it with:

```
scripts/make-macos-engine-bundle.sh --build-dir native/build-engine --link-sdk
```

(`--link-sdk` symlinks the ~600MB SDK for local iteration; omit for a
distributable copy. Never commit any Zoom SDK file.) Point the core at the
bundled binary with `COREVIDEO_ZOOM_ENGINE_PATH=<...>.app/Contents/MacOS/corevideo-zoom-engine`.

## Dev runtime wiring

The native Studio and WinUI shells use the C++ media core. To exercise the native media
core and have that core supervise the vendored Zoom engine, set both layers:

```
COREVIDEO_MEDIA_CORE_COMMAND=...\native\build\corevideo-native.exe
COREVIDEO_ZOOM_ENGINE_PATH=...\native\build-zoom-engine\corevideo-zoom-engine.exe
COREVIDEO_ZOOM_PUBLIC_APP_KEY=...
```

Use `COREVIDEO_ZOOM_SDK_JWT` instead of `COREVIDEO_ZOOM_PUBLIC_APP_KEY` when
testing the JWT auth path. Optional join tokens are read from
`COREVIDEO_ZOOM_ON_BEHALF_TOKEN`, `COREVIDEO_ZOOM_USER_ZAK`, and
`COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN`. These values are only sent to the helper
process; do not commit or log them.

## Keeping it in sync

This is a vendored copy, not a submodule. If the upstream capture engine changes,
re-copy `engine/src/*` and `src/engine-ipc.h` and re-apply the include-path
rewrites above. Keep this engine standalone so it stays portable to this app.


## CoreVideo Edge vendoring (2026-11-08)

Copied into CoreVideo Edge from `iamfatness/CoreVideoPro` main @ `8751b8d2`
(`native/zoom-engine/`). Build wiring lives in Edge's `native/CMakeLists.txt`.

The macOS engine `engine/main-macos.mm` and `scripts/make-macos-engine-bundle.sh`
were added 2026-11-09 from CoreVideoPro main @ **`219ca1db`** (commit
`f95925a` in Edge is the verbatim copy; blob hashes match Pro's). Its own
deltas are the "macOS engine deltas" below.

### Edge deltas (keep this list exact when re-syncing from Pro)

1. **Capture-only join** (`engine/main.cpp`). The `join` command accepts
   `"capture_only":true`. When set:
   - `JoinParam4WithoutLogin::isVideoOff = true` (join camera-off);
   - VoIP audio stays connected (`isAudioOff = false`) because raw audio
     depends on it, but `EngineParticipants::enforce_capture_only()` mutes
     **our own** mic (`MuteAudio(<self user id>)`) and stops our own camera
     (`MuteVideo()`) on entering the meeting and whenever the roster shows
     either back on. It never calls `MuteAudio(0)` (zero mutes everyone); if
     the self user cannot be resolved it reports
     `{"cmd":"error","stage":"capture_only","reason":"self_user_unavailable"}`
     instead. After 20 re-applications it stops and reports
     `"reason":"meeting_keeps_unmuting"`.
   - Without the flag the join behaves exactly as Pro's (CoreVideo Pro never
     sends it).

2. **AAC-visible synthetic clap** (`fake/fake-engine.cpp`). The fake's A/V
   clap audio burst alternated sign every sample (a 24 kHz tone at 48 kHz);
   AAC's low-pass removes it entirely, so the clap could not be located in any
   AAC file. It is now a full-scale 2 kHz square burst at the same sample
   position and length (`clap_burst_sample`). Placement and timing logic are
   unchanged.

3. **Silent guests** (`fake/fake-engine.cpp`). `COREVIDEO_FAKE_SILENT_PARTICIPANTS=N`
   makes the first N roster participants send no audio packets at all, the
   way real Zoom sends nothing for a guest who never speaks. The fake used to
   send every subscribed guest a continuous tone, which hid the defect that
   took down the first live take (2026-11-09). Default 0 = unchanged.

Otherwise the fake engine is unmodified.

### macOS engine deltas (`engine/main-macos.mm`, from Pro @ `219ca1db`)

Each is marked `Edge E<n>` in the source and listed in its header comment.
They give the ObjC++ engine what Edge's Windows engine does and the node
relies on:

- **E1 capture-only join** (delta 1 above, macOS SDK): `isNoVideo` from
  `"capture_only":true`, VoIP kept, own mic/camera muted through
  `actionMeetingWithCmd:{MuteAudio,MuteVideo} userID:<self id>` (never 0) on
  joining and on every audio/video status change; `self_user_unavailable`,
  `meeting_keeps_unmuting` after 20.
- **E2 ISO arrival ring**: `iso_video` command (`IsoVideoDemand`, cap 8) and a
  per-source `<prefix><uuid>-iso` ring (`shared/iso-video-ring.h`) written on
  every frame with a `steady_clock` capture stamp, released with the target.
- **E3 fixed 1080p video regions** (`i420_region_capacity(kMaxVideoShmYLen)`),
  as on Windows. Frames above 1080p are dropped with
  `video_frame_exceeds_shm_capacity`, never truncated.
- **E4 limited-range expansion** (`I420RangeNormalizer` +
  `I420ContentRangeTracker`, `isLimitedI420`), uncropped frames only;
  `COREVIDEO_ZOOM_RANGE_CORRECTION=0` turns the content tracker off.
- **E5 command dispatch by JSON `cmd` value** (`ipc_command_is`) for every
  command, including `quit`, `init`, `join`, `leave`, `start_media`,
  `stop_media` (Pro's file matched substrings of the whole line).
- **E6 same-account collision**: `onEndOtherMeetingToJoinMeetingNotification`
  ends the other meeting only for `"end_other_meeting":true`, else cancels and
  reports `join_failed` / `account-busy-elsewhere` (Windows #475).
- The `sdk_runtime_missing` reason names `make-macos-engine-bundle.sh`.

`scripts/make-macos-engine-bundle.sh` deltas: Edge bundle id
(`us.iamfatness.corevideoedge.zoom-engine`) and usage strings, version read
from `native/CMakeLists.txt` (Edge has no package.json), help text.

Not ported, deliberately: Pro's `needCustomizedUI = NO` stays (the SDK's
meeting window and a Dock icon appear on the Edge Mac, as the Windows engine
shows its UI). Only the live Mac run can say whether breakout moves arrive as
`Disconnecting/Ended` + `InMeeting` the way the node expects (Windows does).

### Lifted node code deltas (`native/src/modules`)

- `ZoomEngineProcess.cpp` (POSIX fork/exec only): the child gets stdin from
  `/dev/null`, stdout redirected to stderr (stdout is the node's JSON
  protocol; anything the SDK prints must not land there) and every other
  descriptor closed before `execl`, the POSIX equivalent of Windows'
  `bInheritHandles=FALSE`. Windows path unchanged. Edge launches it with
`COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE=0` so it never streams a participant Edge
did not explicitly subscribe.
