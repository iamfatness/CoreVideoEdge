#!/bin/bash
# macOS/Linux twin of render-screenshots.ps1: renders every screen and key state
# headlessly (no window) with the given app binary.
#
#   scripts/render-screenshots.sh <CoreVideoEdge binary> <outDir> [capturesDir]
#
# State lines come from <capturesDir>/state-<name>.json when present (from
# scripts/capture-ui-fixtures.mjs against a real node), else from the committed
# docs/screenshots/fixtures. Preview copies exist only in captures; without them
# Monitor tiles show "Waiting for the node's preview". Fixture state contains no
# Zoom SDK content (fake-engine captures).
set -euo pipefail
exe="${1:?usage: $0 <app binary> <outDir> [capturesDir]}"
out="${2:?usage: $0 <app binary> <outDir> [capturesDir]}"
captures="${3:-}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
fixtures="$repo/docs/screenshots/fixtures"
mkdir -p "$out"

state() {
  if [ -n "$captures" ] && [ -f "$captures/state-$1.json" ]; then echo "$captures/state-$1.json"; else echo "$fixtures/state-$1.json"; fi
}
preview() {
  local name="${1%.edited}"
  if [ -n "$captures" ] && [ -d "$captures/preview-$name" ]; then printf '%s\n' --preview-dir "$captures/preview-$name"; fi
}

# name|state|screen|extra args (space separated)
shots=(
  "connect-signed-out|idle|connect|"
  "connect-sign-in-pending|idle|connect|--sign-in-pending"
  "connect-signed-in|idle|connect|--signed-in --meeting-link https://zoom.us/j/81234567890?pwd=fake"
  "connect-join-failed|join-failed.edited|connect|--signed-in --meeting-link https://zoom.us/j/81234567890"
  "connect-in-meeting|in-meeting|connect|--signed-in"
  "connect-engine-off|engine-off|connect|--signed-in"
  "connect-moving-rooms|moving-rooms|connect|--signed-in"
  "connect-node-not-answering|in-meeting|connect|--signed-in --state-age 12"
  "routing-not-joined|idle|routing|--signed-in"
  "routing-nothing-routed|joined-empty|routing|--signed-in"
  "routing-in-meeting|in-meeting|routing|--signed-in"
  "routing-recording|recording|routing|--signed-in"
  "routing-feeds-ndi|feeds-ndi|routing|--signed-in --size 1600x900"
  "routing-feeds-srt|feeds-srt|routing|--signed-in --size 1600x900"
  "routing-narrow|in-meeting|routing|--signed-in --size 1024x720"
  "monitor-not-joined|idle|monitor|--signed-in"
  "monitor-nothing-routed|joined-empty|monitor|--signed-in"
  "monitor-in-meeting|in-meeting|monitor|--signed-in"
  "monitor-recording|recording|monitor|--signed-in"
  "monitor-engine-off|engine-off|monitor|--signed-in"
  "monitor-moving-rooms|moving-rooms|monitor|--signed-in"
  "monitor-feeds-ndi|feeds-ndi|monitor|--signed-in"
  "monitor-feeds-srt|feeds-srt|monitor|--signed-in"
  "record-not-joined|idle|record|--signed-in"
  "record-armed|in-meeting|record|--signed-in"
  "record-engine-off|engine-off|record|--signed-in"
  "record-recording|recording|record|--signed-in"
  "record-saved|saved|record|--signed-in"
)
for s in "${shots[@]}"; do
  IFS='|' read -r name st screen extra <<< "$s"
  args=(--screenshot "$(state "$st")" "$screen" "$out/$name.png")
  while IFS= read -r a; do [ -n "$a" ] && args+=("$a"); done < <(preview "$st")
  # shellcheck disable=SC2206
  [ -n "$extra" ] && args+=($extra)
  "$exe" "${args[@]}"
  echo "rendered $out/$name.png"
done
"$exe" --render-about "$out/about.png"
echo "rendered $out/about.png"
