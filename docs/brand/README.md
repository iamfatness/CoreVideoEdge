# CoreVideo Edge brand

Edge is CoreVideo Pro's satellite seat: Pro's neutral ramp, fonts and tally
colours, with its own accent, **Edge cyan**, so a cyan selection can never be
mistaken for a healthy (green) feed.

## Mark: "Satellite Tile"

`mark.svg` (120 x 120): a Pro-style multiview frame (stroke 6, cross stroke 5)
with a solid tile docked off its lower-right corner, the second machine feeding
the main one. `lockup.svg`: mark + "CoreVideo Edge" (Space Grotesk SemiBold,
`#E9EDEF`) + "Guest feeds" (11, `#9BA4AB`). No gradient behind the wordmark.

App icon: the mark in Pro's squircle (36 x 36 at r 9, padding 6, gradient
`#15191B` to `#0B0D0E`), rendered from the same geometry by
`CoreVideoEdge --render-brand <dir>` into `CoreVideoEdge.ico` (16-256) and
`icon-1024.png` (macOS, later).

**State rule:** only the satellite fill changes. `#3CC4E0` by default,
`#E5433F` while any ISO records, `#E8A41F` while something is degraded. The
frame never changes colour. Never redraw the mark per state otherwise.

## Colours (dark only)

| Token | Hex | Use |
|---|---|---|
| bg / panel / surface / raised | `#0A0B0C` / `#101315` / `#16191B` / `#1B1F22` | Pro's neutral ramp |
| text / text-2 | `#E9EDEF` / `#9BA4AB` | primary / secondary |
| accent | `#3CC4E0` (ink `#04161B`) | selection, primary action, the mark; never health |
| live | `#22C86E` | receiving, engine on |
| warn | `#E8A41F` | degraded, waiting |
| rec | `#E5433F` (ink `#1A0000`) | recording only (fill, dot) |
| error-text | `#FF7A6E` | failure text and icons (red text is never `rec`) |
| focus | `#5EA3EA` | family focus ring, 2 px, 2 px offset |

The full table and every contrast pair: `app/CoreVideoEdge.Desktop/Theme/Tokens.axaml`,
checked by `TokenContrastTests`.

## Type

Space Grotesk for all UI (sentence case labels). IBM Plex Mono only for measured
values, paths and the uppercase tally tags (`REC`, `ISO`). Both OFL, bundled.

## Usage

- Status is never colour alone: dot + word, plus an icon for warn and error.
- Red means recording (fill) or failure (`error-text` + icon), never branding.
