#!/bin/bash
# Assembles "CoreVideo Edge.app" (Apple Silicon) from the operator app, the node
# and the Zoom engine bundle, and signs it ad hoc.
#
#   scripts/package-macos-app.sh --node <dir> [--engine <corevideo-zoom-engine.app>] --out <dir>
#                                [--allow-missing-node]
#
#   --node <dir>     folder holding corevideo-edge and corevideo-edge-ndi-probe
#                    (and corevideo-zoom-engine-fake when built), e.g. build/mac.
#   --engine <app>   the engine bundle from scripts/make-macos-engine-bundle.sh.
#                    It carries ZoomSDK.framework: a bundle built WITH it goes to
#                    the private draft release only, never to an Actions artifact.
#                    Without it the app has no real engine (Zoom joins cannot work);
#                    the bundle says so in Contents/Resources/edge-bundle.txt and
#                    scripts/ci/check-macos-app.sh fails unless told to allow it.
#   --allow-missing-node   package even when --node has no node yet (app-only
#                    CI loop while the native port is in progress). Recorded the
#                    same way; never silent.
#
# Layout (the node finds the engine at <nodeDir>/../Helpers/...):
#   CoreVideo Edge.app/Contents/
#     Info.plist  PkgInfo
#     MacOS/CoreVideoEdge (+ .NET runtime, self-contained)  corevideo-edge  corevideo-edge-ndi-probe
#     Helpers/corevideo-zoom-engine.app
#     Resources/CoreVideoEdge.icns  edge-bundle.txt
#
# Signing is ad hoc (no Developer ID certificate yet, no notarization). On the
# owner's Mac, Gatekeeper blocks a downloaded copy until it is opened once with
# right-click > Open, or: xattr -dr com.apple.quarantine "/Applications/CoreVideo Edge.app"
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
node_dir=""; engine=""; out=""; allow_missing_node=0
while [ $# -gt 0 ]; do
  case "$1" in
    --node) node_dir="$2"; shift 2 ;;
    --engine) engine="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --allow-missing-node) allow_missing_node=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$out" ] || { echo "usage: $0 --node <dir> [--engine <engine.app>] --out <dir> [--allow-missing-node]" >&2; exit 2; }
[ "$(uname -s)" = "Darwin" ] || { echo "error: packaging needs macOS (codesign, iconutil, sips)" >&2; exit 1; }

app_name="CoreVideo Edge"
bundle_id="us.iamfatness.corevideo.edge"
exe_name="CoreVideoEdge"
csproj="$repo/app/CoreVideoEdge.Desktop/CoreVideoEdge.Desktop.csproj"
version=$(sed -n 's:.*<Version>\(.*\)</Version>.*:\1:p' "$csproj" | head -1)
[ -n "$version" ] || { echo "error: no <Version> in $csproj" >&2; exit 1; }
build_number="${GITHUB_RUN_NUMBER:-0}"

mkdir -p "$out"
out="$(cd "$out" && pwd)"
app="$out/$app_name.app"
work="$out/.package-work"
rm -rf "$app" "$work"
mkdir -p "$work" "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Helpers"

echo "== publish the operator app (osx-arm64, self-contained) =="
dotnet publish "$csproj" -c Release -r osx-arm64 --self-contained true \
  -p:UseAppHost=true -p:Version="$version" -o "$work/publish"
cp -R "$work/publish/." "$app/Contents/MacOS/"
chmod +x "$app/Contents/MacOS/$exe_name"

notes=()
echo "== node =="
node_found=0
if [ -n "$node_dir" ] && [ -x "$node_dir/corevideo-edge" ]; then
  for f in corevideo-edge corevideo-edge-ndi-probe corevideo-zoom-engine-fake; do
    if [ -x "$node_dir/$f" ]; then
      cp "$node_dir/$f" "$app/Contents/MacOS/$f"
      echo "  + $f"
    elif [ "$f" != corevideo-zoom-engine-fake ]; then
      echo "error: $node_dir/$f is missing" >&2; exit 1
    fi
  done
  node_found=1
elif [ $allow_missing_node -eq 1 ]; then
  echo "::warning::NO NODE in this bundle (--allow-missing-node): the app will report that corevideo-edge is missing."
  notes+=("node: MISSING (packaged with --allow-missing-node; the app cannot run a meeting)")
else
  echo "error: no corevideo-edge in '${node_dir:-<none>}' (pass --allow-missing-node for an app-only bundle)" >&2
  exit 1
fi
[ $node_found -eq 1 ] && notes+=("node: present")

echo "== engine =="
if [ -n "$engine" ]; then
  [ -x "$engine/Contents/MacOS/corevideo-zoom-engine" ] || { echo "error: $engine is not an engine bundle" >&2; exit 1; }
  cp -R "$engine" "$app/Contents/Helpers/corevideo-zoom-engine.app"
  notes+=("engine: present (contains the Zoom SDK: private release only)")
else
  echo "::warning::NO ZOOM ENGINE in this bundle: real Zoom meetings cannot be joined with it."
  notes+=("engine: MISSING (no --engine; real Zoom joins cannot work)")
fi

echo "== ad-hoc signing (code in Contents/MacOS) =="
# First, so the app can run to render its icon (arm64 runs only signed code).
# Every loose Mach-O in Contents/MacOS (the .NET host, native libraries, the node).
while IFS= read -r -d '' f; do
  if file -b "$f" | grep -q 'Mach-O'; then
    codesign --force --sign - --timestamp=none "$f"
  fi
done < <(find "$app/Contents/MacOS" -type f -print0)
echo "== icon =="
brand="$work/brand"
"$app/Contents/MacOS/$exe_name" --render-brand "$brand"
iconset="$work/$exe_name.iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
  sips -z $size $size "$brand/icon-mac-1024.png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
  double=$((size * 2))
  sips -z $double $double "$brand/icon-mac-1024.png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/$exe_name.icns"

echo "== Info.plist =="
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleName</key><string>$app_name</string>
  <key>CFBundleDisplayName</key><string>$app_name</string>
  <key>CFBundleIdentifier</key><string>$bundle_id</string>
  <key>CFBundleExecutable</key><string>$exe_name</string>
  <key>CFBundleIconFile</key><string>$exe_name</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version.$build_number</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>LSApplicationCategoryType</key><string>public.app-category.video</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSRequiresAquaSystemAppearance</key><false/>
  <key>NSHumanReadableCopyright</key><string>CoreVideo Edge</string>
  <key>CFBundleURLTypes</key>
  <array>
    <dict>
      <key>CFBundleURLName</key><string>$bundle_id.oauth</string>
      <key>CFBundleTypeRole</key><string>Viewer</string>
      <key>CFBundleURLSchemes</key><array><string>corevideo</string></array>
    </dict>
  </array>
  <key>NSLocalNetworkUsageDescription</key>
  <string>CoreVideo Edge sends each routed guest to CoreVideo Pro on your network over NDI or SRT.</string>
  <key>NSBonjourServices</key><array><string>_ndi._tcp</string></array>
</dict>
</plist>
PLIST
plutil -lint "$app/Contents/Info.plist"
printf 'APPL????' > "$app/Contents/PkgInfo"

{
  echo "CoreVideo Edge $version (build $build_number), packaged $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: ${GITHUB_SHA:-$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)}"
  for n in "${notes[@]}"; do echo "$n"; done
} > "$app/Contents/Resources/edge-bundle.txt"

echo "== ad-hoc signing (bundle) =="
helper="$app/Contents/Helpers/corevideo-zoom-engine.app"
if [ -d "$helper" ]; then
  # Keep the engine bundle's own signature when it is intact (it seals ZoomSDK.framework).
  codesign --verify --deep --strict "$helper" 2>/dev/null || codesign --force --deep --sign - --timestamp=none "$helper"
fi
codesign --force --sign - --timestamp=none "$app"
codesign --verify --deep --strict --verbose=2 "$app"

rm -rf "$work"
echo "packaged $app"
cat "$app/Contents/Resources/edge-bundle.txt"
