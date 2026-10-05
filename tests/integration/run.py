#!/usr/bin/env python3
"""Minimal integration-case runner (F02).

Usage
-----
    python3 tests/integration/run.py --case <ID>
    python3 tests/integration/run.py --all

Cases are Python modules in tests/integration/cases/*.py (V01 owns that
directory).  Every module declares a unique string CASE_ID and a zero-argument
callable run():

    from run import CaseFailure, PrerequisiteMissing, REPO_ROOT

    CASE_ID = "E2E-01"

    def run():
        binary = REPO_ROOT / "build" / "campfire"
        if not binary.exists():
            raise PrerequisiteMissing(f"{binary} not built; run make first")
        ...
        if observed != expected:
            raise CaseFailure(f"got {observed!r}, expected {expected!r}")

Returning normally is a pass.  CaseFailure and PrerequisiteMissing both make
the case FAIL with the message as its reason; PrerequisiteMissing only labels
the failure as a missing prerequisite.  Any other exception is reported as an
error with a traceback.  There is no skip status: a case that cannot run must
fail with its reason.

Python standard library only.  The runner exits 0 when every selected case
passes, 1 when any case fails or cannot be found, and 2 on usage errors.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
import traceback
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CASES_DIR = Path(__file__).resolve().parent / "cases"


class CaseFailure(Exception):
    """The case ran and observed a wrong result."""


class PrerequisiteMissing(Exception):
    """The case could not run because a prerequisite is absent; still a FAIL."""


# Allow `from run import ...` from case modules loaded by this script.
sys.modules.setdefault("run", sys.modules[__name__])


class CaseError:
    """A module that could not be loaded or does not satisfy the contract."""

    def __init__(self, label: str, reason: str):
        self.label = label
        self.reason = reason


class Case:
    def __init__(self, module, path: Path):
        self.module = module
        self.path = path

    @property
    def case_id(self) -> str:
        return self.module.CASE_ID


def load_case(path: Path):
    spec = importlib.util.spec_from_file_location(f"cf_case_{path.stem}", path)
    if spec is None or spec.loader is None:
        return CaseError(path.name, "cannot create an import spec")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as exc:  # import-time failure is a failure, not a skip
        return CaseError(path.name, f"import failed: {type(exc).__name__}: {exc}")
    case_id = getattr(module, "CASE_ID", None)
    if not isinstance(case_id, str) or not case_id:
        return CaseError(path.name, "module does not declare a nonempty string CASE_ID")
    run = getattr(module, "run", None)
    if not callable(run):
        return CaseError(path.name, "module does not declare a callable run()")
    return Case(module, path)


def discover():
    if not CASES_DIR.is_dir():
        return []
    return [load_case(path) for path in sorted(CASES_DIR.glob("*.py")) if not path.name.startswith("_")]


def run_case(case: Case) -> bool:
    try:
        case.module.run()
    except PrerequisiteMissing as exc:
        print(f"FAIL {case.case_id}: missing prerequisite: {exc}")
        return False
    except CaseFailure as exc:
        print(f"FAIL {case.case_id}: {exc}")
        return False
    except Exception as exc:
        print(f"FAIL {case.case_id}: error: {type(exc).__name__}: {exc}")
        traceback.print_exc(file=sys.stderr)
        return False
    print(f"PASS {case.case_id}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Campfire integration case runner")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--case", metavar="ID", help="run the case with this CASE_ID")
    group.add_argument("--all", action="store_true", help="run every discovered case")
    args = parser.parse_args()

    discovered = discover()
    cases = [x for x in discovered if isinstance(x, Case)]
    errors = [x for x in discovered if isinstance(x, CaseError)]

    seen: dict[str, Case] = {}
    duplicates: list[tuple[Case, Case]] = []
    for case in cases:
        if case.case_id in seen:
            duplicates.append((seen[case.case_id], case))
        else:
            seen[case.case_id] = case
    duplicates_failed = set()
    for first, second in duplicates:
        duplicates_failed.add(first.path)
        duplicates_failed.add(second.path)

    if args.all:
        if not discovered:
            print(f"FAIL --all: no cases found under {CASES_DIR.relative_to(REPO_ROOT)}")
            print("integration: 0 case(s), 0 passed, 1 failed")
            return 1
        selected: list = list(discovered)
    else:
        matches = [case for case in cases if case.case_id == args.case]
        if not matches:
            known = ", ".join(sorted(seen)) if seen else "none"
            detail = f"CASE_ID {args.case!r} not found (discovered: {known})"
            for error in errors:
                detail += f"; {error.label}: {error.reason}"
            print(f"FAIL {args.case}: {detail}")
            print("integration: 0 case(s), 0 passed, 1 failed")
            return 1
        selected = matches

    passed = 0
    failed = 0
    for item in selected:
        if isinstance(item, CaseError):
            print(f"FAIL {item.label}: {item.reason}")
            failed += 1
            continue
        case: Case = item
        if case.path in duplicates_failed:
            others = ", ".join(
                sorted(other.path.name for other in cases if other.case_id == case.case_id and other.path != case.path)
            )
            print(f"FAIL {case.case_id}: duplicate CASE_ID also declared by {others}")
            failed += 1
            continue
        if run_case(case):
            passed += 1
        else:
            failed += 1

    print(f"integration: {passed + failed} case(s), {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
