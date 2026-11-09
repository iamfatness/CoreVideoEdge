# CoreVideo Edge v2: routing, monitor, Engine On, breakouts, record format, Mac-ready

Date: 2026-11-09. Status: written and executed autonomously at the owner's
request ("execute on all of this while I am at work today and I will review
when I get home"). Calls marked **[my call]** were not decided by the owner and
are open to change at review.

## Why

v1 (PR #1) proved the node: a second machine joins the same Zoom meeting under
its own account, subscribes up to N guests at 1080p and records one ISO per
guest. Its first live use (2026-11-09) exposed what an operator actually needs:

- More than 6 feeds per node (the engine's ISO table holds 8).
- To *see* every routed guest, not read a table.
- A routing model, not checkboxes: who is pulled, and who is recorded.
- Breakout rooms: Zoom moves the node between rooms; v1 treated that as the
  meeting ending and fell out.
- "Engine On": the toggle Pro operators use to request raw media and, where the
  account lacks record rights, to make the engine ask the host.
- A choice of recording resolution and frame rate.
- macOS next, without a rewrite.
- A product that looks as good as Pro, with its own mark.

## Goals

1. Cap 8 routed sources by default (1..8 configurable), each at 1080p.
2. Routing screen: Zoom participants, filtered to video-on by default (All
   available), with a **Subscribe** routing band and an **ISO** routing band.
3. Monitor screen: a live picture of every routed / ISO'd source with measured
   health (received size, measured fps, frames lost, audio level, ISO state).
4. Engine On toggle: On requests raw media (and host record permission); Off
   releases it; toggling is the operator's re-request.
5. Survive breakout-room moves (and SDK reconnects) without losing the
   routing or the take.
6. Record format: 1080p or 720p; frame rate as delivered, 30 or 60 constant.
7. Windows first; every platform-specific piece behind a seam so macOS is a
   port, not a rewrite.
8. A logo and a visual language that is family with CoreVideo Pro and Volt.

## Non-goals (this round)

- Compositing, scenes, graphics, streaming, virtual camera.
- Live relay to Pro: designed separately in
  `2026-11-09-edge-to-pro-ndi-srt-design.md`.
- Picking breakout rooms: the node goes where Zoom sends it **[my call]**.
- Shipping the macOS build (designed for; built in CI later).

## Anti-goals

- Never trade resolution for count (720p is an explicit record choice, never
  an automatic downgrade).
- Never show a value that was not measured; never a placeholder or demo frame.
- Never fail silently: a frame not written, an audio gap or a held picture is
  counted and labelled.

## Decisions

### Node (C++, `native/src/edge`)

| Topic | Decision |
|---|---|
| Cap | Default 8 (`kDefaultMaxSubscriptions`), config 1..8. 8 x 1080p needs Zoom Enhanced Media (HBM) on the node's account: standard downlink is ~30 Mbps, eight feeds are ~32-48 Mbps. |
| Engine On | `{"cmd":"engine","on":bool}`. On sends `start_media` (the engine runs `CanStartRawRecording`, else `RequestLocalRecordingPrivilege` = asks the host); Off sends `stop_media`. Refused Off while recording **[my call]**: it would black every ISO. State: `engineOn` (choice) vs `rawMedia.active` (engine's report). |
| Re-arm | The engine forgets every subscription on stop / room move (`unsubscribe_all`). Edge re-sends all routed subscriptions (and ISO arms) on every inactive->active `raw_media_status` edge. (Pro's research: Pro does not, so Pro likely goes dark after a breakout until Engine Off/On. Inferred, not tested.) |
| Breakouts | The engine forwards SDK `meeting_status`. Status 14/15 (JOIN/LEAVE_BREAKOUT_ROOM) or 5 (RECONNECTING) marks `movingRooms`; the following `left` keeps the engine, the routing slots and the take; `joined` re-arms. Guests re-attach by same Zoom id (new: fixed a latent bug where a same-id return never re-marked a slot present) or same display name. A move that never arrives anywhere in 90 s becomes a real leave. **The take keeps running across a move [my call]**: absent guests hold their last frame and record silence, counted. |
| Record format | `{"cmd":"set-record-format","height":1080|720,"fps":0|30|60}`. 16:9 at that height. fps 0 = variable (source timing, the v1 behaviour); 30/60 = every sample on the grid: a second arrival in one slot replaces the held frame (`cadenceSkipped`), an empty slot repeats it (`cadenceRepeats`). |
| Previews | `PreviewPublisher`: 15 Hz, 480x270 BGRA per routed slot in a memory-mapped file (`<previewDir>/slot-N.cvp`, format in PROTOCOL.md). File mapping because it is identical on Windows and POSIX for C++ and .NET. fps is measured from the engine's frame sequence (every delivered frame), audio level from the guest's isolated ring. |
| Live-found defects (v1, fixed first) | Silent guests stalled the MP4 muxer (50 GB): audio catch-up + held-frame repeat + a 60-sample writer cap. 720p->1080p conform at 35 ms/frame lost 20%: `FastConform` 2 ms. |

### Operator app (`app/`)

- **Avalonia** replaces WPF **[my call, stated to the owner]**: same C#, the
  lifted Pro OAuth code is reused unchanged, and it runs on Windows and macOS.
- Screens and visual system: `docs/design/edge-design-brief.md` (researched
  from impeccable, UI UX Pro Max, 21st.dev patterns, Pro's and Volt's tokens).
  Shell with a persistent status cluster and failure banner; Connect (sign in,
  join, latching Engine button); Routing (crosspoint grid: participants x
  slots, Subscribe band + ISO band); Monitor (16:9 tiles, stats strip under the
  picture, vertical audio meter, health border and slates); Record (folder,
  Resolution and Frame-rate segmented controls, per-ISO table).
- Logo: direction B "Satellite Tile": a small Pro-style multiview frame with a
  solid Edge-cyan tile docked off its lower-right corner; only the tile changes
  colour (red while recording, amber when degraded). `docs/brand/`.
- Accent: Edge cyan `#3CC4E0`, never a status colour; Pro's tallies unchanged.

### macOS seams (designed now, built later)

| Piece | Windows today | macOS route |
|---|---|---|
| Zoom engine | vendored `main.cpp` (+ ISO ring) | Pro's `main-macos.mm` (ZoomSDK.framework 7.1.5) **lacks `iso_video`/the ISO ring**: port it; `.app` bundle with the SDK in `Contents/Frameworks` (`make-macos-engine-bundle.sh`). |
| Engine IPC / SHM | named pipes, file-mapping sections | already in `engine-ipc.h`: unix sockets, `shm_open` with hashed names. Edge's `EngineSupervisor` uses the lifted `ZoomEngineProcess`, which has the POSIX spawn path. |
| ISO writer | Media Foundation (`IsoMp4Writer`) | an `AVAssetWriter` + VideoToolbox implementation behind the same interface (open / writeVideo / writeAudio / writeSilence / repeatHeldVideo / finalize / counters). |
| Scaler, timeline, policy, previews | portable C++ | as is (`PreviewPublisher` already has the POSIX `mmap` path). |
| App | Avalonia on Windows | same app; secret store seam (DPAPI -> Keychain), `corevideo://` registration seam (registry -> `CFBundleURLTypes`). |
| Build | local + scripts/build.ps1 | GitHub Actions macOS job; the mac Zoom SDK supplied as a private artifact (never committed). Hands-on QA on the M3 Max. |

## Verification (what "done" means)

- Native unit tests (writer, scaler, timeline, policy, previews, config).
- `scripts/validate-edge-iso.mjs` against the fake engine, through the control
  API only: standard 6 x 60 s; `--silent N`; `--room-move` (breakout and back,
  take continues, routing restored); `--format 720/30`; Engine Off/On on
  every run; preview files live with measured fps matching the source.
- One live 6-guest take in the owner's test meeting (done for the v1 fixes).
- App: headless screenshots of every screen state (`docs/screenshots/`),
  token-contrast tests, state-parse and preview-reader tests.

## Left to the builder / open at review

- Confirm the take-across-breakout behaviour, and Engine Off refusal while
  recording.
- Whether a breakout room needs a fresh host record grant (unverified live).
- Zoom delivered 720p on a 1080p subscription live; the sender side decides.
