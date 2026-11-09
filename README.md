# CoreVideo Edge

Lightweight, multi-instance Zoom Meeting SDK capture node that relays raw participant feeds to CoreVideo Pro.

It exists to bypass the per-client downlink bandwidth ceiling of the Zoom Meeting SDK (typically ~30 Mbps standard / ~100 Mbps with Enhanced Media / High Bandwidth Mode). Each Edge joins the meeting under its own Zoom identity, subscribes only to an assigned subset of participants at full quality, and forwards clean frames + audio + metadata over the LAN to one or more CoreVideo Pro instances.

## Why a separate repo

- Different lifecycle and packaging from the full production console
- Can be deployed on multiple machines / accounts independently
- Isolates Zoom SDK process risk from the main studio
- Shares the proven zoom-engine via vendoring (same pattern CoreVideo Pro already uses)

## Architecture (high level)

```
[Zoom Cloud]
     │  (≤30 / 100 Mbps per client)
     ▼
[Edge Node 1] ──┐
[Edge Node 2] ──┬── LAN transport ──► [CoreVideo Pro]
[Edge Node N] ──┘                    (aggregates into media bus)
```

- Edge runs a vendored copy of the `corevideo-zoom-engine` subprocess
- Speaks the same JSON-line IPC + shared-memory frame contract as Pro
- Relays frames over a custom low-latency protocol (NDI or SRT as fallbacks)
- Pro discovers Edges, assigns participant subsets, and injects the feeds as first-class sources

## Status

Early. Spec is in `SPEC.md`. Implementation not started.

## Related

- [CoreVideo Pro](https://github.com/iamfatness/CoreVideoPro)
- [CoreVideo OBS plugin](https://github.com/iamfatness/CoreVideo)
