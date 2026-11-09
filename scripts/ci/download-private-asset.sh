#!/bin/bash
# Download a file from the PRIVATE draft release (see upload-private-asset.sh).
# Usage: scripts/ci/download-private-asset.sh <asset-name> <out-file>
set -euo pipefail
name="$1"
out="$2"
release_name="${SDK_RELEASE_NAME:-SDK assets (private storage - DO NOT PUBLISH)}"
repo="${GITHUB_REPOSITORY:?}"

url=$(gh api "repos/$repo/releases" \
  --jq ".[] | select(.draft and .name == \"$release_name\") | .assets[] | select(.name == \"$name\") | .url")
[ -n "$url" ] || { echo "::error::asset $name not on the private draft release" >&2; exit 1; }
gh api -H "Accept: application/octet-stream" "$url" > "$out"
