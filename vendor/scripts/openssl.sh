#!/usr/bin/env bash
# F00: fetch OpenSSL at the revision pinned in
# docs/devel/implementation/contracts/dependencies.json (openssl-4.0.3).
# Idempotent and non-interactive: verifies and skips when already present.
set -euo pipefail

NAME="openssl"
URL="https://github.com/openssl/openssl"
PIN="af1775b60dfa141a4ad762585052cabeb9f37e9e"
DEST="$(cd "$(dirname "$0")/.." && pwd)/src/${NAME}"

if [ -e "$DEST/.git" ]; then
    HAVE="$(git -C "$DEST" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "$HAVE" = "$PIN" ]; then
        echo "${NAME}: already present at ${PIN} in ${DEST}"
        exit 0
    fi
    echo "${NAME}: ERROR ${DEST} is at ${HAVE}, expected ${PIN}" >&2
    exit 1
fi
if [ -e "$DEST" ]; then
    echo "${NAME}: ERROR ${DEST} exists but is not a git checkout" >&2
    exit 1
fi

mkdir -p "$(dirname "$DEST")"
git init -q "$DEST"
git -C "$DEST" remote add origin "$URL"
git -C "$DEST" fetch --depth 1 origin "$PIN"
git -C "$DEST" checkout -q FETCH_HEAD

HAVE="$(git -C "$DEST" rev-parse HEAD)"
if [ "$HAVE" != "$PIN" ]; then
    echo "${NAME}: ERROR checked out ${HAVE}, expected ${PIN}" >&2
    exit 1
fi
echo "${NAME}: fetched ${HAVE} into ${DEST}"
