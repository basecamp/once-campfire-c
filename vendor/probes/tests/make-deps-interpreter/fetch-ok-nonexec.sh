#!/usr/bin/env bash
# P1A-02 regression fixture for the `make deps` interpreter fix.
#
# Committed non-executable on purpose (mode 0644): the Makefile must fall back
# to `bash "$script"` for it, so its Bash-only BASH_SOURCE code still runs.
#   make deps DEPS_SCRIPTS=vendor/probes/tests/make-deps-interpreter/fetch-ok-nonexec.sh
set -eu

[ -n "${BASH_VERSION:-}" ] || { echo "fixture: not running under bash" >&2; exit 2; }
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -n "$SCRIPT_DIR" ] || { echo "fixture: BASH_SOURCE root resolution failed" >&2; exit 2; }
echo "fixture: non-executable fetch ok (bash ${BASH_VERSION%(*}) at $SCRIPT_DIR"
exit 0
