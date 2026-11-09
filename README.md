# CoreVideo Edge

A second (or Nth) Windows machine that joins the **same Zoom meeting** as
CoreVideo Pro under **its own Zoom account**, so it has its own downlink
budget, subscribes to the guests Pro is **not** pulling, and records each one
as a self-contained ISO MP4.

Why: a Zoom Meeting SDK client is capped by its downlink (about 30 Mbps
standard, about 100 Mbps with Enhanced Media / HBM; a 1080p30 feed is about
4-6 Mbps). One CoreVideo Pro client tops out around 8 high-quality feeds. Each
Edge adds another full-quality budget. Resolution is never traded for count.

v1 is three features lifted from CoreVideo Pro, and nothing else:

1. **Zoom sign-in and join**: Pro's OAuth broker flow
   (`https://corevideo.iamfatness.us`), joins muted with the camera off.
2. **Sources**: the meeting roster; the operator (or `edge.json`) picks which
   participants this node subscribes to. Hard cap 6 at 1080p by default. The
   meeting is never auto-subscribed.
3. **ISO record**: one MP4 per selected source (that source's video plus its
   isolated, pre-mix audio, silence-filled on one shared epoch), plus
   `manifest.json`.

Those files are the v1 hand-off. Live relay to CoreVideo Pro is out of scope
for v1 (the relay sketched in `SPEC.md` is not implemented).

```
<RecordingFolder>/<prefix>-<yyyymmdd-hhmmss>/
  ISO-01-<SafeName>.mp4      H.264 1920x1080 (BT.709 full range, VFR) + AAC 48 kHz stereo
  ISO-02-<SafeName>.mp4
  manifest.json              session, node, meeting, shared epoch (steady clock + UTC), per-ISO health
```

## Layout

| Path | What |
|---|---|
| `native/zoom-engine/` | Pro's Zoom engine, vendored (same pattern Pro uses). Edge deltas listed in its README. |
| `native/src/modules`, `core`, `rpc` | Code lifted from Pro verbatim (engine process/IPC client, PTS clock, frame conform, naming, JSON, logging). |
| `native/src/edge/` | The node: `corevideo-edge.exe` (supervisor, subscriptions, ISO writer). |
| `app/` | Operator app `CoreVideoEdge.exe` (Avalonia, `CoreVideoEdge.Desktop`; Windows now, macOS next) + `CoreVideoEdge.Core` (node client, state, preview reader, Pro's lifted OAuth/token-store code) + tests. |
| `docs/brand/`, `docs/screenshots/` | The Satellite Tile mark and brand note; headless screenshots of every screen and state. |
| `scripts/` | `build.ps1`, `validate-edge-iso.mjs` (end-to-end check through the control API), `capture-ui-fixtures.mjs` + `render-screenshots.ps1` (UI screenshots from real fake-engine state). |
| `docs/PROTOCOL.md` | The node's JSON-line control API. |
| `config/edge.example.json` | Node config (display name, folder, prefix, cap, names to subscribe). |

## Build (Release, Windows x64)

Needs Visual Studio 2022+ with the C++ x64 tools, CMake 3.24+ and Ninja (both
ship with Visual Studio), the .NET 9 SDK, and for the real engine the Zoom
Meeting SDK for Windows. The SDK is **not** in git. Edge builds against the
same SDK as Pro (7.0.5.39292).

```powershell
$env:ZOOM_SDK_DIR = 'C:\path\to\zoom-sdk-windows-7.0.5.39292\x64'   # folder with h\zoom_sdk.h and lib\sdk.lib
.\scripts\build.ps1            # native node + real and fake engines + app, runs the native and .NET test suites
.\scripts\build.ps1 -Dist      # also stages dist\CoreVideoEdge (app + node + engine + SDK runtime) for an Edge machine
.\scripts\build.ps1 -NoZoomEngine   # a machine without the SDK: fake engine only
```

What `build.ps1` runs, if you want it by hand (from a VS x64 developer prompt):

```powershell
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DZOOM_SDK_DIR="$env:ZOOM_SDK_DIR"
cmake --build build
.\build\bin\corevideo-edge-tests.exe
dotnet build app\CoreVideoEdge.sln -c Release -p:Platform=x64
dotnet test app\CoreVideoEdge.Tests\CoreVideoEdge.Tests.csproj -c Release -p:Platform=x64
```

Outputs: `build\bin\corevideo-edge.exe`, `corevideo-zoom-engine.exe` (with the
SDK runtime copied beside it), `corevideo-zoom-engine-fake.exe`;
`app\CoreVideoEdge.Desktop\bin\x64\Release\net9.0\CoreVideoEdge.exe`.

## Prove it end to end (no meeting, no desktop)

`validate-edge-iso.mjs` drives `corevideo-edge.exe` only through its control
API against the fake engine. It joins, checks nothing is subscribed until
asked, checks the cap refuses one more than the cap (8 by default), subscribes N,
turns Engine Off and On (every feed stops, then returns with no re-routing),
checks the Monitor preview files are live with a measured fps, records, stops,
then checks every file with ffprobe and ffmpeg. Needs Node 18+ and ffmpeg/ffprobe
on PATH.

```powershell
node scripts\validate-edge-iso.mjs                          # 4 ISOs, 20 s, 30 fps
node scripts\validate-edge-iso.mjs --sources 6 --seconds 60  # the full cap
node scripts\validate-edge-iso.mjs --sources 6 --seconds 30 --fps 60
node scripts\validate-edge-iso.mjs --sources 6 --seconds 50 --silent 3   # 3 guests never speak
node scripts\validate-edge-iso.mjs --sources 6 --seconds 30 --room-move  # breakout move mid-take and back
node scripts\validate-edge-iso.mjs --sources 4 --seconds 15 --format 720/30 --fps 60   # record format
node scripts\validate-edge-iso.mjs --sources 3 --seconds 10 --feeds ndi  # NDI feeds, received by the probe
node scripts\validate-edge-iso.mjs --sources 3 --seconds 10 --feeds srt  # SRT feeds, received by FFmpeg listeners (Pro's role)
```

In a live meeting (the node must be signed in when the meeting refuses
anonymous apps; pass the ZAK through `COREVIDEO_ZOOM_USER_ZAK`, never on the
command line):

```powershell
node scripts\validate-edge-iso.mjs --engine zoom --meeting "<full join link with pwd=>" --sources 6 --seconds 60
```

It passes only if each ISO has h264 1920x1080 video **and** aac 48 kHz stereo
audio, both tracks end together, all ISOs end together, no arrival was lost,
the encoder never held more than 30 unwritten frames and dropped none,
the frame count matches the source rate, and the fake's synthetic clap
(a white frame plus an audio burst at the same instant) lines up within one
frame inside each file and across files.

To check the files yourself:

```powershell
ffprobe -v error -show_entries stream=codec_type,codec_name,width,height,sample_rate,channels -of compact "<session>\ISO-01-<name>.mp4"
```

Results on the dev box (2026-11-08, NVIDIA hardware H.264, fake engine):

| Run | Result |
|---|---|
| 6 ISOs, 60 s, 30 fps (3 runs) | 1801 frames each, 0 lost, files end within 0 ms of each other, clap A/V skew 8-33 ms depending on the run (constant within a file), clap spread across ISOs 1.1-1.5 ms |
| 6 ISOs, 30 s, 60 fps | 1800 frames each, 0 lost, clap A/V skew 16-18 ms, spread 2.8 ms |
| 6 ISOs, 50 s, 3 silent guests | node memory flat at ~530 MB, encoder backlog 0, silent ISOs carry a full-length silent track, clap spread 2.4 ms |
| **Live Zoom** (2026-11-09), 6 guests, 60 s | 0 lost, 0 dropped, encoder backlog 0, all ISOs end within 43 ms; node peak ~620 MB. Zoom delivered these guests at 1280x720 (upscaled into the 1080p files) |
| Breakout move mid-take (fake, `--room-move`) | node stays in the meeting, take keeps running, every routed guest back in their slot and received; guests absent from the room hold their frame and record silence |
| 720p / 30 fps constant from a 60 fps source | 1280x720, `avg_frame_rate=30/1`, clap alignment intact; 1080p/60 also passes |
| NDI feeds (fake) | probe receives `CoreVideo Edge <node> S<n>` at 1920x1080 ~30 fps, 48 kHz stereo, guest metadata |
| SRT feeds (fake, encrypted) | FFmpeg listener records h264 1920x1080 + aac 48 kHz; an unlistened slot says "Pro is not listening at host:port"; passphrase never in state or log |
| **Live Zoom** (2026-11-09), 8 guests (cap), 60 s | 8 ISOs, 0 lost, backlog 0-1, all 1080p + AAC ending together; all 8 previews live; NDI feeds received (with audio after the silent-guest fix) |

The first live take failed: guests who never spoke never advanced their audio
track, the MP4 muxer held every one of their video frames waiting for it, and
the node reached 50 GB until Windows ran out of memory and the engine died.
The fake had always sent every guest audio. Fixed (silent tracks now advance
in real time, a stalled camera repeats its held frame once a second, and the
writer refuses to queue past 60 frames), and the fake can now make silent
guests. The second live take then lost ~20% of frames to CPU scaling of 720p
frames into 1080p (35 ms per frame); a new scaler does it in 2 ms.

The A/V skew is a fake-engine artifact, measured and explained: the fake only
writes audio packets on its video tick, so its audio reaches the ring up to one
tick after its content time, and the lag at the moment a take starts (0 to one tick) stays for that take. Edge places audio by arrival time (Pro does the
same; the audio ring carries no source timestamps). The skew halves when the
fake runs at 60 fps. The AAC path itself adds 0 samples (unit-tested). Live
Zoom delivers audio every 10 ms. **The live-meeting number has not been
measured yet.**

## Feeds to CoreVideo Pro (NDI / SRT)

Routing > **Send to Pro** (or `{"cmd":"set-feeds",...}`, see PROTOCOL.md) sends
every routed slot to Pro live. Identities follow the slot, not the guest, so a
Pro input stays wired when the guest in a slot changes.

- **NDI**: in Pro, add an NDI input and pick `CoreVideo Edge <node> S<n>` (Pro
  discovers NDI sources on the LAN; Windows Pro only today). Source size and
  rate pass through. Needs the NDI 6 Runtime (or NDI Tools) on the Edge machine.
- **SRT**: in Pro, turn on SRT inputs (listener mode; input n listens on
  `10000 + n - 1` by default). On Edge set Pro's address, the base port,
  latency (Pro's default 120 ms) and an optional passphrase (same on both
  sides). Each slot calls Pro's matching port: 1080p H.264 at 30 fps + AAC.
  Needs FFmpeg on the Edge machine (PATH or `COREVIDEO_FFMPEG_DIR`).
- Pro re-stamps incoming NDI/SRT media on its own clock; Pro's per-input audio
  offset (+-500 ms) is the trim if a feed needs one.

Design: `docs/superpowers/specs/2026-11-09-edge-to-pro-ndi-srt-design.md`.

## Run the operator app

```powershell
.\app\CoreVideoEdge.Desktop\bin\x64\Release\net9.0\CoreVideoEdge.exe          # real Zoom
.\app\CoreVideoEdge.Desktop\bin\x64\Release\net9.0\CoreVideoEdge.exe --fake   # synthetic meeting (header says FAKE ENGINE)
```

The app finds `corevideo-edge.exe` beside itself, then in `build\bin` up the
tree, or at `COREVIDEO_EDGE_NODE`. Config lives in
`%LOCALAPPDATA%\CoreVideoEdge\edge.json`; the node log in `edge-node.log`
beside it; Zoom tokens in `zoom-oauth.json` (DPAPI-encrypted, current user).
Screens: **Connect**, **Routing**, **Monitor**, **Record** (Ctrl+1-4). The
header always shows the meeting, Engine, Routed n/cap, ISO n/cap, measured disk
free and the REC tally; failures sit in a banner under it until they clear.

1. **Connect: sign in with Zoom** using this node's own Zoom account, not the
   account CoreVideo Pro uses. Two clients on one account collide. The app
   registers `corevideo://` (the only return URI the broker accepts) for the
   current user when you click Sign in. On a machine that also runs CoreVideo
   Pro, whichever app signed in last owns that link.
2. **Connect: join.** Paste the full join link (with `pwd=`). The node joins
   with its camera off and its mic muted (enforced for the whole meeting).
3. **Connect: Engine.** The latching Engine key asks Zoom for raw media (and
   the host for record permission). It shows Requesting until the engine
   reports raw media active. Tokens also work: `COREVIDEO_ZOOM_ON_BEHALF_TOKEN`,
   `COREVIDEO_ZOOM_USER_ZAK`, `COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN`.
4. **Routing.** A crosspoint grid: click a Subscribe cell to route a guest (the
   node assigns the next free slot, up to the cap), the ISO band arms that
   slot's ISO. Cells stay pending until the node confirms; refusals appear on
   the row. **Send to Pro** sends every routed slot to CoreVideo Pro over NDI or
   SRT (see `docs/PROTOCOL.md` `set-feeds`).
5. **Monitor.** One tile per routed slot from the node's preview files
   (`docs/PROTOCOL.md` "Preview frames"): picture, measured fps and size,
   audio meter, ISO and feed health. Enter opens a larger view.
6. **Record.** Folder, file prefix, 1080p | 720p and Source | 30 | 60, then
   **Start recording**. The table shows frames, lost, held, cadence and size per
   ISO. **Stop recording** (confirmed) finalizes every file and the manifest.

Screenshots of every screen and state: `docs/screenshots/`. They are rendered
headlessly (no window) from node state lines captured off the fake engine:

```powershell
node scripts\capture-ui-fixtures.mjs build\bin\corevideo-edge.exe out\captures D:\EdgeISO main   # also: move, feeds
.\scripts\render-screenshots.ps1
# one screen: CoreVideoEdge.exe --screenshot <state.json> <connect|routing|monitor|record> <out.png> [--preview-dir <dir>] [--signed-in]
```

### macOS (not built yet)

`CoreVideoEdge.Core` and the app target plain `net9.0`. Two seams are
Windows-only today and fail loudly elsewhere: token protection
(`EdgeSecretProtection`: DPAPI on Windows; macOS refuses to store tokens until
the Keychain store is written) and the `corevideo://` registration
(`IUrlSchemeRegistrar`: HKCU on Windows; on macOS the scheme must be declared in
the bundle's `Info.plist` `CFBundleURLTypes`, TODO). Reduced motion reads
`SPI_GETCLIENTAREAANIMATION` on Windows only.

Headless (no UI), the same node with a config file:

```powershell
build\bin\corevideo-edge.exe --config config\edge.example.json   # then JSON lines on stdin, see docs/PROTOCOL.md
```

## Rules this repo keeps

- Never commit Zoom SDK files, tokens, ZAK, or client secrets. `build\` and
  `dist\` hold the SDK runtime and are git-ignored.
- A value the operator sees is measured, or absent. Never a placeholder.
- A bad folder or low disk refuses the take. There is no silent `%TEMP%` write.
- Test through the control API or the fake engine. Never drive the desktop.

## Related

- [CoreVideo Pro](https://github.com/iamfatness/CoreVideoPro), the source of every lifted file (main @ `8751b8d2`)
- [CoreVideo OBS plugin](https://github.com/iamfatness/CoreVideo)
