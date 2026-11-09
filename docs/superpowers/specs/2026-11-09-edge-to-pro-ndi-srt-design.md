# Edge -> CoreVideo Pro live feeds over NDI and SRT

Date: 2026-11-09. Status: designed and being executed autonomously at the
owner's request. Supersedes the v1 note "live relay to Pro is out of scope".
**[my call]** marks decisions the owner has not made.

## Why

Edge exists so one show can pull more full-quality Zoom guests than one Pro
client's downlink allows. ISO files are an after-the-fact hand-off; the show
needs those guests **live in Pro**: each guest an Edge node is routing should
appear in Pro as a routable input, with low latency, at full quality.

## What Pro already does (research, file:line in Pro's repo)

- **NDI ingest** (Windows): `NdiReceiveCaptureAdapter.cpp`. Discovers sources
  on the LAN (finder, `showLocal=true`), the operator picks one; the device id
  is a hash of the **NDI source name**. Receives BGRX/BGRA, re-stamps video and
  audio with Pro's own render tick (NDI timecodes ignored); manual +-500 ms
  audio offset per device. Runtime loaded dynamically
  (`Processing.NDI.Lib.x64.dll`, NDI 5/6 runtime paths, `NDI_RUNTIME_DIR`).
- **SRT ingest**: `SrtIngestCaptureAdapter.cpp`, one FFmpeg decoder per source.
  Config: mode (default **listener**), host 0.0.0.0, port **10000 + n - 1**,
  latency 120 ms, streamid, passphrase (>= 10 chars). Output scaled to the
  source's configured size (default 1920x1080 at 60); PTS discarded, re-stamped
  at Pro's tick.
- Pro has **no Edge / relay / remote-node concept** yet. Its NDI and SRT
  *outputs* send the program only.

So a first version needs **no Pro change**: Edge sends each routed guest as an
NDI source or as an SRT caller into Pro's listener ports, and the Pro operator
routes them like any other input.

## Goals

1. Per Edge node, a **Send to Pro** mode: Off | NDI | SRT **[my call: one mode
   per node, not per slot]**. Every routed slot is sent while the mode is on.
2. **Stable identities**: a feed is named by node + slot, not by guest, so a
   Pro input stays wired when the guest in that slot changes or rejoins.
   NDI name: `CoreVideo Edge <NodeName> S<n>` (Pro's device id = hash(name)).
   SRT: slot n -> `<proHost>:<basePort + n - 1>` (Pro's default listener map).
3. Full quality: source resolution and rate pass through (no downscale);
   isolated per-guest audio (the same pre-mix stem the ISO records).
4. Measured health per feed in Edge's state and Monitor: frames sent, frames
   dropped, receiver connected (NDI: `NDIlib_send_get_no_connections`; SRT:
   the FFmpeg caller's connection), last error.
5. Loud failure: runtime missing (NDI), FFmpeg missing (SRT), Pro not
   listening, a feed falling behind: each is a stated warning, never silence.

## Non-goals (v1 of feeds)

- Pro-side changes (labels from Edge metadata, auto-discovery of Edge nodes,
  health back-channel to Pro's UI). Listed as follow-ups.
- Program/composite output from Edge.
- WAN transport tuning beyond SRT latency/passphrase.

## Design

### Frame source

A `FeedSource` thread per routed slot reads the engine's **main video region**
(`<prefix><uuid>`, latest frame, sequence-checked; the same region the preview
reads) at a 2 ms poll and the guest's **isolated audio ring** with its own
cursor. It does not use the ISO ring: that ring belongs to the recorder and is
epoch-armed per take. Frames are passed at source size (I420), with the
node's capture time; audio as 48 kHz stereo float, with **silence filled in
real time** while a guest is quiet (the ISO lesson: receivers and muxers stall
on a track that stops).

### Sinks (one interface, two transports)

```
class FeedSink {
  virtual bool open(const FeedTarget&, std::string& error) = 0;
  virtual void video(const uint8_t* i420, int w, int h, int64_t capture100ns) = 0;
  virtual void audio(const float* stereo, int frames, int64_t capture100ns) = 0;
  virtual FeedHealth health() const = 0;   // measured only
};
```

- **NdiFeedSink**: dynamic NDI runtime (same candidate list as Pro, plus
  `libndi.dylib` on macOS). `NDIlib_send_create` with the stable name,
  `clock_video = clock_audio = false` (frames go out as they arrive; Pro
  re-stamps anyway). Video sent as **I420** FourCC at source size (no
  colour conversion on Edge); audio via planar float 48 kHz stereo.
  Per-frame metadata XML carries the guest's display name and Zoom id for a
  future Pro label. Health: frames sent, `get_no_connections` (> 0 = Pro is
  receiving), dropped = arrivals skipped while the previous send was still in
  flight.
- **SrtFeedSink**: one FFmpeg process per slot (Pro's own SRT stack is FFmpeg).
  Video: raw I420 on stdin at the source size; size changes (Zoom's 360p ->
  1080p ramp) restart the encoder at the new size **[my call]** or are
  conformed to the configured size: conform to 1080p (FastConform) so the SRT
  stream never changes size mid-stream. Audio: f32le 48 kHz stereo on a second
  pipe (Windows named pipe; POSIX `pipe:3`). Encode `h264_nvenc` (`-preset p1
  -tune ll -zerolatency 1 -g <fps> -bf 0`), fallback `libx264 -preset
  ultrafast -tune zerolatency`; AAC 48 kHz; `-f mpegts
  "srt://<host>:<port>?mode=caller&transtype=live&latency=<us>[&passphrase=..&pbkeylen=16]"`.
  Health: frames written to the pipe, FFmpeg alive, connection state parsed
  from FFmpeg's log (a connection refusal is reported as "Pro is not listening
  on host:port"), dropped = frames skipped because the pipe was full.

### Control API

- `{"cmd":"set-feeds","mode":"off|ndi|srt","srtHost":"10.0.0.5","srtBasePort":10000,"srtLatencyMs":120,"srtPassphrase":"..."}`
  (passphrase never logged or echoed in state).
- State: `feeds: { mode, srtHost?, srtBasePort?, slots: [ { slot, name|url,
  state: "starting|sending|failed", framesSent, framesDropped,
  receivers (NDI), connected (SRT), error } ] }`.

### App

A "Send to Pro" block on the Routing screen: segmented Off | NDI | SRT; SRT
host/port/latency/passphrase fields; per-slot feed health shown on the Monitor
tile strip (`NDI 1 rx` / `SRT connected`).

## Verification

- Unit: target naming (stable per slot), SRT URL building and redaction,
  silence fill on the feed audio.
- End to end on the dev box against the fake engine:
  - NDI: a small probe (`corevideo-edge-ndi-probe`, built with the runtime
    loader) finds `CoreVideo Edge <node> S1` on localhost, receives for 10 s,
    reports resolution / fps / audio; gate: 1920x1080, fps within 10% of the
    source, audio present.
  - SRT: FFmpeg as a **listener** on 127.0.0.1:10000 recording to a file
    (exactly Pro's role); gate: ffprobe shows h264 1920x1080 + aac 48 kHz,
    duration within 1 s of the send.
- Live: one routed guest into a running Pro on the same LAN (owner, later).

## Follow-ups (Pro repo)

- Show Edge feeds as a group in Pro's input roster using the NDI metadata
  (guest name) instead of the slot name.
- Health back-channel (Edge -> Pro) so Pro's operator sees Edge-side drops.
- macOS Pro NDI ingest (Pro's NDI ingest is Windows-only today).
