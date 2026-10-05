#!/usr/bin/env python3
"""Generate the H03 ordered route table for src/routes.c from the binding
artifact docs/devel/implementation/contracts/routes.json.

The table is data only: 177 rows in the artifact's exact order with each
row's id/method/pattern/endpoint/c_symbol/task/disposition/defaults and its
bound action. `src/routes.c` embeds the emitted block verbatim between the
BEGIN/END markers; --check re-runs the generator and fails if the checked-in
block differs (route-table provenance, HTTP-07).

Binding rules (spec 01 H03, D-C08):
  - reference_handler "action_not_found"  -> cf_action_reference_action_not_found
  - reference_handler "missing_controller"-> cf_action_reference_missing_controller
  - the small built-ins 01 names (health, turbo_native, mailbox ingress, and
    the conductor rows: CSRF is a no-op for GET and A01's cf_check_csrf for
    POST)
  - everything else -> CF_ROUTE_DEV_501: the development-only 501 handler.

Usage:
  python3 tests/routes/tools/gen_routes.py                 # print the block
  python3 tests/routes/tools/gen_routes.py --check FILE    # verify FILE
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
ROUTES_JSON = os.path.join(REPO, "docs", "devel", "implementation", "contracts", "routes.json")
MARK_BEGIN = "/* ---- BEGIN GENERATED ROUTE TABLE (tests/routes/tools/gen_routes.py) ---- */"
MARK_END = "/* ---- END GENERATED ROUTE TABLE ---- */"

METHODS = {"GET": "CF_GET", "HEAD": "CF_HEAD", "POST": "CF_POST", "PUT": "CF_PUT",
           "PATCH": "CF_PATCH", "DELETE": "CF_DELETE"}

# reference_handler -> bound C action. Rows absent here use the dev-501.
BOUND = {
    "action_not_found": "cf_action_reference_action_not_found",
    "missing_controller": "cf_action_reference_missing_controller",
    "health::show": "cf_action_health_show",
    "turbo_native::recede": "cf_action_turbo_native_recede",
    "turbo_native::resume": "cf_action_turbo_native_resume",
    "turbo_native::refresh": "cf_action_turbo_native_refresh",
    "mailbox::ingress_not_configured": "cf_action_mailbox_ingress_not_configured",
    # mailbox::conductor is handled per method below.
}

# The finite matcher grammar: literals, :name, *name, (.:format).


def validate_pattern(pattern, row_id):
    """Reject any pattern the finite matcher cannot represent (or a group
    shape other than `(.:name)`)."""
    i = 0
    names = []
    while i < len(pattern):
        c = pattern[i]
        if c == ":" or c == "*":
            j = i + 1
            while j < len(pattern) and (pattern[j].isalnum() or pattern[j] == "_"):
                j += 1
            if j == i + 1:
                raise SystemExit("route %d: capture without a name in %r" % (row_id, pattern))
            names.append(pattern[i + 1:j])
            i = j
        elif c == "(":
            close = pattern.find(")", i)
            if close < 0:
                raise SystemExit("route %d: unbalanced '(' in %r" % (row_id, pattern))
            inner = pattern[i + 1:close]
            m = re.fullmatch(r"\.\:([A-Za-z0-9_]+)", inner)
            if m is None:
                raise SystemExit("route %d: unsupported group %r in %r" % (row_id, inner, pattern))
            names.append(m.group(1))
            i = close + 1
        elif c == ")":
            raise SystemExit("route %d: stray ')' in %r" % (row_id, pattern))
        else:
            i += 1
    if len(names) > 8:
        raise SystemExit("route %d: more than 8 captures in %r" % (row_id, pattern))
    return names


def c_string(text):
    out = ['"']
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            for b in ch.encode("utf-8"):
                out.append("\\%03o" % b)
    out.append('"')
    return "".join(out)


def action_for(row):
    handler = row["reference_handler"]
    if handler == "mailbox::conductor":
        return "cf_action_mailbox_conductor_get" if row["method"] in ("GET", "HEAD") \
            else "cf_action_mailbox_conductor_post"
    return BOUND.get(handler, "CF_ROUTE_DEV_501")


def generate():
    with open(ROUTES_JSON, "r", encoding="utf-8") as fh:
        artifact = json.load(fh)
    rows = artifact["routes"]
    if len(rows) != 177:
        raise SystemExit("routes.json has %d rows, expected 177" % len(rows))
    ids = [r["id"] for r in rows]
    if ids != list(range(1, 178)):
        raise SystemExit("routes.json ids are not 1..177 in order")

    lines = []
    lines.append(MARK_BEGIN)
    lines.append("/*")
    lines.append(" * Generated from docs/devel/implementation/contracts/routes.json")
    lines.append(" * (reference_commit %s) by tests/routes/tools/gen_routes.py." % artifact["reference_commit"])
    lines.append(" * 177 rows, artifact order; do not hand-edit rows. Rebinding a row to")
    lines.append(" * its packet's action is route registration and belongs to the integrator.")
    lines.append(" */")

    for row in rows:
        names = validate_pattern(row["pattern"], row["id"])
        row["_names"] = names
        if row["method"] not in METHODS:
            raise SystemExit("route %d: unknown method %s" % (row["id"], row["method"]))
        defaults = list(row["defaults"].items())
        row["_defaults"] = defaults
        if len(defaults) > 4:
            raise SystemExit("route %d: more than 4 defaults" % row["id"])
        if row["disposition"] not in ("implement", "reference_error"):
            raise SystemExit("route %d: unknown disposition %s" % (row["id"], row["disposition"]))
        if not re.fullmatch(r"cf_action_[a-z0-9_]+", row["c_symbol"]):
            raise SystemExit("route %d: unexpected c_symbol %r" % (row["id"], row["c_symbol"]))

    for row in rows:
        if row["_defaults"]:
            entries = ", ".join("{%s, %s}" % (c_string(k), c_string(v))
                                for k, v in row["_defaults"])
            lines.append("static const cf_route_default cf_defaults_r%d[] = { %s };"
                         % (row["id"], entries))
    lines.append("")

    lines.append("static const cf_route cf_route_table[177] = {")
    for row in rows:
        disp = "CF_ROUTE_IMPLEMENT" if row["disposition"] == "implement" \
            else "CF_ROUTE_REFERENCE_ERROR"
        defaults = "cf_defaults_r%d" % row["id"] if row["_defaults"] else "NULL"
        lines.append("    {%d, %s, %s, %s, %s, %s, %s,\n     %s, %d, %s}," % (
            row["id"], METHODS[row["method"]], c_string(row["pattern"]),
            c_string(row["endpoint"]), c_string(row["c_symbol"]),
            c_string(row["task"]), disp, defaults, len(row["_defaults"]),
            action_for(row)))
    lines.append("};")
    lines.append(MARK_END)
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", metavar="FILE",
                        help="verify FILE's generated block matches")
    args = parser.parse_args()
    block = generate()
    if args.check:
        with open(args.check, "r", encoding="utf-8") as fh:
            text = fh.read()
        begin = text.find(MARK_BEGIN)
        end = text.find(MARK_END)
        if begin < 0 or end < 0:
            print("error: %s has no generated route-table markers" % args.check, file=sys.stderr)
            return 1
        checked_in = text[begin:end + len(MARK_END)] + "\n"
        if checked_in != block:
            print("error: %s route table is stale; regenerate with this tool"
                  % args.check, file=sys.stderr)
            return 1
        print("OK: %s matches routes.json (%d rows)" % (args.check, 177))
        return 0
    sys.stdout.write(block)
    return 0


if __name__ == "__main__":
    sys.exit(main())
