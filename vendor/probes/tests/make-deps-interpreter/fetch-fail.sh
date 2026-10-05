#!/usr/bin/env bash
# P1A-02 regression fixture for the `make deps` interpreter fix.
#
# A deliberately failing pinned fetch: a Bash-only script (BASH_SOURCE use,
# like vendor/scripts/*.sh) that exits 7. `make deps` must stop and return
# nonzero, proving a failing fetch still fails the target.
#   make deps DEPS_SCRIPTS=vendor/probes/tests/make-deps-interpreter/fetch-fail.sh
set -eu

[ -n "${BASH_VERSION:-}" ] || { echo "fixture: not running under bash" >&2; exit 2; }
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "fixture: deliberate fetch failure at $SCRIPT_DIR" >&2
exit 7
