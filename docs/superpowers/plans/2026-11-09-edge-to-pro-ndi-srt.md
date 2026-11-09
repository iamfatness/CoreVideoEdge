# Plan: Edge -> Pro feeds (NDI, SRT)

Spec: `docs/superpowers/specs/2026-11-09-edge-to-pro-ndi-srt-design.md`.
Each task ends green (native tests + the validator it adds) and is committed.

1. **Feed targets (pure).** `edge/FeedTargets.h`: NDI name for (node, slot);
   SRT URL for (host, basePort, slot, latency, passphrase) with a redacted
   form for logs/state. Tests: stable names, port map, passphrase >= 10 chars
   rule, redaction.
2. **FeedSource.** `edge/FeedSource.{h,cpp}`: per-slot thread reading the
   engine video region (sequence-checked, latest frame, 2 ms poll) and the
   isolated audio ring (own cursor), real-time silence fill (200 ms holdback,
   the ISO timeline), pushing to a `FeedSink`. Test with a fake sink against
   synthetic SHM regions.
3. **NDI runtime loader + NdiFeedSink.** Dynamic load (Pro's candidate list +
   `libndi.dylib`), v5 ABI subset declared locally (send create/destroy,
   send_video_v2, send_audio_v3 or v2, get_no_connections, find for the probe,
   recv for the probe). Missing runtime -> "NDI runtime not installed" warning.
4. **SrtFeedSink.** FFmpeg discovery (`COREVIDEO_FFMPEG_DIR`, PATH), argv
   builder (nvenc, fallback x264), stdin video + named-pipe audio (Windows) /
   `pipe:3` (POSIX), conform to 1080p, connection state from the log, restart
   with backoff when Pro is not listening (stated in health).
5. **Controller.** `set-feeds` command, feed lifecycle following the routing
   (slot routed -> sink opened; unrouted -> closed), state `feeds`, warnings.
6. **Probe + validator.** `corevideo-edge-ndi-probe` (receives a named NDI
   source, prints JSON: width, height, fps, audio). Validator `--feeds ndi`
   and `--feeds srt` (FFmpeg listener on 127.0.0.1:10000+n recording to files,
   ffprobe gates).
7. **App.** "Send to Pro" block (Routing) + per-tile feed health (Monitor).
8. **Docs.** PROTOCOL.md, README (how to wire Edge feeds into Pro: NDI pick
   by name; SRT listener ports), CLAUDE.md laws learned.
