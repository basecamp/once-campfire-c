#!/usr/bin/env bash
# F00 fetch script: SQLite 3.53.4 amalgamation zip (dependencies.json source URL).
# Idempotent and non-interactive: skips when the extracted version is already present.
# The archive is verified against the SHA3-256 published on https://sqlite.org/download.html
# (the published value for this exact archive is pinned below).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC="$ROOT/vendor/src/sqlite"

NAME=sqlite
VERSION=3.53.4
ZIPNAME="sqlite-amalgamation-3530400.zip"
URL="https://sqlite.org/2026/$ZIPNAME"
PUBLISHED_SHA3_256="628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"
ZIP="$SRC/$ZIPNAME"
DIR="$SRC/sqlite-amalgamation-3530400"

if [ -f "$DIR/sqlite3.c" ] && [ -f "$DIR/sqlite3.h" ]; then
  if grep -qE "^#define SQLITE_VERSION[[:space:]]+\"$VERSION\"" "$DIR/sqlite3.h"; then
    echo "$NAME: already extracted $VERSION at $DIR"
    exit 0
  fi
  echo "$NAME: ERROR extracted tree does not report version $VERSION" >&2
  exit 1
fi
if [ -e "$DIR" ] && [ -n "$(ls -A "$DIR" 2>/dev/null)" ]; then
  echo "$NAME: ERROR $DIR exists but is not the expected $VERSION tree; refusing to overwrite" >&2
  exit 1
fi

mkdir -p "$SRC"
if [ ! -f "$ZIP" ]; then
  curl -fL --retry 3 --retry-delay 2 -o "$ZIP" "$URL"
fi
actual_sha3="$(openssl dgst -sha3-256 "$ZIP" | awk '{print $NF}')"
if [ "$actual_sha3" != "$PUBLISHED_SHA3_256" ]; then
  echo "$NAME: ERROR SHA3-256 mismatch: $actual_sha3 != $PUBLISHED_SHA3_256" >&2
  exit 1
fi
echo "$NAME: archive SHA3-256 verified (published value)"

unzip -q -o "$ZIP" -d "$SRC"
if ! grep -qE "^#define SQLITE_VERSION[[:space:]]+\"$VERSION\"" "$DIR/sqlite3.h"; then
  echo "$NAME: ERROR extracted sqlite3.h does not report version $VERSION" >&2
  exit 1
fi
echo "$NAME: extracted version $VERSION to $DIR"
