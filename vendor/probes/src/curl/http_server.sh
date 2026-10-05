#!/usr/bin/env bash
# Local HTTP server fixture for the F00 curl probes (clang and Fil-C).
#
# Committed replacement for the ad-hoc `python3 -m http.server` snippet in the
# recorded curl build commands: it binds 127.0.0.1 itself, waits until the
# listener is ready before running the client, and shuts the server down
# cleanly (kill + wait) on any exit path so no orphan process survives.
#
# Usage:
#   vendor/probes/src/curl/http_server.sh <www-root> <command> [args...]
#
# Any argument equal to "@PORT@" is replaced with the chosen free port before
# the command is executed; the port is also exported as CAMPFIRE_PROBE_PORT.
# The exit status is the command's exit status (or 3 if the server never became
# ready). Set CAMPFIRE_HTTP_SERVER_LOG to capture the server log (default
# /dev/null).
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 <www-root> <command> [args...]" >&2
  exit 2
fi

WWW_ROOT="$1"
shift
[ -d "$WWW_ROOT" ] || { echo "http_server: no such directory: $WWW_ROOT" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "http_server: python3 not found" >&2; exit 2; }

PORT="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$WWW_ROOT" \
  >"${CAMPFIRE_HTTP_SERVER_LOG:-/dev/null}" 2>&1 &
SERVER_PID=$!

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

ready=0
for _ in $(seq 1 100); do
  if python3 - "$PORT" <<'PY' 2>/dev/null
import socket
import sys
socket.create_connection(("127.0.0.1", int(sys.argv[1])), 0.2).close()
PY
  then
    ready=1
    break
  fi
  sleep 0.05
done
if [ "$ready" -ne 1 ]; then
  echo "http_server: server did not become ready on 127.0.0.1:$PORT" >&2
  exit 3
fi

args=()
for arg in "$@"; do
  args+=("${arg//@PORT@/$PORT}")
done

export CAMPFIRE_PROBE_PORT="$PORT"
rc=0
"${args[@]}" || rc=$?
exit "$rc"
