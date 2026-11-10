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

7. **Comms: Zoom talkback in the engine** (`engine/engine-comms.{h,cpp}`,
   `engine/comms/` ported from ZComms @ `168a572`, listed in
   `engine/comms/PORTED.md`; `engine/main.cpp` wiring). The talkback station
   is this participant (the SDK is one participant per process). New P2E
   lines `comms_key`, `comms_latch`, `comms_assign`, `comms_mic`,
   `comms_sidetone_out`, `comms_sidetone`, `comms_tone`, `comms_gain`; new
   E2P `comms_state` (<= 10 Hz, only on change). Schema: Edge
   `docs/PROTOCOL.md` "Comms". `main.cpp` changes, all marked "delta 7":
   - comms_* lines are routed BEFORE the existing dispatch, which matches
     `quit`/`join`/`leave` as substrings (a device name could hold one);
   - `EngineComms::start()` before the command loop sets a 100 ms thread
     timer that the loop's own message pump dispatches; everything Comms does
     with the SDK runs on that thread;
   - `EngineComms::shutdown()` runs after the heartbeat stops and before
     `Leave`/`CleanUPSDK()`: the bank is released, the audio worker and the
     TX pacer thread are joined, the silent mic removed;
   - `onMeetingStatusChanged` calls `EngineComms::on_meeting_status()`
     FIRST, on every status: mic ownership is dropped synchronously (so
     `attach()`'s capture-only mute applies on re-entry even when a leave/
     rejoin or reconnect lands between two ticks) and, away from INMEETING
     (LOCKED/UNLOCKED excepted: those fire while still in the meeting), the
     bank is released at once so the TX pacer can never reach a controller
     the SDK is tearing down;
   - the join handler passes `capture_only` to `EngineComms::set_capture_only()`;
   - **delta 1 changes**: `enforce_capture_only()` does not count our mic as
     "open" while `EngineComms::owns_mic()`. Ownership is decided by
     `comms/mic_guard` (pure, unit-tested) under one rule: **the real mic can
     never be open to the room**. Only for a capture-only join, only when the
     station can run talkback (in meeting, controller, supported,
     host/co-host), and only with `HasRawdataLicense()` (without it ZComms
     found `setExternalAudioSource` can succeed and do nothing), the engine
     installs a never-fed virtual mic and then WAITS, still muted and not
     owning, for the virtual mic's `onMicInitialize`. Only then does it own
     the mic (capture-only stands down) and unmute (`UnMuteAudio(<self id>)`,
     re-asserted every 2 s). No `onMicInitialize` within 5 s: the mic stays
     muted, nothing is owned, Comms gives up for that meeting and the state
     reason says the virtual mic never initialised (if the SDK only
     initialises it once audio flows, that is what the live gate will show).
     Once owned, any drop (`onMicUninitialized` -- acted on inside the
     callback -- or the flag going false) clears ownership FIRST and sends
     `MuteAudio(<self id>)` at once, and Comms gives up for the meeting.
     A synchronous install refusal exposes nothing and is retried every 5 s
     (logged once). A join that is NOT capture-only (Pro's operator who
     speaks) is never touched: no virtual mic, no unmute; the ops log says
     when their own mute silences talkback. The camera rule is unchanged;
   - `comms_release_all` drops every held key and ALL CALL (latches stay);
     `comms_key` takes an optional `lease_ms` after which an unrefreshed hold
     drops by itself; the node sends `comms_release_all` before quitting
     (its stdin closing -- the app gone -- quits it);
   - `comms_state` change detection ignores the meter (`CommsStateEmitter`):
     a meter-only change goes out at most every 250 ms;
   - shutdown waits at most 3 s for the comms audio worker (a stuck WASAPI
     driver is leaked, not allowed to hang quit).
   The ZComms laws carried over (each enforced in `comms/comms_core.cpp` or
   the ported `TalkbackChannels`/`DuckPlanner`):
   - L1 talkback delivers only while our meeting mic is OPEN (above; MTG MIC
     lamp = `lamps.meeting_mic`) -- and the real mic is never open to the
     room (mic_guard);
   - L2 talkback does not cross breakout rooms -- NOT handled this slice: no
     room tracking, reach counts everyone as reachable;
   - L3 same-account host collision: already answered by
     `EngineConfigurationEvent` (#475);
   - L4 Zoom ducks channel members by default: unity at creation, duck (0.2)
     only while a channel is keyed AND the voice gate is open, one paced
     `SetChannelBackgroundVolume` per 300 ms;
   - L5 sends are MONO only (the SDK seam hardcodes mono);
   - the bank is ONE `CreateChannel(16)`; re-asked every 10 s only while
     nothing was granted; a partial grant is kept;
   - membership heals ONE call per 600 ms pass, rotated across channels, with
     per-(channel,person) backoff 5 s doubling to 60 s and 10 s patience after
     a successful invite; ALREADY_EXIST = present;
   - auto-assign (each eligible person their own channel, spillover to the
     least loaded) waits until the bank finished forming (16, or 5 s without
     growth) -- assigning while channels arrive piled everyone onto channel 1;
   - `IsSupportTalkback()==false` (web client) is reported, never invited,
     and its intent pruned; intent for people who left is pruned (ids recycle);
   - the roster is POLLED (1 s): `EngineParticipants` owns the participants
     controller's only event slot;
   - mic chain (miniaudio capture, gain, limiter, PTT envelope, sidetone, 20 ms
     pacer) on its own worker thread; enumeration (5 s) and device opens never
     run on the SDK thread. A missing sidetone output falls back to no
     sidetone instead of failing the talk path;
   - every tick and command is exception-fenced; nothing here calls into
     EngineVideo/EngineAudio/EngineShare.
   Not ported this slice: AEC (speexdsp), extern feeds, chat cues, breakout
   awareness and station-room moves, `roster.cpp` (replaced by polling),
   `talkback_source` (the single-channel spike). After a breakout move the
   engine sees `left` + `joined` and re-requests the bank; whether the
   previous room's channels count against Zoom's 16 cap is UNVERIFIED.
   TalkbackSdkWin adapters are parked (never freed) after each meeting so a
   late SDK callback can never hit a freed sink.

8. **Comms in the fake engine** (`fake/fake-comms.{h,cpp}`, wiring in
   `fake/fake-engine.cpp`). Runs the SAME `comms_core` over a scripted
   TalkbackSdk: channels answer ~1.2 s after `CreateChannel(16)`, invites
   and removals confirm after ~0.4 s, every guest supports talkback except
   one "web client" guest (default the last baseline guest;
   `COREVIDEO_FAKE_WEB_CLIENT_ID=<id>`, 0 = none) whose invite would be
   refused with code 3; a 50 Hz synthetic mic feeds the shared bank sink
   (keyed -> `channel_sends` rise; the peak moves while keyed or TONE is on,
   -60 dBFS otherwise; an unknown mic name withdraws the meter and says so).
   `COREVIDEO_FAKE_COMMS_ROLE=attendee` and
   `COREVIDEO_FAKE_COMMS_UNSUPPORTED=1` exercise the unavailable states.
   comms_* lines are routed before the substring dispatch, as in delta 7.

Otherwise the fake engine is unmodified. Edge launches it with
`COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE=0` so it never streams a participant Edge
did not explicitly subscribe.
