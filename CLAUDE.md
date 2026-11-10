# CLAUDE.md: working guide for CoreVideo Edge

Product and commands: `README.md`. Control API: `docs/PROTOCOL.md`. Update this
file in the same change as any substantive work.

## Scope

- 2026-11-08 (v1): Zoom sign-in + join, Sources (subscribe a subset), ISO record.
- 2026-11-09 (owner, v2): routing (Subscribe + ISO, cap 8), Monitor with live
  previews, Engine On, breakout-room survival, record format (1080p/720p,
  source/30/60), live **feeds to Pro over NDI and SRT** (per routed slot), a
  logo + Pro/Volt-family design, Windows first and designed for macOS.
  Specs: `docs/superpowers/specs/2026-11-09-*`.
- Still out: compositor, scenes, graphics, program output, streaming, virtual
  camera. Never downgrade resolution automatically to fit more people on one
  node (720p is only ever an explicit record choice).

## Processes

- `CoreVideoEdge.exe` (Avalonia, `app/CoreVideoEdge.Desktop`): sign-in, join,
  routing, monitor, record, feeds to Pro. Owns no media: spawns the node, speaks
  its JSON-line API and reads the node's preview files read-only.
- `corevideo-edge.exe` (C++, `native/src/edge/`): the node. Supervises the
  engine, enforces the subscription cap, records ISOs (Media Foundation),
  publishes Monitor previews (`PreviewPublisher`) and feeds to Pro
  (`FeedSender`: NDI via the runtime loaded at run time, SRT via an FFmpeg
  child per slot).
- `corevideo-edge-ndi-probe.exe`: receives a named NDI source like Pro's NDI
  input does; used by the validator.
- `corevideo-zoom-engine.exe` / `-fake.exe` (`native/zoom-engine/`): Pro's
  engine, vendored. I420 + audio rings in shared memory.
- macOS (2026-11-09, branch `mac-native`): the same processes without `.exe`.
  The node records with VideoToolbox + AVAssetWriter
  (`edge/IsoEncoderAVFoundation.mm`; `IsoMp4Writer` keeps every timing rule
  and only the backend differs). The real engine is the ObjC++
  `main-macos.mm` and runs only as `corevideo-zoom-engine.app` with the SDK in
  `Contents/Frameworks`. Node lookup: `<node>/../Helpers/corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine`,
  then `<node>/corevideo-zoom-engine.app/...`; fake `<node>/corevideo-zoom-engine-fake`.

## Laws learned 2026-11-09 (v2 day)

- **Breakout moves are `left` + `joined`.** The SDK first reports
  `meeting_status` 14/15 (or 5, reconnecting); `EngineSupervisor` surfaces the
  number. Keep engine, slots and take across the move; re-arm on `joined`;
  the 90 s arrival timeout keys on `awaitingArrival_`, never on a display
  string; reset the flags on every exit path.
- **The engine forgets every subscription when raw media stops** (Engine Off,
  room move, lost grant). Re-send all routed subscriptions + ISO arms on each
  inactive -> active `raw_media_status` edge. A guest back with the SAME Zoom
  id must be re-marked present (was a latent bug).
- **Silent guests have no audio ring at all.** The engine creates it on the
  first packet. Anything that must carry audio (ISO, feeds) fills silence in
  real time independent of the ring existing (live: NDI feeds had no audio).
- **Preview fps comes from the engine's frame sequence** (+2 per frame), not
  from how often the 15 Hz publisher sampled.
- **The fake sizes video regions to the frame; the real engine to a fixed
  1080p capacity.** Readers try both.
- **Concurrent child processes + inheritable handles = cross-slot leaks.** Pass
  only the child's handles (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), create pipes
  non-inheritable, serialize launches.
- **FFmpeg echoes the full SRT URL (passphrase included).** Never copy its
  lines into state or logs; publish the redacted URL only.
- **Tooling trap:** in this environment the Bash tool collapses `\\n` inside
  heredocs (python/JS strings and regexes came out with real newlines). Put
  scripted edits with backslashes in a file written by the Write tool, or use
  Edit.

## Laws learned 2026-11-09 (macOS port)

- **A POSIX shm region the engine re-creates is a NEW object.** Windows
  sections are shared by name while any handle is open, so readers never
  noticed the engine destroying and re-creating a region (unsubscribe,
  Engine Off, room move). On macOS the old mapping reads the orphan forever.
  Readers reopen a region whose counter has not moved for 250 ms
  (`edge/ShmReopen.h`; preview, feeds, ISO rings) and treat a lower counter as
  a new ring. The mac engine also allocates video regions at the fixed 1080p
  capacity (Edge E3) so a resolution ramp never re-creates one.
- **macOS shm names are hashed** (`/ZOP<fnv64>`, PSHMNAMLEN 31) inside
  `shm_region_create/open_read`; never build a POSIX shm name by hand.
- **Nothing may block the ISO recorder thread for more than a few frames.**
  The ring is 4 frames (~130 ms). On the 3-vCPU runner default-QoS recorder
  threads sat off-CPU for 150 ms, and `VTCompressionSessionEncodeFrame`
  blocked 150-180 ms when six 1080p encoders fell behind; either way the
  ring overwrote 1-4 arrivals per file. Recorder threads run at
  user-interactive QoS, and the mac backend encodes on its own thread from a
  queue that counts in `videoBacklog()` (as MF's WriteSample queues).
- **AVAssetWriter does not hold video for a silent audio track** (backlog
  peak 9, 0 dropped), unlike the MF muxer. `maxVideoBacklog` stays the
  guarantee on both.
- **Sub-tick timestamps collapse at 90 kHz.** A preroll at 0 and a frame at
  100 ns rounded to one PTS and AVAssetWriter refused the append (-16364).
  The mac backend keeps video PTS strictly increasing in its own timescale.
- **The fake's clap frame is a flat Y=235.** A decoded average "> 235" only
  passed on Windows by encoder overshoot, and some fake guests' gradients
  average above 225. The validator calls a frame a clap when it is bright AND
  flat (average > 225, minimum > 215).
- **The node ignores SIGPIPE on POSIX** (a dead app, engine socket or FFmpeg
  pipe is a write error). FFmpeg children get the default back
  (`POSIX_SPAWN_SETSIGDEF`); every SRT pipe is close-on-exec and
  `POSIX_SPAWN_CLOEXEC_DEFAULT` passes the child only its three ends (the
  handle-inheritance law, POSIX form). The forked engine gets stdout -> stderr
  (stdout is the node's protocol) and no other descriptor.
- **The macOS CI runner is not a realtime machine.** A 5 ms sleep takes
  ~22 ms there (median, any QoS; `Platform.FiveMillisecondSleepPrecision`
  prints it) and the whole VM stalls 100-300 ms a few times a run. The
  recorder polls at ~30 Hz instead of 200 Hz, the fake's audio pacing lags its
  clap frame by 80-130 ms (run to run), six 1080p sources overrun the 4-frame ring now and
  then and a 60 fps source always does. So CI gates on four 30 fps sources
  with `--max-skew-ms 250 --max-lost 5` (there skew and ring overruns measure
  the VM, not Edge; both default to the product rule: 45 ms, 0 lost); the
  six-source take only reports; the writer's A/V placement is proven exact
  by a unit test (0.0 ms on both platforms). Do not "fix" CI by weakening a
  product rule; prove 6-8 sources on a real Mac.
- **Mac compiles and tests only in CI** (`.github/workflows/macos.yml`). Push
  to `mac-*`, `gh run watch`, `gh run view <id> --log-failed`; the validator log
  is in the `mac-native-diagnostics` artifact on failure. Never put SDK files
  in an artifact.

## Lifted code: keep it lifted

`native/src/modules|core|rpc`, `native/zoom-engine/`, `app/CoreVideoEdge.Core/Lifted`
and `app/CoreVideoEdge.Tests/Lifted` are CoreVideo Pro main @ `8751b8d2`,
verbatim, in Pro's paths and namespaces; `native/zoom-engine/engine/main-macos.mm`
and `scripts/make-macos-engine-bundle.sh` are Pro main @ `219ca1db`. Engine deltas are listed in
`native/zoom-engine/README.md`; keep that list exact. Edge-only code lives in
`native/src/edge/` and `app/CoreVideoEdge.Core/*.cs`. When re-syncing from Pro,
re-copy and re-apply the listed deltas. Do not edit lifted files casually.

## Build / test / prove

```powershell
$env:ZOOM_SDK_DIR = 'C:\Users\walla\Downloads\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292\x64'
.\scripts\build.ps1                                   # Release, both test suites
node scripts\validate-edge-iso.mjs --sources 6 --seconds 60
```

Ninja single-config: `build\bin\*.exe` is always the build you just made.
macOS (CI, or a Mac): `cmake -S native -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release`,
binaries at `build/mac/` (no `bin/`); the real engine with
`-DEDGE_BUILD_ZOOM_ENGINE=ON -DZOOM_SDK_DIR=<dir containing ZoomSDK.framework>`, then
`scripts/make-macos-engine-bundle.sh --build-dir build/engine`. README has the lot. The
app renders screens without a window:
`CoreVideoEdge.exe --screenshot <state.json> <screen> <out.png> [--preview-dir <dir>]`
(state lines and preview copies from `scripts/capture-ui-fixtures.mjs`; all of
`docs/screenshots` from `scripts/render-screenshots.ps1`). Look at every
screenshot after a UI change. Never drive the desktop; use the control API or the fake.

## Operator app rules

- Design source: the CoreVideo Edge design brief (tokens, type, screens).
  Tokens live only in `Theme/Tokens.axaml`; `TokenContrastTests` measures every
  pair. Components use token keys, never raw hex.
- A cell, button or value never shows a result the node has not confirmed
  (pending until the state line says so). Node refusal text is shown verbatim
  at its cause.
- Nothing unmeasured is drawn: no fps before the node measured it, no meter
  without a dBFS, no counts before the roster. After 10 s without a state line
  the screens' values are removed (the app polls `state` every second).
- View models (`ViewModels/`) hold the behaviour and are unit-tested for the
  JSON each action sends (`ViewModelCommandTests`). Views stay thin.
- Windows-only seams (`EdgePlatform.cs`): DPAPI token protection and the HKCU
  `corevideo://` registration. macOS: Keychain and `Info.plist` are TODO.

## Laws learned here (do not regress)

- **ISO head loss:** the engine's ISO ring is 4 frames deep. Open every writer,
  THEN take the shared epoch, THEN send `iso_video`, and map rings on every 5 ms
  poll. Arming first lost 24-33 head frames per ISO (more for later ISOs).
- **Writer size:** always open at 1920x1080 and conform frames into it. Opening
  at the first frame's size (Pro's rule) pins a file to Zoom's 360p ramp frame.
- **Audio placement:** a batch read at `now` ENDS at `now`. Anchoring its start
  to `now` turns every late poll into silence that is never removed (drift).
  Gaps under 40 ms are jitter, not pauses.
- **The MP4 muxer interleaves by time.** A track that stops advancing makes it
  hold every sample of the other track, and with throttling off that is
  unbounded (live 2026-11-09: guests who never spoke -> 50 GB, engine killed).
  Silent stems advance in real time (`IsoAudioTimeline::catchUp`, 200 ms
  holdback), a stalled camera repeats its held frame each second, and
  `IsoMp4Writer` refuses to queue past `maxVideoBacklog`. Zoom sends NO audio
  for a silent guest; the fake must be told to (`--silent N`).
- **Conform cost decides frame loss.** The ISO ring is 4 frames (~130 ms). Pro's
  box-filter conform took ~35 ms per 720p->1080p frame and lost ~20% live.
  `edge/FastConform.h` (bilinear, tables) takes ~2 ms. Zoom often delivers
  720p even on a 1080p subscription. Diagnose with the `[iso-diag]` log lines.
- **A failed join is followed by `left`.** Do not let `left` overwrite the
  engine's specific failure reason.
- **Fake clap vs AAC:** an every-sample-alternating burst is 24 kHz and AAC
  deletes it. The fake's clap is a 2 kHz square (engine README delta 2).
- **`MuteAudio(0)` mutes the whole meeting.** Capture-only mutes only the
  resolved self id, or reports `self_user_unavailable`.
- **Windows PowerShell 5.1 `Get-Content` reads BOM-less UTF-8 as ANSI.**
  Round-tripping a file through it double-encodes every non-ASCII character
  (it corrupted the vendored fake engine once). Edit with the editor tools or
  `[IO.File]::ReadAllText/WriteAllText` with UTF-8.
- The fake engine auto-subscribes everyone unless `COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE=0`;
  `EngineSupervisor` forces it off for every child.

## Not yet verified (be honest about it)

- macOS: the real engine has only been compiled and bundled (CI). Auth, join,
  capture-only muting, raw data, the ISO ring, breakout moves (does the mac
  SDK report `Disconnecting/Ended` + `InMeeting` like Windows?) and hardware
  VideoToolbox encoding under 8 x 1080p are unverified until a live run on
  the owner's Mac. NDI on macOS is untested (no runtime on the runner).

- Live A/V skew (there is no clap in a real meeting) and long live soaks. A live
  6-ISO 60 s take passed 2026-11-09 (signed-in join, raw data granted, 0 lost).
- Software-encoder fallback under load (6 x 1080p on CPU) and long soaks.
