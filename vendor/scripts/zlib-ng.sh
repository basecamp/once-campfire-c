#!/usr/bin/env bash
# F00 fetch script: zlib-ng 2.3.3 at the pin in docs/devel/implementation/contracts/dependencies.json
# Idempotent and non-interactive: skips when the pinned commit is already checked out.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC="$ROOT/vendor/src/zlib-ng"

NAME=zlib-ng
URL="https://github.com/zlib-ng/zlib-ng"
PIN="12731092979c6d07f42da27da673a9f6c7b13586"
REF="2.3.3"

if [ -e "$SRC/.git" ]; then
  actual="$(git -C "$SRC" rev-parse HEAD)"
  if [ "$actual" = "$PIN" ]; then
    echo "$NAME: already fetched at pinned commit $PIN"
    exit 0
  fi
  echo "$NAME: ERROR existing checkout at $actual does not match pin $PIN" >&2
  exit 1
fi
if [ -e "$SRC" ] && [ -n "$(ls -A "$SRC" 2>/dev/null)" ]; then
  echo "$NAME: ERROR $SRC exists but is not a git checkout; refusing to overwrite" >&2
  exit 1
fi

mkdir -p "$SRC"
git -C "$SRC" init -q
git -C "$SRC" remote add origin "$URL"
if ! git -C "$SRC" fetch --depth 1 origin "$PIN"; then
  if [ -n "$REF" ]; then
    echo "$NAME: direct commit fetch failed, trying ref refs/tags/$REF" >&2
    git -C "$SRC" fetch --depth 1 origin "refs/tags/$REF"
  else
    echo "$NAME: ERROR cannot fetch commit $PIN" >&2
    exit 1
  fi
fi
git -C "$SRC" checkout -q FETCH_HEAD

actual="$(git -C "$SRC" rev-parse HEAD)"
if [ "$actual" != "$PIN" ]; then
  echo "$NAME: ERROR checked out $actual != pin $PIN" >&2
  exit 1
fi
echo "$NAME: fetched and verified $PIN"
