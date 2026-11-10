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
(`native/zoom-engine/`). The macOS engine (`main-macos.mm`) was not copied:
Edge v1 is Windows x64 only. Build wiring lives in Edge's `native/CMakeLists.txt`.

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

4. **Scripted breakout move** (`fake/fake-engine.cpp`).
   `COREVIDEO_FAKE_ROOM_MOVE_AT_MS=T` moves the fake into a breakout room T ms
   after the first join and back after `COREVIDEO_FAKE_ROOM_MOVE_MS` (default
   4000), reporting the SDK's meeting_status sequence. Default off.

5. **Subscribe outcome lines** (`fake/fake-engine.cpp`). Each video `subscribe`
   emits `{"cmd":"debug","stage":"video_subscribe",...,"code":0,"resolution":N}`
   like the real engine. `COREVIDEO_FAKE_REFUSE_1080=1` makes a 1080p subscribe
   fail (code 12), succeed at 720p and emit `video_resolution_downgraded`, the
   real engine's fallback. Default: always granted as asked.

6. **Zoom Meeting SDK 7.1.5.43953** (build only). Edge builds the engine
   against 7.1.5 (Pro @ `8751b8d2` is on 7.0.5) because Comms needs the
   talkback controller. The vendored sources compiled and linked against
   7.1.5 with no source change (2026-11-09: no new pure virtuals on any
   interface the engine implements, MeetingStatus values unchanged). Raw
   video/ISO re-proved with the fake validator only; a live 7.1.5 take is
   still owed.

Otherwise the fake engine is unmodified. Edge launches it with
`COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE=0` so it never streams a participant Edge
did not explicitly subscribe.
