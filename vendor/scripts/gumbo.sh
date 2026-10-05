#!/usr/bin/env bash
# F00 fetch script: Gumbo C parser from Nokogiri v1.19.4 (gumbo-parser subtree only).
# Idempotent, non-interactive. Skips when the pinned revision is already present.
# Uses a blob:none partial fetch plus sparse checkout so the Ruby tree is not kept.
set -euo pipefail

name=gumbo
url=https://github.com/sparklemotion/nokogiri
pin=8cfb9daae9ee4a0837508eab43c40fbc8c4138c9
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="$root/src/$name"

if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" = "$pin" ] && [ -d "$dir/gumbo-parser" ]; then
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
git -C "$dir" sparse-checkout init --no-cone
git -C "$dir" sparse-checkout set 'gumbo-parser'
git -C "$dir" fetch --depth 1 --filter=blob:none origin "$pin"
git -C "$dir" checkout -q FETCH_HEAD
actual="$(git -C "$dir" rev-parse HEAD)"
if [ "$actual" != "$pin" ]; then
  echo "MISMATCH: checked out $actual, expected $pin" >&2
  exit 1
fi
if [ ! -d "$dir/gumbo-parser" ]; then
  echo "ERROR: gumbo-parser subtree missing after checkout" >&2
  exit 1
fi
echo "OK: $name at $pin (sparse checkout: gumbo-parser/ only)"
