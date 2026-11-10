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

## Lifted code: keep it lifted

`native/src/modules|core|rpc`, `native/zoom-engine/`, `app/CoreVideoEdge.Core/Lifted`
and `app/CoreVideoEdge.Tests/Lifted` are CoreVideo Pro main @ `8751b8d2`,
verbatim, in Pro's paths and namespaces. Engine deltas are listed in
`native/zoom-engine/README.md`; keep that list exact. Edge-only code lives in
`native/src/edge/` and `app/CoreVideoEdge.Core/*.cs`. When re-syncing from Pro,
re-copy and re-apply the listed deltas. Do not edit lifted files casually.

## Build / test / prove

```powershell
$env:ZOOM_SDK_DIR = 'C:\Users\walla\Downloads\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292\x64'
.\scripts\build.ps1                                   # Release, both test suites
node scripts\validate-edge-iso.mjs --sources 6 --seconds 60
```

Ninja single-config: `build\bin\*.exe` is always the build you just made. The
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
- Platform seams (`EdgePlatform.cs`): token protection (Windows DPAPI; macOS
  `KeychainSecretProtector`, AES-256-GCM under a key in the login Keychain) and
  the `corevideo://` return (Windows HKCU + second-instance pipe; macOS
  `Info.plist` CFBundleURLTypes + Launch Services, delivered as an Apple Event).
  Every callback enters through `App.Callbacks` (`OAuthCallbackInbox`).

## macOS app (branch mac-app; Apple Silicon, macOS 13+)

- No local Mac: every Mac build/test/package runs in `.github/workflows/macos.yml`.
  Job `app-only` is the fast loop (no engine, node optional): .NET tests (the
  `[MacOnlyFact]` Keychain tests run for real there), `package-macos-app.sh`,
  `ci/check-macos-app.sh`, `render-screenshots.sh` -> artifact `mac-screenshots`.
  `workflow_dispatch` with `app_only` skips the native/engine/full-app jobs.
- Bundle: `CoreVideo Edge.app/Contents/MacOS/{CoreVideoEdge (self-contained
  .NET), corevideo-edge, corevideo-edge-ndi-probe}`,
  `Contents/Helpers/corevideo-zoom-engine.app` (the node finds it at
  `<nodeDir>/../Helpers/...`), `Resources/CoreVideoEdge.icns` (rendered by the
  app itself: `--render-brand` writes `icon-mac-1024.png` on Apple's 824/1024
  grid), `Resources/edge-bundle.txt` (what is in it: node/engine present or
  MISSING). Bundle id `us.iamfatness.corevideo.edge`; version = the csproj.
- A bundle with the engine contains the Zoom SDK: private draft release only
  (`ci/upload-private-asset.sh`), never an Actions artifact. Screenshots are
  fine (fixture/fake state, no SDK content).
- Signing is ad hoc, inside out (Mach-Os in MacOS first so the app can render
  its icon, then the engine bundle only if its own signature is broken, then
  the bundle). No Developer ID/notarization yet: first open is right-click >
  Open or `xattr -dr com.apple.quarantine`. An ad-hoc re-sign changes the code
  identity, so after an update macOS asks again for the Keychain key ("Always
  Allow").
- Mac feel: title bar extended into the 48 px header (traffic lights, left
  inset `MacTrafficLightInset`, header drags the window), Ctrl gestures become
  Command (`Shortcuts.ForPlatform`, tooltips `Shortcuts.*`), `MacMenus` (About,
  Settings = Record screen with Cmd+comma, Quit via Avalonia's app menu, View
  Cmd+1-4, Help). Quit goes through `OnClosing`: a take is confirmed first.
  Windows is untouched by all of it (guarded by `OperatingSystem.IsMacOS()`).
- `PlatformTarget` x64 applies only on Windows; mac publishes osx-arm64.

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

- macOS, needs a real Mac: the Keychain prompt and "Always Allow" flow, the
  corevideo:// round trip from the browser (Apple Event while running and on a
  cold launch), Launch Services default-handler switching against CoreVideo
  Pro, Gatekeeper first-open, traffic-light placement in the 48 px header, the
  menu bar, and the Local Network prompt for NDI/SRT. CI proves the bundle
  builds, signs, maps corevideo:// in Launch Services, runs headless, and that
  SecItem add/read/update/delete works on the runner.

- Live A/V skew (there is no clap in a real meeting) and long live soaks. A live
  6-ISO 60 s take passed 2026-11-09 (signed-in join, raw data granted, 0 lost).
- Software-encoder fallback under load (6 x 1080p on CPU) and long soaks.
