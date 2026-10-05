# Integration cases (F02 contract)

`run.py` is the minimal integration runner required by
`docs/devel/implementation/07-verification.md`:

    python3 tests/integration/run.py --case <ID>    # one acceptance case
    python3 tests/integration/run.py --all          # every discovered case

It uses the Python standard library only. `run.py` and this README belong to
F02; `tests/integration/cases/**` belongs to V01 (and later case owners).

## Case modules

A case is a Python module at `tests/integration/cases/<anything>.py` (files
whose name starts with `_` are ignored). It declares:

| Name | Required | Meaning |
| --- | --- | --- |
| `CASE_ID` | yes | unique nonempty string, e.g. `E2E-01` (use the acceptance IDs from 07-verification.md) |
| `run()` | yes | zero-argument callable; returning normally means PASS |

`run()` reports failures by raising:

| Exception | Meaning in the report |
| --- | --- |
| `CaseFailure("...")` | the case ran and observed a wrong result |
| `PrerequisiteMissing("...")` | a prerequisite is absent; reported as `missing prerequisite: ...` |
| any other exception | reported as `error: <type>: <message>` with a traceback |

Both failure kinds are **FAIL**; the runner has no skip status. A case that
cannot run because a binary, fixture, server or tool is missing must raise
`PrerequisiteMissing` with the exact reason (path + what to build), so the
suite fails loudly instead of reporting green.

Import the exceptions and the repository root from the runner:

```python
from run import CaseFailure, PrerequisiteMissing, REPO_ROOT
```

The runner registers itself as the `run` module before loading cases, so this
import returns the same module instance that is executing.

## Example

```python
from run import CaseFailure, PrerequisiteMissing, REPO_ROOT

CASE_ID = "E2E-01"

def run():
    binary = REPO_ROOT / "build" / "campfire"
    if not binary.exists():
        raise PrerequisiteMissing(f"{binary} is not built; run make first")
    # start the server, drive it over HTTP, compare against tests/fixtures/...
    if observed != expected:
        raise CaseFailure(f"expected {expected!r}, observed {observed!r}")
```

## Rules

- Standard library only; no third-party Python packages. Drive the C server
  with `subprocess`/sockets; do not add another framework.
- Browser flows use the pinned Playwright parity project, not a new browser
  framework (`07-verification.md`, "Commands implementation must provide").
- Expected values come from `tests/fixtures/` (pinned reference bytes).
  Never regenerate expected output from the candidate C server.
- Cases must not read `tmp/` at runtime; fixtures are self-contained.
- `tests/integration/run.py --all` fails with `no cases found` while
  `cases/` is empty, and `--case <ID>` fails when the ID is unknown or its
  module cannot be imported - by design.
