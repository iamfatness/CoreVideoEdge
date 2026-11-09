# CoreVideo Edge — operator app design brief

Research only. Date 2026-11-09. Target: Avalonia 11 (C#), Windows first, macOS later. Dark theme only for v1.
Product rules carried into every decision: **a value shown is measured or absent** (no placeholders, no invented numbers), **failures are loud**, **no fake data in the UI**.

All contrast ratios below were computed with the WCAG 2.x relative-luminance formula (script: `scratchpad/tools/contrast.py`), not estimated.

---

## 1. What the sources say (concrete rules only)

### 1.1 impeccable (pbakaus) — https://github.com/pbakaus/impeccable
Read: `.agents/skills/impeccable/SKILL.md`, `reference/craft-floor.md`, `reference/operate.md`, `reference/audit.md`, `reference/critique.md`, `reference/typeset.md`, `reference/colorize.md` (raw.githubusercontent.com, branch `main`).

- **Modes.** Edge is *Operate* mode (apps, dashboards, tools): "familiarity is a feature", the tool should "disappear into the task"; the failure to avoid is strangeness without purpose.
- **Operate typography.** Usually one well-tuned sans for headings, labels, buttons, body and data. Fixed scale, no fluid type, step ratio about 1.125–1.2. Prose 65–75ch; tables can be wider. No display fonts in labels, buttons or data.
- **Operate color.** Restrained by default. Accent only for primary actions, current selection and state indicators, never decoration. No heavy or saturated color on inactive states. Add a second neutral layer for sidebars, toolbars and panels. Standardize the state vocabulary: hover, focus, active, disabled, selected, loading, error, warning, success, info.
- **Components.** Every interactive component needs default, hover, focus, active, disabled, loading and error states. Load with skeletons, not a spinner in the middle of content. Empty states teach how the interface works. Affordances stay consistent across screens. Try inline or progressive UI before reaching for a modal.
- **Motion.** 150–250 ms for most transitions. Motion only for state change, feedback, loading or reveal. No orchestrated page-load sequences. Exponential ease-out, starting from an already visible state.
- **Quality floor.** Body and placeholder text ≥4.5:1, large text ≥3:1, controls, icons and focus indicators ≥3:1. On coloured surfaces, tint secondary text from the hue, never grey. Shadows need an offset and a soft blur. Groups tight, separations generous, more space above a heading than below. Tracking never below -0.04em. Theme selection, caret, scrollbars, focus rings and tabular numerals instead of leaving platform defaults. Controls name their action; errors name the problem and the recovery.
- **Absolute bans and refusals.** No same-size icon-heading-text card grids as page structure. Nested cards are always wrong. No hero-metric template (big number, small label, stats, accent). **No kicker or eyebrow above a heading.** No section numbers unless the sequence carries information. No modal for a task that needs no interruption. No gradient text. No decorative glass or blur. No coloured border-left or border-right thicker than 1px on cards, list items or alerts. No hard offset or zero-blur shadows. No sparklines, progress rings or soft rounded rectangles standing in for content. **Monospace only for code, data and measurement, never as a "technical" costume.** No emoji or Unicode glyphs as icons; use one drawn icon set with one stroke weight. Pick light or dark from the scene of use, not the category.
- **Elevation and radius.** Declare elevation once, as a border *or* a shadow (a 1px border under a wide soft shadow is the "ghost card"). Card radii 12–16px. Pills only for small controls.
- **Claims.** Take claims and configuration from supplied truth. Label illustrative values honestly. This matches Edge's measured-or-absent rule.
- **README slop tells.** "Inter for everything", purple-to-blue gradients, cards nested in cards, grey text on coloured backgrounds, a rounded-square icon tile above every heading, cramped padding, skipped headings. The detector has 62 deterministic rules, including `overused-font` and low-contrast or tiny-text checks.
- **Audit (`/audit`).** Five dimensions, each scored 0–4 (total /20): Accessibility, Performance, Theming (hard-coded colours vs tokens), Responsive (touch targets 44×44 recommended, 24×24 WCAG 2.2 minimum), and Implementation Integrity (weighted critical: design-system drift, decorative or misleading content, generic structure). Severities P0 to P3.
- **Critique (`/critique`).** Nielsen's 10 heuristics scored 0–4 (/40). Cognitive-load checklist of 8 items: at a decision point ≤4 items is fine, 5–7 borderline, 8+ overloads. Named violations: Wall of Options, Memory Bridge, Hidden Navigation, Jargon Barrier, Visual Noise Floor, Inconsistent Pattern, Multi-Task Demand, Context Switch. Personas: Alex (power user), Jordan (first-timer), Sam (accessibility), Riley (stress tester), Casey (distracted). For Edge, use **Alex, Riley and Sam**.

### 1.2 UI UX Pro Max — https://github.com/nextlevelbuilder/ui-ux-pro-max-skill
Read: `.claude/skills/ui-ux-pro-max/SKILL.md`, `references/quick-reference.md`, and data CSVs under `src/ui-ux-pro-max/data/` (`styles.csv`, `products.csv`, `colors.csv`, `typography.csv`, `ux-guidelines.csv`, `motion.csv`).

- **Priority order.** Accessibility, then Touch & Interaction, Performance, Style, Layout, Typography & Color, Animation, Forms, Navigation, Charts.
- **Accessibility.** Text 4.5:1, large text 3:1, data marks 3:1, data labels 4.5:1. Visible focus. Keyboard order follows visual order. `color-not-only`: badge meaning never relies on colour alone. WCAG 2.2: focus-not-obscured, focus-appearance, target size 24×24 minimum. Live status changes are announced without moving focus (`contextual-live-badge-updates`).
- **Interaction.** Targets 44×44 recommended, 8px between targets. Disable a button and show progress during async work. Show errors next to the cause. Never use a 0 ms state change, and never rely on hover alone.
- **Type and colour.** Weights: 600–700 headings, 500 labels, 400 body. Tabular figures for data. Semantic colour tokens, never raw hex in components. Dark mode uses desaturated or lighter tonal variants instead of inverting, and its contrast is tested separately. No body text under 12px. No grey-on-grey.
- **Animation.** Exits run at 60–70% of the enter duration. Stagger 30–50 ms. Animate only 1–2 key elements per view. Animate only transform and opacity, never width or height. Animations are interruptible and never block input. From `motion.csv`: hover 150–200 ms, subtle page transition 200–300 ms, skeleton loop 1200–1600 ms. Infinite animation only for loading indicators (`ux-guidelines` #12).
- **Forms and feedback.** Visible labels, never placeholder-only. Errors sit next to the field and state cause and recovery. Toasts dismiss after 3–5 s and never steal focus. Confirm destructive actions. Distinguish read-only from disabled. Every async action shows loading, then success or error.
- **Layout.** 4/8 spacing scale. A density dial: Dense 8–32px, Standard 16–64px. One primary action per screen. A defined z-index scale. No nested scroll regions.
- **Matching rows for this product.**
  - Style #31 *Real-Time Monitoring*: status indicators, alert colours, streaming updates. The CSV pairs it with "alert pulse/glow, blink" effects. **Reject the blink and glow**; they break the motion rules above.
  - Style #28 *Data-Dense Dashboard*: minimal padding, grid layout.
  - Style #7 *Dark Mode (OLED)*: high contrast, visible focus. **Reject** its neon accents and `text-shadow` glow.
  - `colors.csv` *Developer Tool/IDE*: background `#0F172A`, muted foreground `#94A3B8`, "run green" `#22C55E`. *Video Streaming/OTT*: card `#0C0C0D`, accent `#E11D48`. These confirm a near-black ground with a green "running" signal, which Pro already owns.
  - `typography.csv` #3 *Tech Startup*: Space Grotesk + DM Sans. #42 *Dashboard Data*: Fira Code + Fira Sans. Pro's choice, Space Grotesk + IBM Plex Mono, fits the same niche.
- **Pre-delivery checklist** (README sample). No emoji icons (use SVG: Lucide or Heroicons). Pointer cursor on every clickable element. 4.5:1 text. Visible focus. Reduced motion respected.

### 1.3 21st.dev — https://21st.dev (component registry)
Listing pages examined:
- https://21st.dev/@diceui/components/status.md (docs: https://www.diceui.com/docs/components/status)
- https://21st.dev/@corr/components/status-pill.md
- https://21st.dev/@serafimcloud/components/status-badge.md
- https://21st.dev/@adobe/components/css-segmentedcontrol.md
- https://21st.dev/@kevingirelli/components/segmented-control.md
- https://21st.dev/@ddoemonn/components/segmented-control.md
- https://21st.dev/@prebuiltui/components/toggle-switch.md
- https://21st.dev/@lailai0916/components/empty-state.md
- https://21st.dev/@serafimcloud/components/empty-state.md
- https://21st.dev/community/components/s/table

Caveats:
- Component source on 21st.dev sits behind an API-key install, so the listings give only structure.
- Nearly every entry is built on shadcn/ui. The exact metrics below therefore come from shadcn's `new-york-v4` registry source (github.com/shadcn-ui/ui `apps/v4/registry/new-york-v4/ui/*.tsx`), converted to px.
- **There is no multiview or camera-grid component on 21st.dev.** The closest, `@thegridcn/video-player`, is a single-player sci-fi HUD and does not apply. The monitor grid below is modelled on broadcast multiviewers and on Pro's own multiview instead.

Patterns to re-implement in XAML:

| Pattern | Structure (shadcn / 21st metrics) | States | Edge adaptation |
|---|---|---|---|
| **Status pill** (Dice UI Status, corr Status Pill) | `Status` container > `StatusIndicator` dot > `StatusLabel`. Badge base: inline-flex, gap 4, px 8, py 2, text 12/500, icon 12, full radius. Dice pulses the dot with `animate-ping`. Variants: default, success, error, warning, info. Dice: "color isn't the only signal, always include a text label". | One per variant, plus focus ring when interactive | Keep dot + label + optional icon. **Radius 4, not full** (Pro rule: nothing fully rounded). **No ping pulse on healthy states.** A pulse is allowed only on transient states (Connecting, Requesting), max 1.2 s period, off under reduced motion. |
| **Segmented control** (Adobe/React-Aria, kevingirelli/Radix toggle-group, ddoemonn) | Track: h 36, padding 3, radius 8, muted fill. Items: flex-1, radius 6, px 8, py 4, text 14/500, inactive text at 60% opacity, active = raised fill + 1px border + small shadow. Single-select. Arrow keys move selection. The Adobe and Radix versions slide a thumb. | Hover (text to full), focus (3px ring), active, disabled (50%) | Used for Resolution, Frame rate and the participant filter. Track `surface`, active item `raised` + 1px `ctl-border`. **No sliding-thumb animation; snap in 120 ms colour.** `role=radiogroup` semantics. |
| **Toggle switch** (prebuiltui, shadcn Switch) | Track 32×18 full radius, thumb 16, thumb travels 14px, checked = primary fill, unchecked = input fill. Focus ring 3px at 50% opacity. Disabled 50%. | On, off, focus, disabled | Use only for preferences (for example "Show video-off participants"). **Not for Engine On**: that is a latching hardware-style button (see 4.1). |
| **Underline tabs** (shadcn Tabs `variant=line`) | Trigger text 14/500, inactive 60% opacity; active shows a 2px bar 5px below the text. | Hover, focus, active, disabled | Top-level nav (Connect / Routing / Monitor / Record). |
| **Data table / grid** (21st Tables, shadcn Table) | Header row h 40, px 8, text 500. Rows separated by a 1px border. Row hover = muted at 50%. Selected = muted. Checkbox column has zero right padding. | Hover, selected, focus-within | Base for the routing matrix and per-ISO table: sticky header and sticky first column. |
| **Empty state** (lailai0916, serafimcloud, shadcn Empty) | Root: flex column, centered, gap 24, padding 24 (48 wide), dashed border, radius 8. Media tile 40×40 radius 8 muted fill, icon 24. Title, then description 14 muted relaxed, max-w 384. Actions max-w 384, gap 16. | — | **Drop the 40px icon tile** (impeccable slop tell: rounded-square icon tile above a heading). Keep the dashed 1px outline, title, one sentence that teaches, one action. |
| **Toolbar header** (21st Navigation category) | 48–56px bar: brand left, nav centre or left, actions and status right. | — | 48px app header (matches Volt `--head-h: 48px`). |

---

## 2. Family visual languages (extracted from the repos)

### 2.1 CoreVideo Pro
Sources:
- `C:\Users\walla\CoreVideoPro\native-shell\CoreVideoPro.WinUI\Themes\StudioTheme.xaml`
- `C:\Users\walla\CoreVideoPro\native-shell\CoreVideoPro.WinUI\App.xaml`
- `C:\Users\walla\CoreVideoPro\native-shell\CoreVideoPro.WinUI\Views\OperatorTabResources.xaml`
- `C:\Users\walla\CoreVideoPro\docs\design-handoff-macos.md`
- `C:\Users\walla\CoreVideoPro\docs\design-reference\multiview-mark.svg`

| Token | Hex | Use |
|---|---|---|
| Background | `#0A0B0C` | Window |
| Panel | `#101315` | Panels, sections |
| Surface | `#16191B` | Raised controls |
| Surface raised | `#1B1F22` | Hover, popovers |
| Field / tile well | `#0E1112` | Inputs, tile wells |
| Readout well | `#0C1118` | Numeric readouts (OperatorReadoutBorder) |
| Border | `#FFFFFF` α 0.09 (`#17FFFFFF`) | 1px control borders |
| Line2 | `#FFFFFF` α 0.05 (`#0DFFFFFF`) | Hairline dividers |
| Text primary | `#E9EDEF` | |
| Text muted | `#8B949B` | Body copy, placeholders, mono labels |
| Text dim | `#5C656B` | Dim mono labels. **Fails 4.5:1** (3.13 on panel); Edge must not use it for text. |
| Accent / live green | `#22C86E` | Accent buttons and live, with on-accent ink `#06170D` |
| Program / warning amber | `#E8A41F` | Tally and warnings |
| Air red | `#E5433F` | On-air and record |

- **Fonts.** Bundled OFL: **Space Grotesk** (variable TTF) for all UI, and **IBM Plex Mono** Regular, Medium and SemiBold for labels, timecode, meters and badges.
- **Type scale.**
  - Section title 14 SemiBold. Body 12 in muted. Buttons 13 SemiBold; nav buttons 12.
  - Mono label 10px, tracking 0.16em, UPPERCASE. Mono value 12 Medium, 0.02em. Mono tag 10 SemiBold, 0.12em.
- **Radii.** 8 for controls and panels, 10 for nav and pills, 6 on the operator ghost button and readout. "Nothing is fully rounded, nothing is square."
- **Spacing and components.**
  - Ghost button: padding 14×9, min-h 38.
  - Accent button: padding 16×10, min-h 40.
  - Field: padding 10×8.
  - Section: padding 16.
- **Logo ("Multiview" mark).** 120×120 viewBox.
  - Frame: rect 14,22 92×76, rx 15, stroke 6.
  - Cross lines at x=60 and y=60, stroke 5.
  - Live tile: rect 65,27 36×28, rx 6, filled.
  - Everything is `#22C86E`. State variants change **only the tile fill** (amber `#E8A41F`, red `#E5433F`).
  - App icon: the mark in a 36×36 squircle, r 9, padding 6, gradient `#15191B` to `#0B0D0E` (0,0 to 0.35,1).
  - Wordmark: "CoreVideo Pro" in Space Grotesk SemiBold 14 with the subtitle "Live production" 11 in muted. "The wordmark IS the font."
  - Note: the shipped `Square150x150Logo.png` still shows an older "CV" glyph on a green squircle; the vector mark is canonical.
- **Known gap.** Stock WinUI sliders and toggles render in Windows light-blue accent. Edge must override the platform accent from day one.

### 2.2 Volt
Sources:
- `C:\Users\walla\CoreVideo-Volt\docs\brand-guide.md`
- `C:\Users\walla\CoreVideo-Volt\control\volt\session\operator\operator.css`
- `C:\Users\walla\CoreVideo-Volt\brand\logos\*.svg`

| Token | Hex |
|---|---|
| Ground | `#141414` |
| Panel | `#1C1C1C` |
| Raise | `#242424` |
| Hairline | `#303030` |
| Text | `#E2E2E2` |
| Muted | `#A0A0A0` in CSS (`#8A8A8A` in the guide) |
| Suite blue (CoreVideo family actions, links, focus) | `#1D6DC2`; text and focus use `--blue-hi #5EA3EA` |
| Signal amber (Volt accent: next, selected, publish) | `#F0B429`, ink `#1A1200` |
| On air | `#FF4D4D`, ink `#1A0000` |
| Cleared | `#3DDC97` |
| Tints | air-bg `#2A1616`, signal-bg `#2A2210`, ok-bg `#12261D` |

- **Scales.** Spacing 4/8/12/16/24/32. Radius 3 and 6. Type 12/14/16/20/28. Rail 64, head 48.
- **Type.** Font is the system UI font; data uses Consolas / Cascadia Mono with `tabular-nums`.
- **Focus.** `outline: 2px solid #5EA3EA; outline-offset: 2px`.
- **Reduced motion.** Kills transitions.
- **Rules.**
  - "Red is a tally, not a brand", and red is used only for a live path.
  - "Signal amber is the Volt color. Suite blue stays the CoreVideo color."
  - No gradients behind the wordmark. Do not add a second blue.
  - Every colour pair is measured by a test.
- **Logo.** Four amber slashes, then white geometric VOLT on charcoal.
  - Mark (128 viewBox): four 6×60 rects, rx 1, at x = 28/46/64/82, y = 34, each rotated -18° about its centre, fill `#F0B429`, on `#141414`.
  - Rule: "always four, same angle, same gap, same weight".

### 2.3 What the family shares
- A near-black ground.
- One owned accent per product: Pro green, Volt amber.
- Broadcast tally semantics: red = on air / recording, amber = program / warning, green = live / OK.
- 1px hairlines. Mono, tabular numerals for measured values.
- A mark built from simple geometric strokes, plus a wordmark set in the font.
- Suite blue `#1D6DC2` / `#5EA3EA` as the CoreVideo family colour for links and focus.

---

## 3. Proposed CoreVideo Edge design system

### 3.1 Positioning
Edge is Pro's **satellite seat**. It shares Pro's neutral ramp, fonts and tally colours, so an operator moving between the two machines reads both instantly. Its owned accent is **Edge cyan**, a hue no family product uses. Pro overloads green as both brand accent and "live"; Edge separates the two, so a cyan selection can never be mistaken for a healthy feed.

### 3.2 Colour tokens (dark)
Avalonia: declare each as `Color` + `SolidColorBrush` in a `ResourceDictionary` under `ThemeVariant Dark`. Components reference tokens only, never raw hex (an impeccable/UUPM audit item).

**Neutrals** (Pro's ramp, unchanged)

| Token | Hex | Role |
|---|---|---|
| `bg` | `#0A0B0C` | Window ground |
| `panel` | `#101315` | Header, rails, section containers (the second neutral layer) |
| `surface` | `#16191B` | Controls, segmented track, metadata strip under video |
| `raised` | `#1B1F22` | Hover, selected segment, flyouts |
| `hover` | `#22272B` | Matrix cell / row hover |
| `well` | `#0E1112` | Text fields, numeric readouts |
| `video` | `#000000` | Video letterbox inside tiles |
| `hairline` | `#FFFFFF` α 0.09 (≈ `#26282A` on panel, 1.26:1) | Decorative separators only |
| `ctl-border` | `#6B767F` | Borders that identify a control (fields, unselected matrix cells, switch-off track outline). Needs ≥3:1 per WCAG 1.4.11. |

**Text**

| Token | Hex | Use |
|---|---|---|
| `text` | `#E9EDEF` | Primary |
| `text-2` | `#9BA4AB` | Secondary and labels (Pro's `#8B949B` lifted one step for small 12px labels on `raised`) |
| `text-disabled` | `#5C656B` | Disabled only (WCAG-exempt). Never for information. |

**Accent and status**

| Token | Hex | Use |
|---|---|---|
| `accent` | `#3CC4E0` | Edge cyan: primary action fill, selection, routed crosspoints, the logo |
| `accent-hover` / `accent-press` | `#5FD3EA` / `#2AA9C4` | |
| `on-accent` | `#04161B` | Ink on cyan |
| `accent-tint` | `#0B2128` | Selected row background |
| `live` | `#22C86E` | Receiving / healthy / engine on (Pro live green) |
| `on-live` | `#06170D` | |
| `live-tint` | `#0F2419` | |
| `warn` | `#E8A41F` | Degraded (fps under target, frames being lost, permission pending). Pro amber. |
| `on-warn` | `#1A1200` | |
| `warn-tint` | `#2A2110` | |
| `rec` | `#E5433F` | **Recording only**: the REC tally fill and dot. Pro air red. |
| `on-rec` | `#1A0000` | |
| `error-text` | `#FF7A6E` | Failure text, icons and borders. Red-family, lighter so it passes on every surface. |
| `error-tint` | `#2A1414` | Failure banner fill |
| `focus` | `#5EA3EA` | Family suite-blue-hi. Focus is *family*, selection is *Edge*, so the two never look alike. |

**Contrast, computed.** Text tokens against each surface:

| Foreground | bg `#0A0B0C` | panel `#101315` | surface `#16191B` | raised `#1B1F22` | well `#0E1112` | video `#000000` |
|---|---|---|---|---|---|---|
| `text` `#E9EDEF` | 16.72 | 15.83 | 14.99 | 14.08 | 16.09 | 17.82 |
| `text-2` `#9BA4AB` | 7.78 | 7.36 | 6.97 | 6.55 | 7.49 | 8.29 |
| `accent` `#3CC4E0` (text, icons) | 9.53 | 9.03 | 8.55 | 8.03 | 9.18 | 10.16 |
| `live` `#22C86E` | 8.96 | 8.49 | 8.04 | 7.55 | 8.63 | 9.55 |
| `warn` `#E8A41F` | 9.16 | 8.67 | 8.21 | 7.71 | 8.81 | 9.76 |
| `error-text` `#FF7A6E` | 7.75 | 7.34 | 6.95 | 6.53 | 7.46 | 8.26 |
| `focus` `#5EA3EA` (UI ≥3) | 7.41 | 7.02 | 6.65 | 6.24 | 7.14 | 7.90 |
| `ctl-border` `#6B767F` (UI ≥3) | 4.24 | 4.02 | 3.80 | 3.57 | 4.08 | — |
| `rec` `#E5433F` (fill / glyph only) | 4.88 | 4.62 | 4.38 | 4.11 | 4.70 | 5.21 |
| `text-disabled` `#5C656B` (exempt) | 3.31 | 3.13 | 2.97 | 2.79 | 3.19 | 3.53 |

- `rec` stays ≥3:1 as a glyph on every surface. **Red text always uses `error-text`**, never `rec`; `rec` falls to 4.11 on `raised`.
- Rejected candidates:
  - Pro `#5C656B` "dim" as label text: 3.13 on panel, fails 4.5.
  - `#5F6970` for control borders: 2.96 on raised, fails 3.0.
  - White on red `#E5433F`: 4.03, fails for small text. Use dark ink instead.

Fills with ink:

| Pair | Ratio |
|---|---|
| `on-accent` on `accent` | 8.95 |
| `on-accent` on `accent-hover` | 10.56 |
| `on-accent` on `accent-press` | 6.67 |
| `on-live` on `live` | 8.41 |
| `on-warn` on `warn` | 8.63 |
| `on-rec` on `rec` | 4.99 |
| `text` on `hover` | 12.79 |
| `text-2` on `hover` | 5.95 |

Tinted banners and pills:

| Pair | Ratio |
|---|---|
| `live` on `live-tint` | 7.43 |
| `warn` on `warn-tint` | 7.38 |
| `error-text` on `error-tint` | 6.84 |
| `accent` on `accent-tint` | 8.05 |
| `text` on `error-tint` | 14.75 |
| `text-2` on tints (live / warn / error / accent) | 6.44 / 6.27 / 6.86 / 6.57 |

**Colour-blind caveat (computed).** `accent` against `live` is 1.06:1: same lightness, different hue only. Two rules follow:
- (a) A status is never shown by colour alone: always dot + word, plus an icon for warn and error.
- (b) Cyan never means health and green never means selection.

**Platform accent.** Set Fluent `SystemAccentColor` (and its Light1–3 and Dark1–3 variants) to Edge cyan so no stock control falls back to the Windows or macOS accent. This avoids Pro's documented light-blue-slider bug.

### 3.3 Typography
Bundle both as OFL fonts via `avares://CoreVideoEdge/Assets/Fonts#…`. This is identical on Windows and macOS, with no system-font dependency.
- **Space Grotesk** for all UI text: Regular 400, Medium 500, SemiBold 600.
- **IBM Plex Mono** (Regular, Medium, SemiBold) **only for measured values**: resolution, fps, frames lost, dBFS, timecode, durations, bytes, file names. Its fixed width keeps updating numbers from jittering.
- **Labels use Space Grotesk, sentence case. This differs from Pro**, which uses uppercase tracked mono labels. Mono-as-costume and the eyebrow label are impeccable refusals. Uppercase mono survives only in **tally tags** (`REC`, `LIVE`, `ISO`, `ENGINE`), where it is broadcast convention.

| Role | Font | Size / line-height | Weight | Tracking |
|---|---|---|---|---|
| Screen title | Space Grotesk | 18 / 24 | 600 | -0.01em |
| Section title | Space Grotesk | 15 / 20 | 600 | 0 |
| Body / control | Space Grotesk | 13 / 20 | 400 (buttons 600) | 0 |
| Label / secondary | Space Grotesk | 12 / 16 | 500, `text-2` | +0.01em |
| Measured value | IBM Plex Mono | 12 / 16 (tiles), 13 / 20 (tables) | 500 | +0.02em |
| Large readout (REC timer, tile name) | IBM Plex Mono (timer), Space Grotesk (name) | 22 / 28 | 600 | 0 |
| Tally tag | IBM Plex Mono | 10 / 12 | 600 UPPERCASE | +0.12em |

- The scale ratio is about 1.15–1.2 (Operate mode).
- 12px is the floor for anything read; 10px is allowed only for 3–6-letter tally tags that always have a coloured fill.
- Light-on-dark adjustment (impeccable `typeset`): body sits at 13/20 rather than 13/18.

### 3.4 Spacing, size, radius, elevation
- **Spacing (4-pt):** `s1` 4, `s2` 8, `s3` 12, `s4` 16, `s5` 24, `s6` 32, `s7` 48.
  - Inside controls: 8–12. Between related controls: 8. Between groups: 24. Screen gutter: 16 (24 at ≥1440px wide).
  - Headings get 24 above and 8 below.
- **Control heights:**
  - Compact 28: table-row actions, matrix cells are 28 visual / 32 hit.
  - Default 36: buttons, fields, segmented controls.
  - Primary 40: Join, Start recording.
  - Engine button 48.
  - All targets ≥24×24 (WCAG 2.2). Primary targets ≥36 on a mouse-driven desktop.
- **Radii:** 4 (tags, matrix cells, small chips), 6 (buttons, fields, segmented items), 8 (panels, video tiles, segmented track, flyouts).
  - This sits deliberately between Volt (3/6) and Pro (8/10).
  - Nothing fully rounded except the switch track (a platform convention).
  - impeccable's 12–16 card radius is for marketing cards. A dense operator tool follows family radii instead, and this is an intentional exception.
- **Elevation, declared once.**
  - In-window layers are separated by fill steps (`bg` → `panel` → `surface` → `raised`) plus a 1px `hairline` where two equal fills meet. **No shadows in-window.**
  - Flyouts, menus and dialogs: `raised` fill + shadow `0 8 24 rgba(0,0,0,0.55)` with **no border** (no ghost card).
  - z-scale: content 0, sticky headers 10, flyout 100, dialog 200, toast 300.

### 3.5 Motion
- **Durations:** 120 ms for hover and press colour; 180 ms for state changes (cell routed, tile health border, segment select); 240 ms maximum for reveals (flyout, banner expand). Exits at 65% of the enter time.
- **Easing:** exponential ease-out `cubic-bezier(0.16, 1, 0.3, 1)` for enters, `cubic-bezier(0.7, 0, 0.84, 0)` for exits. In Avalonia, use `SplineEasing`.
- **What may animate:** opacity and `RenderTransform` (translate, scale ≤1.02) only. Never Width, Height or layout.
- **Allowed infinite motion:**
  - The 1.2 s pulse on a transient Connecting or Requesting dot.
  - An indeterminate progress bar.
  - **The REC tally is solid, never blinking.** Health borders never pulse. Failures are loud through words and position, not flashing (and this avoids flash-threshold risk).
- **Video tiles never animate** in or out on route changes, and there are no entrance sequences on screen load.
- **Reduced motion:**
  - Windows: `SystemParametersInfo(SPI_GETCLIENTAREAANIMATION)`.
  - macOS: `NSWorkspace.accessibilityDisplayShouldReduceMotion`.
  - When set, all durations become 0 *except* colour cross-fades of 80 ms, so state changes still read. The pulse becomes a static dot.

### 3.6 Focus and keyboard
- **Focus ring.** Avalonia 11 `:focus-visible` style draws 2px `focus` `#5EA3EA` with a 2px offset onto `bg` (the Volt spec). On matrix cells the offset ring sits outside the cell, so it shows against a cyan fill too: `#5EA3EA` on the `bg` gap is 7.41:1. Never remove the FocusAdorner without a replacement.
- **Global keys.** Ctrl+1–4 switch screens. F5 or Ctrl+R retries the current failed action.
- **Matrix keys.** Arrow keys move through cells, Space or Enter toggles, Home and End jump in a row.
- **Record keys.** Ctrl+Shift+R starts or stops recording, guarded by a confirm on Stop.
- **Announcements.** Every status pill exposes `AutomationProperties.Name` (for example "Guest 3, receiving, 1920 by 1080, 59.9 frames per second"). Health changes raise a UIA LiveRegion change without moving focus.

### 3.7 Iconography
- **Lucide**, as SVG paths turned into Avalonia `PathIcon`/`StreamGeometry`: 24 grid, 1.75 stroke (matches Volt's sprite), rendered at 16 and 20.
- No emoji, no Unicode ✓/✗/●. Draw the record dot as a 10px `Ellipse`.

---

## 4. Screen-by-screen

### Shell (all screens)
- **Header (48px, `panel`, bottom hairline).**
  - Left: the Edge mark 20px + "CoreVideo Edge" in Space Grotesk 600 14.
  - Centre-left: underline tabs Connect / Routing / Monitor / Record (13/500). The active tab is `text` with a 2px `accent` bar; inactive tabs are `text-2`.
  - Right, the persistent **status cluster** (always visible, so failures on another screen stay loud):
    - `Meeting`: pill with meeting ID or topic.
    - `Engine`: Off / Requesting / On / Denied.
    - `Routed n/8`.
    - `ISO n/8`.
    - REC tally (red fill + mono timer, only while actually recording).
    - Disk free (measured, mono). It turns `warn` under 20 GB, then `error` under 5 GB or when free space is below 10 minutes of the current measured write rate. Only measured rates are used.
- **Failure banner slot.** Directly under the header, full width, `error-tint` fill with a 1px `error-text` *top and bottom* edge (no thick left stripe).
  - Content: icon + problem + recovery + an action button.
  - It persists until the condition clears. Toasts are only for completed, non-critical events (for example "Recording saved, 3 files"), 4 s, no focus steal.
- **Stale data.** Any value not refreshed for more than 2 s greys to `text-2` with a mono `stale 3s` suffix. After 10 s the value is removed and the reason is shown instead. A frozen number never pretends to be live.

### 4.1 Connect
Layout: a single centred column, 560px wide. Three **sequential step rows**. The 1–2–3 numbering carries real order, so impeccable allows it. Each row is a `panel` block with radius 8, padding 16, and 8px between rows: state icon (16), title 15/600, one-line detail in `text-2`, and the action on the right.

1. **Sign in to Zoom.**
   - Action: "Sign in with Zoom" (primary, cyan).
   - Signed in: the account email (measured from the token) + "Sign out" (ghost).
   - Waiting on the browser: "Waiting for Zoom sign-in in your browser · 0:14" with a live elapsed counter + Cancel.
2. **Join meeting.**
   - Field "Meeting link" (visible label, never placeholder-only), 36px, `well`, `ctl-border`. Below it, helper text: "Paste the full invite link with the passcode."
   - "Join" (primary 40px).
   - Joining: button disabled with an inline spinner + "Joining… 0:06". Joined: meeting topic + participant count (measured) + "Leave" (ghost; confirm only if ISO is recording).
   - Errors render under the field in `error-text`, naming cause and recovery (for example "Meeting needs a passcode. Paste the full invite link, not the bare ID.").
3. **Engine.** A 48px **latching button**, full row width, styled like a hardware key.
   - **Off:** `surface` fill, `ctl-border` 1px, grey lamp dot, label "Engine off. Turn on to request video and recording access."
   - **Requesting:** `warn-tint` fill, amber pulsing dot, "Requesting raw media and local-record permission from the host · 0:09", Cancel link.
   - **On:** `live-tint` fill, solid green lamp, "Engine on", and the permission it got ("Raw media: granted · Local record: granted", each measured from SDK callbacks).
   - **Denied or failed:** `error-tint`, error icon, "Host denied local recording. Ask the host to allow it in Participants > More > Allow record, then retry." with a Retry button. Partial grants are listed individually; for example "Raw media: granted · Local record: denied" makes ISO unavailable, and that is shown on Record too.
- **Empty / first-run.** The three rows *are* the teaching empty state. Steps 2 and 3 are disabled with a reason ("Sign in first") rather than hidden.

### 4.2 Routing
- **Layout.** A left list (320px) plus the matrix (fills the rest).
- **Toolbar** (40px, `panel`):
  - Filter segmented: `Video on` (default) | `All`, with counts from the roster, for example "Video on 7 · All 12".
  - Search field.
  - Capacity readouts in mono: `Subscribed 5/8` and `ISO 3/8` (8 at 1080p).
- **Router crosspoint grid** (the broadcast mental model).
  - Rows = participants. A sticky first column 240px shows: name (13/500), a camera state icon, and the offered resolution (mono 12, when the SDK reports it).
  - Columns = 8 **slots**, `S1`–`S8`, in a sticky header row of 40px. Each header carries the slot's live health dot + mono `1080p59.9` once frames arrive.
  - Two bands sit side by side, **Subscribe** (S1–S8) and **ISO** (I1–I8), separated by 16px and labelled 12/500 in the header. If 16 columns do not fit under 1100px, the two bands become segmented tabs (Subscribe | ISO) over one grid.
- **Cell.** 28px visual, 32px hit target, radius 4, gap 4.

  | State | Look |
  |---|---|
  | Empty | Transparent with 1px `ctl-border` |
  | Hover | `hover` fill |
  | Routed (Subscribe) | `accent` fill + 8px `on-accent` dot |
  | Armed (ISO, not recording) | 1px `accent` border + hollow 8px circle |
  | Recording (ISO, while REC) | Hollow circle becomes a solid `rec` dot |
  | Pending (sent, not yet confirmed by the SDK) | `accent` 1px dashed border + 12px spinner; the cell never shows "routed" before the SDK confirms |
  | Failed | 1px `error-text` border + error icon; the reason appears in the row's detail line and in a tooltip |
  | Unavailable (cap reached or participant video off) | Cell disabled at `text-disabled`; tooltip "All 8 slots in use. Unroute one first." |

  - One participant per slot column (radio per column). Re-routing an occupied slot *replaces* it with a 180 ms cross-fade of the cell fill, without a confirm. ISO unrouting **while recording** requires a confirm ("Stop the ISO for Guest 3? Its file closes now.").
- **Left list** (optional at narrow widths): the same participants as compact rows, with row hover `hover`, selected `accent-tint`. It acts as the keyboard entry point.
- **Empty states.**
  - Not joined: a dashed 1px outline box, "Join a meeting on Connect to see participants." + "Go to Connect".
  - Joined but no video-on participants: "No one has video on. Participants appear here when their camera starts." + toggle "Show all participants".
- **Loading.** Roster rows use skeletons (`surface` bars 12px, 1.4 s opacity loop 0.5–0.8) for at most the first 2 s. After that, show the real (possibly empty) list.

### 4.3 Monitor
- **Grid.** Tiles at 16:9, auto-fit columns: 2 under 1000px, 3 up to 1600px, 4 above. Gap 8 (dense). Order follows slot number, so a tile never jumps when another route changes.
- **Tile anatomy** (`surface`, radius 8, 1px hairline border):
  - **Video area:** `#000000`, the picture letterboxed, never cropped or stretched. A **vertical audio meter**, 6px wide, hugs the right edge *inside* the video area:
    - segments green to −18 dBFS, amber to −6, red above;
    - peak hold 1.5 s;
    - drawn only from measured levels; with no audio measured, the meter is not drawn.
  - **Metadata strip** (36px, below the video, never over it, so text contrast never depends on picture content). Left: slot `S3` (mono tag) + participant name (13/500, truncate with tooltip). Right, mono 12:
    - `1920×1080`
    - `59.9 fps` (measured over a 1 s window)
    - `lost 0` (cumulative; turns `warn` while increasing in the last 10 s)
    - an `ISO` tag (outline when armed, `rec` fill + mono duration while writing)
- **Health border** (2px inset, replacing the hairline):

  | Condition | Border | Strip text |
  |---|---|---|
  | Healthy | None | — |
  | Degraded: fps < 90% of target for 3 s, or frames lost rising | `warn` | e.g. `fps 41 / 60` |
  | Failed: no frames > 2 s while routed, or ISO writer error | `error-text` | — |

  - On failure the video area shows a **slate** (dark `#0E1112` + error icon + "No video for 6 s" with a counting timer, or "ISO write failed: disk full"). The last frame is never silently frozen; a held frame is shown dimmed to 40% with "Last frame 6 s ago".
- **Other tile states.**
  - Warming (routed, before the first frame): slate `#0E1112`, a pulsing grey dot, "Waiting for first frame · 0:03". No metrics are shown, because none are measured.
  - Unrouted slots are **not** drawn as empty tiles. The grid shows only routed or ISO'd sources.
  - Empty Monitor: dashed box, "Nothing routed. Route participants on Routing to monitor them here." + "Go to Routing".
- **Interaction.** A tile is focusable. Enter opens a larger single view (a flyout at 70% of the window, with Esc to close), not a modal workflow. Hover reveals no extra chrome; everything important is always visible.

### 4.4 Record
Two columns ≥1200px, stacked below that.
- **Left: Settings** (`panel` block, padding 16).
  - **Folder:** a row with path (mono 12, middle-truncated, tooltip shows the full path) + "Change…" ghost. Underneath, measured free space ("412 GB free on D:"). If the folder is missing or not writable, an inline `error-text` message + "Choose another folder".
  - **Resolution:** segmented `1080p | 720p`. **Frame rate:** segmented `Source | 30 | 60`.
  - While recording, all settings are **locked** (read-only styling with the value still at `text`, plus the note "Locked while recording"). They are not greyed to disabled.
- **Primary action** (40px, full column width):
  - Idle: "Start recording" with a 10px `rec` dot glyph on a `surface` button with `ctl-border`. Red is *not* a fill until it is true.
  - Recording: `rec` fill, `on-rec` ink, "Stop recording" + mono elapsed `00:12:31`, confirm on stop.
  - Starting or stopping: disabled + spinner + "Starting 3 ISO files…" / "Finalizing files…" with a count of files closed (measured).
  - Start is disabled with a stated reason when Engine is off, local record was denied, ISO is 0/8, or the folder is invalid.
- **Right: per-ISO table.** Header 40, rows 36, mono values. Columns:
  - Slot
  - Participant
  - File (name only, tooltip with path)
  - Duration
  - Frames written
  - Lost
  - Held (counted source stalls, labelled `held 4, source stalled` per the Pro gate rule)
  - Audio (mini horizontal meter, 48×6)
  - Size
  - State tag: `ARMED`, `REC`, `FINALIZING`, `SAVED`, `FAILED`
  - Failed rows use an `error-tint` row fill + the reason in a second line + "Open folder". After stop, rows remain with `SAVED` and real file sizes until the next start.
- **Empty.** "No ISO sources armed. Arm participants in the ISO band on Routing." + link.

---

## 5. Family resemblance: related but distinct

| Attribute | Pro | Volt | **Edge** |
|---|---|---|---|
| Ground / panel | `#0A0B0C` / `#101315` (cool) | `#141414` / `#1C1C1C` (neutral) | **Pro's cool ramp, unchanged.** Edge sits next to Pro in the same control room. |
| Owned accent | Green `#22C86E` (also "live") | Amber `#F0B429` | **Cyan `#3CC4E0`**, an accent only, never a status |
| Tally colours | Green / amber / red `#E5433F` | Green `#3DDC97` / amber / red `#FF4D4D` | **Pro's exact tallies**; red only while recording |
| Focus | Not specified | `#5EA3EA` 2px, offset 2 | **`#5EA3EA` 2px, offset 2** (family colour) |
| UI font | Space Grotesk | System UI | **Space Grotesk** |
| Data font | IBM Plex Mono, also labels | Consolas / Cascadia | **IBM Plex Mono for measured values only** |
| Labels | Uppercase mono, 0.16em | Sentence case, sans | **Sentence case Space Grotesk 12/500** |
| Radii | 8 / 10 | 3 / 6 | **4 / 6 / 8** |
| Density | Comfortable (38–40 controls) | Compact | **Compact-dense**: 36 controls, 28 matrix cells, 8 tile gap |
| Mark | Multiview frame, live tile | Four slashes | **Pro frame vocabulary + a satellite tile** (below) |
| Wordmark | Font-set, "Live production" subtitle | Geometric caps | **Font-set "CoreVideo Edge", subtitle "Guest feeds"** in 11 `text-2` |

---

## 6. Logo directions
All use a 120×120 viewBox. Mark colour is `accent` `#3CC4E0` (9.53:1 on `#0A0B0C`).
- App icon: Pro's squircle recipe. 36×36 at r 9 (scale: 120 unit → rx 30), padding 6, gradient `#15191B` → `#0B0D0E`, start (0,0) → end (0.35,1).
- Wordmark: Space Grotesk SemiBold, "CoreVideo Edge", with the mark at 1.4× cap height to the left and a 0.5× cap-height gap.

### A. Edge Column
Pro's frame, with the right column lit as stacked incoming feeds.
- Frame: `rect x=14 y=22 w=92 h=76 rx=15`, `fill=none stroke=#3CC4E0 stroke-width=6`.
- Divider: `line x1=78 y1=22 x2=78 y2=98`, stroke 5.
- Three feed tiles, each `w=18 h=19.33 rx=4` filled `#3CC4E0`, at `x=83`, `y=27 / 50.33 / 73.67` (4px gaps, 5px inset).
- The left area stays empty (the Pro programme the feeds go into).
- **Pros:** reads as "extra feeds at the side of the multiview". **Cons:** at 16px the three tiles merge into one bar, and the outline is close to Pro's at a glance.

### B. Satellite Tile (recommended)
A smaller Pro-style multiview, with a solid tile docked off its lower-right corner: the second machine feeding the main one.
- Primary frame: `rect x=10 y=18 w=78 h=60 rx=13`, `fill=none stroke=#3CC4E0 stroke-width=6`. This is Pro's r/size ratio of 15/92 ≈ 0.16, applied as 13/78 ≈ 0.17.
- Cross: `line x1=49 y1=18 x2=49 y2=78` and `line x1=10 y1=48 x2=88 y2=48`, stroke 5, `#3CC4E0`.
- Satellite tile: `rect x=62 y=60 w=50 h=38 rx=8`, `fill=#3CC4E0`, drawn **on top**, with a knockout `stroke=#0B0D0E stroke-width=6` (`paint-order: stroke`). A clean 6px gap separates it from the frame and the cross lines, echoing Volt's discrete, never-touching strokes.
- The tile's 50×38 is ≈16:12, the same aspect as Pro's live tile (36×28).
- **State rule (mirrors Pro):** only the satellite fill changes. `#3CC4E0` by default; `#E5433F` while any ISO is recording (taskbar/dock tally); `#E8A41F` while degraded. The frame never changes colour.
- **Why:**
  - Distinct silhouette from Pro at 16px (asymmetric, the corner tile breaks the outline).
  - Literally depicts "second seat feeding the main multiview".
  - Uses Pro's geometry vocabulary (stroke 6/5, rounded 16:9, a filled tile carries state), so it reads as family.
  - Cyan keeps it unmistakable next to Pro's green on a shared taskbar.

### C. Lit Edge
The Pro frame, untouched, with its right edge replaced by a solid bar.
- Frame and cross: exactly Pro's (`rect 14,22 92×76 rx15 stroke 6`; lines at x=60 and y=60, stroke 5), stroked in `#9BA4AB` neutral.
- Edge bar: `rect x=100 y=22 w=12 h=76 rx=6`, fill `#3CC4E0`, covering the frame's right stroke.
- No live tile.
- **Pros:** the most literal "edge", and closest to Pro. **Cons:** a grey frame on a dark squircle loses contrast at small sizes (2-colour mark), it could read as "Pro with a cyan bar", and the state rule has no tile to colour.

**Recommendation: B, Satellite Tile.** Wordmark lockup: mark + "CoreVideo Edge" (`#E9EDEF`), subtitle "Guest feeds" (`#9BA4AB`, 11px). No gradients behind the wordmark (Volt rule). The mark is never redrawn per state except the satellite fill.

---

## 7. Anti-patterns to avoid (and where they would creep in)

1. **Eyebrow or kicker labels** above titles ("ROUTING" above "Routing matrix"). This is an impeccable absolute ban, and it is also Pro's mono-uppercase habit. Use the title alone.
2. **Monospace as costume.** Plex Mono goes on measured numbers, paths and tags only, not on labels, buttons or headings.
3. **Hero-metric tiles** ("8 / 8 sources", big number + small label + accent) on Monitor or Record. Capacity belongs in the toolbar as a plain mono readout.
4. **Same-size icon-heading-text cards** for Connect steps, and **cards nested in cards** (a tile inside a section panel inside a page card). One container level, max.
5. **Rounded-square icon tile above every heading or empty state** (the shadcn Empty `EmptyMedia variant=icon`). Drop it.
6. **Thick coloured left stripe** on alerts, failed rows or selected rows. Use a fill tint plus 1px edges, or a full 1px border.
7. **Ghost cards:** a 1px border plus a wide soft shadow on in-window panels.
8. **Gradient text, glass or blur, neon glow, `text-shadow` glow**, and the "Real-Time Monitoring" style's blink or pulse on alerts. Failures are loud through words and placement, never flashing.
9. **Purple/blue AI gradients, or Inter / system font everywhere** (the `overused-font` tell). The fonts are bundled Space Grotesk + Plex Mono.
10. **Grey text on coloured fills.** Secondary text on tints uses `text-2` (computed ≥6.27:1) or a hue-derived tone, never `#5C656B`.
11. **Colour-only status.** Every dot carries a word. Warn and error carry an icon. Cyan never signals health.
12. **Placeholder or invented values:** "--", "0 fps" before the first frame, estimated time remaining shown as fact, demo thumbnails, fake meters. If it is not measured, it is not drawn, and the reason is stated instead.
13. **Spinner in the middle of content** for roster loads (use a skeleton for ≤2 s), and any **wait without an elapsed counter** beyond 2 s.
14. **Modal-first** flows: no modal for routing, settings or tile focus. Confirms only for destructive acts (Stop recording, Leave while recording, unroute an ISO while recording).
15. **0 ms state flips and layout-animating transitions** (Width or Height on tiles). Use opacity and transform only, 120–240 ms, honouring reduced motion.
16. **Placeholder-only form labels** (meeting link), and errors shown only in a toast or at the top of the screen instead of at the field.
17. **Stock platform accent leakage** (Pro's light-blue sliders). Override `SystemAccentColor`.
18. **Emoji or Unicode glyph icons** (●, ✓, ⚠). Use Lucide paths at 1.75 stroke.
19. **Decorative red.** Red means recording (fill) or failure (`error-text` with an icon), never branding, and never "armed".
20. **Hover-only information** on tiles or cells. Health is always visible; tooltips add detail, they never carry the only copy.

---

## 8. Verification for the build that follows
- Unit-test every token pair in §3.2 (the Volt practice: "every pair is measured by a test").
- Run an Avalonia accessibility pass: Accessibility Insights for Windows (UIA) for names, roles, focus order and live regions; on macOS, use Accessibility Inspector.
- Screenshot every state listed in §4 against a fake engine, labelled as fake in test fixtures only and never reachable in the shipped UI.
- Score with impeccable `/critique` (target ≥28/40) and `/audit` (target ≥14/20, Implementation Integrity ≥3).
