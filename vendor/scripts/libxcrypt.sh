#!/usr/bin/env bash
# F00 fetch script: libxcrypt v4.5.2 (bcrypt crypt_r)
# Idempotent, non-interactive. Skips when the pinned revision is already present.
set -euo pipefail

name=libxcrypt
url=https://github.com/besser82/libxcrypt
pin=db70b42bd7b2a5b00a8580c8dec0aa66791c950a
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
