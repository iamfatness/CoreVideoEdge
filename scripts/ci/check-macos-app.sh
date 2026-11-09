#!/bin/bash
# Self-check of a packaged "CoreVideo Edge.app" (scripts/package-macos-app.sh).
#
#   scripts/ci/check-macos-app.sh <CoreVideo Edge.app> [--allow-missing-engine] [--allow-missing-node]
#                                 [--screenshot-out <dir>]
#
# Fails on: missing structure, wrong Info.plist keys, a signature that does not
# verify, a non-arm64 app binary, Launch Services not mapping corevideo:// to
# this bundle, the app binary failing to render a screen headlessly, and a
# missing node or engine. The two --allow flags exist only for the app-only CI
# loop while the native port lands; what is missing is printed as an error-level
# annotation either way, never hidden.
set -uo pipefail

app="${1:?usage: $0 <app> [--allow-missing-engine] [--allow-missing-node] [--screenshot-out <dir>]}"
shift
allow_engine=0; allow_node=0; shot_dir=""
while [ $# -gt 0 ]; do
  case "$1" in
    --allow-missing-engine) allow_engine=1; shift ;;
    --allow-missing-node) allow_node=1; shift ;;
    --screenshot-out) shot_dir="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
repo="$(cd "$(dirname "$0")/../.." && pwd)"
contents="$app/Contents"
plist="$contents/Info.plist"
failures=0
pass() { echo "  ok    $*"; }
fail() { echo "  FAIL  $*"; echo "::error::check-macos-app: $*"; failures=$((failures + 1)); }

echo "== structure =="
for f in "$plist" "$contents/PkgInfo" "$contents/Resources/CoreVideoEdge.icns" "$contents/Resources/edge-bundle.txt"; do
  [ -f "$f" ] && pass "${f#$app/}" || fail "missing ${f#$app/}"
done
[ -x "$contents/MacOS/CoreVideoEdge" ] && pass "Contents/MacOS/CoreVideoEdge" || fail "missing app executable"
archs=$(lipo -archs "$contents/MacOS/CoreVideoEdge" 2>/dev/null || true)
[[ " $archs " == *" arm64 "* ]] && pass "app binary is arm64 ($archs)" || fail "app binary is not arm64 ($archs)"

echo "== Info.plist =="
key() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null; }
expect() {
  local got; got=$(key "$1")
  [ "$got" = "$2" ] && pass "$1 = $got" || fail "$1 is '$got', expected '$2'"
}
plutil -lint "$plist" >/dev/null && pass "plist is valid" || fail "plist does not lint"
expect CFBundleIdentifier us.iamfatness.corevideo.edge
expect CFBundleExecutable CoreVideoEdge
expect CFBundleName "CoreVideo Edge"
expect CFBundlePackageType APPL
expect CFBundleIconFile CoreVideoEdge
expect LSMinimumSystemVersion 13.0
expect NSHighResolutionCapable true
expect "CFBundleURLTypes:0:CFBundleURLSchemes:0" corevideo
version=$(key CFBundleShortVersionString)
csproj_version=$(sed -n 's:.*<Version>\(.*\)</Version>.*:\1:p' "$repo/app/CoreVideoEdge.Desktop/CoreVideoEdge.Desktop.csproj" | head -1)
[ -n "$version" ] && [ "$version" = "$csproj_version" ] && pass "version $version (from the csproj)" \
  || fail "CFBundleShortVersionString '$version' != csproj '$csproj_version'"
[ -n "$(key NSLocalNetworkUsageDescription)" ] && pass "NSLocalNetworkUsageDescription" || fail "no NSLocalNetworkUsageDescription (NDI/SRT to Pro)"

echo "== signature =="
if codesign --verify --deep --strict --verbose=2 "$app" 2>&1; then pass "codesign verifies"; else fail "codesign does not verify"; fi
codesign -dv "$app" 2>&1 | grep -E '^(Identifier|Signature|TeamIdentifier)=' | sed 's/^/        /'

echo "== node and engine =="
node_ok=1
for f in corevideo-edge corevideo-edge-ndi-probe; do
  [ -x "$contents/MacOS/$f" ] || node_ok=0
done
if [ $node_ok -eq 1 ]; then
  pass "node + ndi-probe in Contents/MacOS"
elif [ $allow_node -eq 1 ]; then
  echo "::error::check-macos-app: NO NODE in this bundle (allowed for the app-only loop; this app cannot run a meeting)"
else
  fail "node (corevideo-edge, corevideo-edge-ndi-probe) missing from Contents/MacOS"
fi
engine="$contents/Helpers/corevideo-zoom-engine.app"
if [ -x "$engine/Contents/MacOS/corevideo-zoom-engine" ]; then
  pass "engine bundle in Contents/Helpers"
  [ -d "$engine/Contents/Frameworks/ZoomSDK.framework" ] && pass "engine carries ZoomSDK.framework" || fail "engine has no ZoomSDK.framework"
elif [ $allow_engine -eq 1 ]; then
  echo "::error::check-macos-app: NO ZOOM ENGINE in this bundle (allowed for the app-only loop; real Zoom joins cannot work)"
else
  fail "engine missing: Contents/Helpers/corevideo-zoom-engine.app"
fi
sed 's/^/        /' "$contents/Resources/edge-bundle.txt" 2>/dev/null

echo "== Launch Services: corevideo:// opens this bundle =="
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$lsregister" -f "$app" || true
handler=$(osascript -l JavaScript -e '
  ObjC.import("AppKit");
  var u = $.NSWorkspace.sharedWorkspace.URLForApplicationToOpenURL($.NSURL.URLWithString("corevideo://oauth/callback"));
  u.isNil() ? "" : ObjC.unwrap(u.path);' 2>/dev/null || true)
real_app=$(cd "$app" && pwd -P)
if [ "$handler" = "$real_app" ]; then
  pass "corevideo:// -> $handler"
elif [ -n "$handler" ]; then
  fail "corevideo:// opens '$handler', not this bundle (another app claims the scheme on this machine)"
else
  fail "Launch Services has no handler for corevideo://"
fi

echo "== the app runs: headless screen render =="
out="${shot_dir:-$(mktemp -d)}"
mkdir -p "$out"
png="$out/selfcheck-connect.png"
if "$contents/MacOS/CoreVideoEdge" --screenshot "$repo/docs/screenshots/fixtures/state-idle.json" connect "$png" --size 1280x800; then
  size=$(stat -f %z "$png" 2>/dev/null || echo 0)
  [ "$size" -gt 20000 ] && pass "rendered $png ($size bytes)" || fail "render produced a near-empty PNG ($size bytes)"
else
  fail "CoreVideoEdge --screenshot exited non-zero"
fi

echo
if [ $failures -gt 0 ]; then
  echo "check-macos-app: $failures failure(s)"
  exit 1
fi
echo "check-macos-app: all checks passed"
