#!/usr/bin/env bash
# P1A-02 regression fixture for the `make deps` interpreter fix.
#
# Mimics vendor/scripts/*.sh: a Bash-only fetch script that computes its root
# from ${BASH_SOURCE[0]}. If make ran this through `sh` while /bin/sh is Dash,
# the substitution fails. Invoked with:
#   make deps DEPS_SCRIPTS=vendor/probes/tests/make-deps-interpreter/fetch-ok.sh
# Expected: exit 0 under a host sh of both bash and dash.
set -eu

[ -n "${BASH_VERSION:-}" ] || { echo "fixture: not running under bash" >&2; exit 2; }
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -n "$SCRIPT_DIR" ] || { echo "fixture: BASH_SOURCE root resolution failed" >&2; exit 2; }
echo "fixture: checked fetch ok (bash ${BASH_VERSION%(*}) at $SCRIPT_DIR"
exit 0
