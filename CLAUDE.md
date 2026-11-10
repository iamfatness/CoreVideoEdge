# CLAUDE.md: working guide for CoreVideo Edge

Product and commands: `README.md`. Control API: `docs/PROTOCOL.md`. Update this
file in the same change as any substantive work.

## Scope

- 2026-11-08 (v1): Zoom sign-in + join, Sources (subscribe a subset), ISO record.
- 2026-11-09 (owner, v2): routing (Subscribe + ISO, cap 8), Monitor with live
  previews, Engine On, breakout-room survival, record format (1080p/720p,
  source/30/60 — now per slot on Routing), live **feeds to Pro over NDI and SRT** (per routed slot), a
  logo + Pro/Volt-family design, Windows first and designed for macOS.
  Specs: `docs/superpowers/specs/2026-11-09-*`.
- 2026-11-09 evening (owner): Routing never hides or greys a crosspoint and
  never reflows on a click: every cell is live (a click takes the guest to
  that slot: route, move, or replace the occupant; an ISO cell on another
  column does the same with the ISO armed); pending shows only in the cell.
  Per slot, chosen on Routing (toolbar default + row pickers): **Zoom feed
  resolution** (1080p default / 720p / 360p, `set-resolution`) and **frame
  rate** (Source / 30 / 60, `set-fps`) for that slot's ISO and SRT feed; the
  Record tab no longer has a rate. Delivered size shows once, under the slot
  header. Monitor tiles scale to fit the window (`MonitorView.BestItemWidth`).
- 2026-11-09 night (owner): **Comms** -- the ZComms talkback station folded
  into Edge (and later Pro) in our look. Slice 1 (engine + node, this repo):
  Zoom SDK 7.1.5, talkback inside the Zoom engine (`engine-comms`, ported
  ZComms under `native/zoom-engine/engine/comms/`), fake emulation, node verbs
  `comms-*` and the `comms` state object (`docs/PROTOCOL.md`). Spec:
  `docs/superpowers/specs/2026-11-09-comms-design.md`. Needs this node's Zoom
  account to be host or co-host; video never depends on it.
- Still out: compositor, scenes, graphics, program output, streaming, virtual
  camera. Never downgrade resolution automatically to fit more people on one
  node: 1080p unless the operator explicitly picks 720p/360p for a slot.

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
  engine, vendored. I420 + audio rings in shared memory. Since Comms it is
  also the talkback station (README deltas 7/8): the comms core is shared by
  the real and fake engines so both run the same ladder.

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
- **The v1 app wrote `"maxSubscriptions": 6` into every edge.json.** An
  upgraded machine stayed capped at 6 under the v2 node's default of 8 (owner
  hit it live). `EdgeSettings.EnsureExists` migrates a v1 file holding exactly
  6 to 8 once and stamps `configVersion: 2`; a value chosen after that stays.
- **Requested is not delivered.** The engine asks 1080p and falls back a tier
  only when `subscribe()` refuses; Zoom may still SEND 720p on a granted
  1080p subscription. `subscriptions[].zoom` + `[zoom-subscribe]` log lines
  tell the two apart; the fake reproduces a refusal with
  `COREVIDEO_FAKE_REFUSE_1080=1` (engine README delta 5). The SDK exposes no
  receive-side frame-rate request: 60 fps arrives only if the guest sends it.
- **Changing a slot's resolution is unsubscribe + subscribe.** The fake (and
  the engine's in-place path) honour only upgrades on a re-subscribe.
- **Validator `--sources 8` fails the clap check on ISO-08 on main too**
  (23 video claps vs 10 audio: false video-clap detections on the fake's 8th
  guest). Pre-existing, not a sync defect; untriaged.
- **A take never greys or reflows.** Cells only lock while a take records the
  slots involved; the node refuses a move/replace touching a recording slot.
  A "Waiting for the node" text line under the row pushed every row below
  down and back up on each click (owner: "refreshes weird"): never reintroduce it.
- **Constant 60 from a 30 fps source probes as 59.99** (`avg_frame_rate`
  27000000/450059) on main too: the validator's `--format 1080/60 --fps 30`
  check fails; 30 from 60 is exact. Pre-existing, untriaged.
- **The app is a GUI-subsystem exe.** PowerShell `&` does not wait for it and
  leaves `$LASTEXITCODE` empty; `render-screenshots.ps1` waits on the process.
- **Tooling trap:** in this environment the Bash tool collapses `\\n` inside
  heredocs (python/JS strings and regexes came out with real newlines). Put
  scripted edits with backslashes in a file written by the Write tool, or use
  Edit.

## Laws learned 2026-11-09 (Comms slice 1)

- **The engine's command dispatch is substring matching** (`line.find("join")`,
  `"quit"`, `"leave"`). Any new verb that carries free text (device names)
  must be routed BEFORE it, by exact command name (`comms_protocol.h`).
- **The real mic can never be open to the room.** Talkback is SILENT while
  our mic is muted (ZComms law 1), so Comms opens it -- but only behind a
  never-fed virtual mic, and `setExternalAudioSource` returning SUCCESS proves
  nothing: without the raw-data licence it can succeed and do nothing
  (ZComms). So: require `HasRawdataLicense()`; ownership is PROVISIONAL until
  `onMicInitialize` (1.5 s grace), and any drop (`onMicUninitialized`, flag
  false) clears ownership FIRST then `MuteAudio(self)` at once, giving up for
  the meeting. All of it lives in `comms/mic_guard` (pure, tested); the engine
  only performs. Never "fix" a closed mic by unmuting.
- **Mic ownership is cleared synchronously on every meeting status change**
  (`EngineComms::on_meeting_status`, first line of `onMeetingStatusChanged`).
  A tick-driven reset left `owns_mic()` true across a fast leave/rejoin, so
  the attach-time capture-only mute was skipped. MEETING_STATUS_LOCKED/
  UNLOCKED fire while still in the meeting: ignore them.
- **A non-capture-only join is never given a virtual mic or unmuted** (Pro's
  operator speaks in the meeting); Comms only reports their mute.
- **A held talk key needs an owner.** stdin EOF quits the node, which sends
  `comms_release_all` first; `comms-key` `leaseMs` covers a hung-but-alive
  app.
- **Only one listener per SDK controller.** `EngineParticipants` owns the
  participants controller's event slot; Comms polls the roster (1 s). Never
  `SetEvent` a second listener on a controller something else owns.
- **Auto-assign waits for the bank to form.** Assigning while
  `CreateChannel(16)` answers trickle in piled every guest onto channel 1
  (fake probe). All 16, or 5 s without growth.
- **Device enumeration and WASAPI opens never run on the SDK thread** (it is
  the thread video callbacks and the command loop share). They run on the
  comms audio worker; the SDK thread only does quick getters and the
  talkback calls.
- **The meter must not drive the state line**: `CommsStateEmitter` sends a
  meter-only change at most every 250 ms; anything else at once.
- **A range-for over `json().get(..)->asArray()` on a temporary `Json`
  dangles** (the array lease dies with the temporary). Keep the tree in a
  local first.
- **`IsoMp4WriterTest` line 220 is timing-flaky** (121 vs 120 frames, about
  1 run in 3 on this box, seen before any Comms code). Untriaged.

## Lifted code: keep it lifted

`native/src/modules|core|rpc`, `native/zoom-engine/`, `app/CoreVideoEdge.Core/Lifted`
and `app/CoreVideoEdge.Tests/Lifted` are CoreVideo Pro main @ `8751b8d2`,
verbatim, in Pro's paths and namespaces. Engine deltas are listed in
`native/zoom-engine/README.md`; keep that list exact. Edge-only code lives in
`native/src/edge/` and `app/CoreVideoEdge.Core/*.cs`. When re-syncing from Pro,
re-copy and re-apply the listed deltas. Do not edit lifted files casually.

## Build / test / prove

```powershell
$env:ZOOM_SDK_DIR = 'C:\Users\walla\Downloads\zoom-sdk-windows-7.1.5.43953\zoom-sdk-windows-7.1.5.43953\x64'   # 7.1.5 since Comms (talkback)
.\scripts\build.ps1                                   # Release, both test suites
node scripts\validate-edge-iso.mjs --sources 6 --seconds 60
node scripts\probe-comms.mjs                          # Comms through node + fake (key/assign/tone/latch)
```

On the dev box `scripts\build.ps1` fails on vcvars stderr; build from a
`.cmd` that calls `vcvars64.bat >nul 2>nul`, then `cmake -G Ninja` +
`cmake --build`.

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

- **Comms against real Zoom: nothing.** The real engine compiles and links on
  SDK 7.1.5; the talkback path, the silent virtual mic (including whether
  `onMicInitialize` really arrives within 1.5 s of the install on a licensed
  sign-in -- if not, the guard mutes and talkback is silent), unmute under
  capture-only, channel creation as co-host, delivery to a desktop guest, the
  duck, breakout moves (channel re-creation vs the 16 cap) and raw video/ISO
  on 7.1.5 in a live meeting are all unproven. Only the fake (unit tests,
  probe, validator) has run.

- Live A/V skew (there is no clap in a real meeting) and long live soaks. A live
  6-ISO 60 s take passed 2026-11-09 (signed-in join, raw data granted, 0 lost).
- Software-encoder fallback under load (6 x 1080p on CPU) and long soaks.
