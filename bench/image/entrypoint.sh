#!/usr/bin/env bash
# Container entrypoint for the C production image (bench/image/Dockerfile.c).
#
# bench/run starts every app with HTTP_PORT (public listener) and TARGET_PORT
# (app listener behind the front server) and mounts the per-rep seed copy at
# /rails/storage/db and /rails/storage/files. The C port is one process, so it
# listens directly on HTTP_PORT; TARGET_PORT is accepted and ignored.
#
# DISABLE_SSL is forced to the C port's "1": the pinned reference.env carries
# the Rails value "true", which the C config parser rejects, and TLS is out of
# scope for these loopback measurements (D-C06).
set -euo pipefail

export HOST="${HOST:-0.0.0.0}"
export PORT="${HTTP_PORT:-${PORT:-3000}}"
export PUBLIC_ORIGIN="${PUBLIC_ORIGIN:-http://127.0.0.1:${PORT}}"
export DATABASE_PATH="${DATABASE_PATH:-/rails/storage/db/production.sqlite3}"
export STORAGE_PATH="${STORAGE_PATH:-/rails/storage/files}"
export DISABLE_SSL=1

# Complete-body cache budget, bytes (06-cache-performance.md ablation:
# uncached -> whole-body cache).  The image default is the C config default,
# 0 (cache disabled); bench/run-c passes the arm explicitly
# (`--cache-bytes 67108864` for the cache-enabled arm) through the harness's
# CF_CACHE_BYTES forwarding.
export CF_CACHE_BYTES="${CF_CACHE_BYTES:-0}"

# HTTP loop threads.  The C config default is 1; the published methodology
# allocates four hardware threads per app, so benchmark arms set CF_LOOPS=4
# (`bench/run-c --loops 4`) to match the references' worker counts.
export CF_LOOPS="${CF_LOOPS:-1}"

exec /app/campfire "$@"
