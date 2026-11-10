# Comms (ZComms talkback) in CoreVideo Edge and CoreVideo Pro

2026-11-09. Owner request: "fold in the features and UI/UX pattern [of the
ZComms screen] but change to adopt our look and feel. ZComms needs to be
added" — into **both** Edge and Pro. The owner skipped the design questions
("just start, use the ZComms screen as the model"), so everything below that
is not the ZComms screen itself is the builder's call and is marked *(call)*.

## Intent

An operator running Edge or Pro can cue talent over Zoom talkback from the
same app that is taking their video, without running ZComms beside it. The
screen is the ZComms talkback station: one grid of person cells where a cell
IS a talk key, ALL CALL above it, LATCH / EDIT TALENT / SETTINGS / LEAVE
below, machine truth in lamps and a one-line ops strip. Only the look
changes: each app's own tokens, type and components.

## Why it has to live in the Zoom engine *(call, forced by Zoom)*

Talkback is sent by a meeting participant that is host or co-host. The Zoom
SDK is one participant per process, and a second process signed in to the
same account collides with the first (ZComms law: same-account host collision
hangs joins). Running zcomms.exe beside Edge or Pro would therefore need a
second Zoom account and a second participant. So the talkback layer runs
inside the engine both apps already use (`corevideo-zoom-engine`, Pro's
engine, vendored verbatim into Edge), as the same participant that receives
video. Consequences:

- The Edge or Pro account must be **host or co-host** for Comms to come up.
  The screen says so when it is not; video keeps working either way.
- The engine needs the **Zoom Meeting SDK 7.1.5** (talkback controller).
  Edge and Pro build against 7.0.5 today; the OBS plugin and ZComms already
  ship on 7.1.5. Raw video, ISO and audio must be re-proved on 7.1.5 (fake
  validator + one live take) before Comms ships.
- Panelists need the native Zoom desktop/mobile client (web cannot receive
  talkback) and must be in the station's room (no cross-breakout delivery).

## What gets ported *(call)*

From ZComms `main` (MIT, same owner), not the OBS plugin's nomination model,
because ZComms' 16-slot channel model is exactly what the screen drives:

- `src/zoom`: `talkback_sdk` (+ `_win`, and `_mac` for the Mac port),
  `talkback_channels`, `talkback_source`, `talkback_eligibility`, `roster`,
  `reach`, `duck_plan`. Not `zoom_client*` (the engine already owns join),
  not `jwt`, not chat signalling or breakout admin.
- `src/audio`: the mic chain (devices, engine, limiter, envelope, signal
  gate, generator for TONE, tx_pacer, frame ring/accumulator) and sidetone.
- The orchestration in ZComms `src/app/main.cpp` that owns the laws: channel
  bank bring-up and healing, auto-assign of eligible talent one per channel
  as capacity allows, invite batching and the shared membership-call pacer,
  mic-open re-assertion, unity-at-create + duck-while-keyed, ALREADY_EXIST =
  present, ineligible (web client) participants reported with the reason.
  This becomes one engine module, `engine-comms.{h,cpp}`, driven from the
  engine's command loop.

Every ZComms delivery law (memory: zcomms-talkback-delivery-laws) carries
over unchanged and gets a line in the engine README's delta list.

Not in the first ship *(call)*: extern feeds, echo cancel, moving the
station between breakout rooms, chat cues. They are the next slice; the
screen leaves room for them (Settings drawer).

## Protocol

Engine (E2P/P2E lines, both apps):

- P2E `comms_key {"slot":0..15|"all","on":bool}`, `comms_latch` (same
  shape), `comms_assign {"slot","participant_id","on"}`, `comms_mic
  {"device"}`, `comms_sidetone_out {"device"}`, `comms_sidetone {"on"}`,
  `comms_tone {"on"}`, `comms_gain {"db"}`.
- E2P `comms_state` at up to 10 Hz, only while changed: availability +
  reason, lamps (link, meeting mic open, any channel ready, tx), 16 channels
  `{slot,label,listeners,ready,keyed,latched,reach:{ok,dark}}`, roster
  `{participant_id,name,talkback,room,channels[16]}`, mic `{device,devices,
  peak_dbfs,gain_db,sidetone,sidetone_device,sidetone_devices,tone}`,
  stats `{frames_sent,channel_sends,underruns,send_errors}`, last ops lines.

Edge node (`docs/PROTOCOL.md`): the same verbs as `comms-key`, `comms-latch`,
`comms-assign`, `comms-mic`, `comms-sidetone-out`, `comms-sidetone`,
`comms-tone`, `comms-gain`, relayed; the state line gains a `comms` object
that is the engine's last `comms_state` (absent when the engine has not
reported). Pro: the core relays the same verbs and state to the shell over
its existing control channel.

The fake engine emulates talkback (channels form after a delay, its fake
guests are eligible except one "web client" guest, keyed channels count
sends) so tests, the validator and screenshots run without Zoom.

## The screen

Same structure, both apps, from the ZComms panel:

- **Status rail**: meeting + room, lamps LINK, MTG MIC, CHANNEL, TX.
- **ALL CALL** bar across the top of the desk.
- **Grid of person cells** (auto-fill, ~170 px min): name + state line
  (`ready`, `on air`, `on air · latched`, `nobody in channel`, `forming…`,
  `N listening` for a group, `in <room>` and unreachable when elsewhere,
  `· N elsewhere` for a partial group). Hold to talk; LATCH makes presses
  toggle. Hot = on air with listeners, armed = keyed with nobody hearing.
- **Keyboard**: digits 1–9 key the first nine cells, Space = ALL CALL,
  releasing focus releases every key (panel-scoped, never global).
- **Strip**: LATCH, EDIT TALENT, SETTINGS, LEAVE (Edge: LEAVE is the existing
  Leave; Comms does not leave on its own *(call)*), mic meter.
- **Edit talent**: one row per person, one chip per channel (all 16, busy
  chips marked, click = assign/unassign); web-client people say why and how
  to fix it.
- **Settings drawer**: microphone, sidetone output, SIDETONE / TONE toggles,
  gain −/+, station counters.
- **Ops strip**: latest event on one line, click for scroll-back.
- **Unavailable states** are explicit: not in a meeting; not host/co-host
  ("Make this account host or co-host to bring talkback up"); SDK refused
  (verbatim reason).

ZComms UI laws that must survive the restyle: never rebuild a control under
the operator's finger (structure-signature-gated rebuilds, values updated in
place; a press eats its release otherwise); a cell never shows on air unless
the engine says keyed; nothing unmeasured is drawn.

Look *(call)*: Edge uses `Theme/Tokens.axaml` (hot = the live/red token,
armed = the warn token, ready = primary text, off = tertiary), its type
ramp and button/cell styles, Comms as a fourth tab after Record. Pro uses its
own WinUI tokens and appears as a Comms page in its navigation. No new raw
colours; Edge's TokenContrastTests cover any new pair.

## Order

1. Edge engine on SDK 7.1.5 + `engine-comms` port + fake emulation + node
   relay and state + tests. Re-prove video/ISO on 7.1.5.
2. Edge Comms tab (view model tested for the JSON it sends, screenshots).
3. Pro: the same engine change upstream in Pro's engine, core relay, WinUI
   Comms page.
4. Live gate (owner): Edge account co-host, two desktop-client guests, key a
   person, a group and ALL CALL, confirm audio at the guest; then Pro.
5. Next slice: extern feeds, echo cancel, station room.

## Anti-goals

- A second Zoom participant or account for comms.
- Talkback that can degrade video: Comms failures never touch subscriptions,
  ISO or feeds.
- A screen that looks like ZComms pasted in (amber rack) instead of Edge/Pro.
- Claiming "talent hears you" from send counters alone; the cell states say
  what the engine knows (keyed, members confirmed), not what reached ears.
