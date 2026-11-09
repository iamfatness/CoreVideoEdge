# CoreVideo Edge – Specification

**Goal**: Multiply the number of simultaneous high-quality Zoom participant feeds available to CoreVideo Pro by distributing raw-data capture across multiple Zoom Meeting SDK clients, each staying under the per-client downlink bandwidth budget, then aggregating the feeds over the LAN.

## Problem

A single Zoom Meeting SDK client is constrained by downlink bandwidth:

| Mode                  | Typical downlink | Approx. 1080p30 feeds |
|-----------------------|------------------|-----------------------|
| Standard              | ~30 Mbps         | 5–8                   |
| Enhanced Media / HBM  | ~100 Mbps        | 12–16                 |

CoreVideo Pro already runs a proven `corevideo-zoom-engine` subprocess that delivers raw I420 + PCM via shared memory. The limit is Zoom’s side, not the local pipeline.

## Solution

Deploy one or more **Edge** nodes. Each Edge:

1. Joins the target meeting with its own Zoom identity / credentials
2. Subscribes only to a subset of participants (assigned by Pro) at full quality
3. Relays clean raw (or lightly processed) video, audio, and metadata to CoreVideo Pro over the LAN
4. Stays silent itself (no camera, muted mic)

Pro treats Edge feeds identically to locally captured Zoom feeds inside the media bus.

## Architecture

### Edge node
- Vendored `zoom-engine` (copy of `native/zoom-engine/` from CoreVideo Pro)
- Lightweight supervisor process (C++ or minimal .NET/WinUI)
- Speaks the existing JSON-line IPC protocol to the engine
- Network relay layer that consumes SHM frames and ships them to Pro
- Local config or control from Pro (discovery, assignment, health)

### Transport (Edge → Pro)
**Preferred**: Custom binary protocol over TCP (or QUIC later)
- Length-prefixed frames
- Header: participant ID, name, timestamp, sequence, width, height, format (I420/NV12), audio presence
- Payload: planes or interleaved frame + optional PCM

**Fallbacks**:
- NDI (fastest to demo, higher overhead)
- SRT (reliable, slightly higher latency)

### Control plane
Pro is the orchestrator:
- Edge announcement (mDNS/Bonjour or static IP list)
- Capacity report (max concurrent 1080p feeds this node can sustain)
- Participant assignment (list of Zoom user IDs or names)
- Health / bitrate / drop counters
- Token-based auth so Edges can join without interactive login

### Zoom permissions
Each Edge must be able to receive raw data. Options (already partially supported in the Pro engine):
- Host grants local recording or raw-streaming permission
- Pre-issued tokens: ZAK, on-behalf token, app privilege token
- Co-host or alternative-host pre-authorization

Edges should appear as production devices, not normal gallery participants.

## Wire contracts (to be defined)

Shared with Pro (or duplicated initially):
- Frame header extending the existing `ShmFrameHeader` idea for network
- Control messages: `announce`, `assign`, `capacity`, `health`, `subscribe-list`, `leave`
- Participant identity mapping (Zoom user ID ↔ stable production identity)

## Execution plan

### Phase 0 – Spec & contracts (done / this document)
- This SPEC + README
- Agree on Phase-1 transport (custom TCP vs NDI)

### Phase 1 – Minimal viable Edge (1–2 weeks)
- Vendor zoom-engine into this repo
- Headless Edge that joins a meeting and relays N participants over simple TCP
- Prove multi-Edge aggregation stays under per-client bandwidth while delivering >8 high-quality feeds to a receiver

### Phase 2 – Pro integration
- Network Zoom source adapter in CoreVideo Pro media core
- Basic discovery + manual assignment UI
- End-to-end soak with mixed local + Edge feeds

### Phase 3 – Orchestration & production
- Automatic participant partitioning by capacity
- Health monitoring and automatic reassignment
- Headless packaging, auto-start, token auth
- Metrics

### Phase 4 – Scale & polish
- Optional light encode on Edge (NVENC) if LAN saturates on raw I420
- Multi-consumer support
- macOS / Linux Edge builds if needed

## Open decisions
- Phase-1 transport: custom TCP (recommended) or NDI
- Exact token / permission flow for unattended Edges
- Whether to extract zoom-engine into a shared package later
- Hardware target for Edge nodes (Windows mini-PC, etc.)

## Related repositories
- CoreVideo Pro: https://github.com/iamfatness/CoreVideoPro
- CoreVideo OBS plugin: https://github.com/iamfatness/CoreVideo
