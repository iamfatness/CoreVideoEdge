# CLAUDE.md: working guide for CoreVideo Edge

Product and commands: `README.md`. Control API: `docs/PROTOCOL.md`. Update this
file in the same change as any substantive work.

## Scope (owner correction, 2026-11-08)

Edge v1 is exactly: Zoom sign-in + join, Sources (subscribe a subset), ISO
record. Do **not** add a live relay, NDI/SRT/TCP sender, compositor, scenes,
graphics, program output, streaming, virtual camera, or Pro-side ingest. Never
downgrade resolution to fit more people on one node: the point of a second
machine is another full-quality budget.

## Processes

- `CoreVideoEdge.exe` (WPF, `app/`): sign-in, join, sources, record, health.
  Owns no media. Spawns the node and speaks its JSON-line API.
- `corevideo-edge.exe` (C++, `native/src/edge/`): the node. Supervises the
  engine, enforces the subscription cap, records ISOs (Media Foundation).
- `corevideo-zoom-engine.exe` / `-fake.exe` (`native/zoom-engine/`): Pro's
  engine, vendored. I420 + audio rings in shared memory.

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
app preview renderer checks layout without showing a window:
`CoreVideoEdge.exe --render-preview <state.json> <out.png>` (capture a state
line from the node first). Never drive the desktop; use the control API or the fake.

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

- Live A/V skew (there is no clap in a real meeting) and long live soaks. A live
  6-ISO 60 s take passed 2026-11-09 (signed-in join, raw data granted, 0 lost).
- Software-encoder fallback under load (6 x 1080p on CPU) and long soaks.
