#!/bin/bash
# Upload (or replace) a file on the PRIVATE draft release that holds the Zoom
# SDK. Anything that contains Zoom SDK files goes here, never to an Actions
# artifact (artifacts on a public repo are downloadable by anyone signed in).
# Usage: scripts/ci/upload-private-asset.sh <file>   (needs GH_TOKEN, contents: write)
set -euo pipefail
file="$1"
name=$(basename "$file")
release_name="${SDK_RELEASE_NAME:-SDK assets (private storage - DO NOT PUBLISH)}"
repo="${GITHUB_REPOSITORY:?}"

release_id=$(gh api "repos/$repo/releases" \
  --jq ".[] | select(.draft and .name == \"$release_name\") | .id")
[ -n "$release_id" ] || { echo "::error::private draft release not found" >&2; exit 1; }

# Replace an older upload of the same name (one per branch).
old=$(gh api "repos/$repo/releases/$release_id/assets" --paginate \
  --jq ".[] | select(.name == \"$name\") | .id")
for id in $old; do gh api -X DELETE "repos/$repo/releases/assets/$id" >/dev/null; done

curl -sSf -X POST \
  -H "Authorization: Bearer $GH_TOKEN" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @"$file" \
  "https://uploads.github.com/repos/$repo/releases/$release_id/assets?name=$name" >/dev/null
echo "stored $name on the private draft release"
