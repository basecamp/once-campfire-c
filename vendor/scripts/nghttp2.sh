#!/usr/bin/env bash
# F00 fetch script: nghttp2 v1.70.0 (HTTP/2 library)
# Idempotent, non-interactive. Skips when the pinned revision is already present.
set -euo pipefail

name=nghttp2
url=https://github.com/nghttp2/nghttp2
pin=85e300c79fb6dbcfa9c1013215c8710c1c2cd3d2
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="$root/src/$name"

if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" = "$pin" ]; then
  echo "SKIP: $name already at $pin"
  exit 0
fi
if [ -e "$dir" ]; then
  echo "removing stale $dir (not at pin)"
  rm -rf "$dir"
fi
mkdir -p "$dir"
git -C "$dir" init -q
git -C "$dir" remote add origin "$url"
git -C "$dir" fetch --depth 1 origin "$pin"
git -C "$dir" checkout -q FETCH_HEAD
actual="$(git -C "$dir" rev-parse HEAD)"
if [ "$actual" != "$pin" ]; then
  echo "MISMATCH: checked out $actual, expected $pin" >&2
  exit 1
fi
echo "OK: $name at $pin"
