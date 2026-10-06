#!/usr/bin/env python3
"""Translate the pinned Marcel tables (tmp/rust-ref/crates/storage/src/tables.rs,
dumped from marcel 1.1.0 by reference-tools/storage/dump_tables.rb) into the C
header src/storage/marcel_tables.h.

Usage:
    python3 tests/fixtures/tools/marcel_tables.py \
        tmp/rust-ref/crates/storage/src/tables.rs src/storage/marcel_tables.h

The generated header is data only (no code): four ordered tables matching the
Rust statics (EXTENSIONS, TYPE_EXTS, TYPE_PARENTS, MAGIC), with the recursive
Match structures flattened into named child arrays. marcel.rs in the same
pinned crate defines the lookup semantics; src/storage/marcel.c ports those.
"""

import re
import sys


def read_section(src: str, name: str) -> str:
    start = src.index("pub static " + name + ":")
    start = src.index("= &[", start) + len("= &[")
    end = src.index("\n];", start)
    return src[start:end]


def unescape_rust(s: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.extend(c.encode("utf-8"))
            i += 1
            continue
        i += 1
        e = s[i]
        if e == "x":
            out.append(int(s[i + 1 : i + 3], 16))
            i += 3
        elif e == "n":
            out.append(0x0A)
            i += 1
        elif e == "r":
            out.append(0x0D)
            i += 1
        elif e == "t":
            out.append(0x09)
            i += 1
        elif e == "0":
            out.append(0)
            i += 1
        elif e == "\\":
            out.append(0x5C)
            i += 1
        elif e == '"':
            out.append(0x22)
            i += 1
        elif e == "'":
            out.append(0x27)
            i += 1
        elif e == "u":
            j = s.index("}", i)
            out.extend(chr(int(s[i + 2 : j], 16)).encode("utf-8"))
            i = j + 1
        else:
            raise SystemExit("unknown escape \\%s" % e)
    return bytes(out)


def c_bytes(data: bytes) -> str:
    if not data:
        return "NULL, 0"
    parts = []
    for b in data:
        if 32 <= b < 127 and b not in (34, 39, 92):
            parts.append("'%s'" % chr(b))
        else:
            parts.append("0x%02x" % b)
    return "(const unsigned char[]){%s}, %d" % (", ".join(parts), len(data))


class Match:
    def __init__(self, offset, range_end, value, children):
        self.offset = offset
        self.range_end = range_end
        self.value = value
        self.children = children


def parse_matches(text: str):
    """Parse `&[Match { .. }, ...]` bodies (recursive)."""
    matches = []
    i = 0

    def skip_ws():
        nonlocal i
        while i < len(text) and text[i] in " \t\n\r":
            i += 1

    def parse_one():
        nonlocal i
        skip_ws()
        assert text.startswith("Match {", i), text[i : i + 20]
        i += len("Match {")
        fields = {}
        while True:
            skip_ws()
            if text[i] == "}":
                i += 1
                break
            m = re.match(r"(offset|range_end|value|children)\s*:", text[i:])
            assert m, text[i : i + 40]
            field = m.group(1)
            i += m.end()
            skip_ws()
            if field == "offset":
                m2 = re.match(r"\d+", text[i:])
                fields["offset"] = int(m2.group(0))
                i += m2.end()
            elif field == "range_end":
                if text.startswith("Some(", i):
                    m2 = re.match(r"Some\((\d+)\)", text[i:])
                    fields["range_end"] = int(m2.group(1))
                    i += m2.end()
                else:
                    assert text.startswith("None", i)
                    fields["range_end"] = None
                    i += 4
            elif field == "value":
                if text.startswith("Some(", i):
                    i += len("Some(")
                    m2 = re.match(r'b"((?:[^"\\]|\\.)*)"', text[i:])
                    assert m2, text[i : i + 40]
                    fields["value"] = unescape_rust(m2.group(1))
                    i += m2.end()
                    assert text[i] == ")"
                    i += 1
                else:
                    assert text.startswith("None", i)
                    fields["value"] = None
                    i += 4
            elif field == "children":
                assert text.startswith("&[", i)
                i += 2
                children = []
                while True:
                    skip_ws()
                    if text.startswith("]", i):
                        i += 1
                        break
                    children.append(parse_one())
                    skip_ws()
                    if text.startswith(",", i):
                        i += 1
                fields["children"] = children
            skip_ws()
            if text.startswith(",", i):
                i += 1
        return Match(
            fields["offset"],
            fields["range_end"],
            fields["value"],
            fields.get("children", []),
        )

    skip_ws()
    while i < len(text):
        skip_ws()
        if i >= len(text):
            break
        matches.append(parse_one())
        skip_ws()
        if i < len(text) and text[i] == ",":
            i += 1
    return matches


def emit_match_arrays(out, matches, prefix, counter):
    """Emit child arrays bottom-up; return the array symbol for `matches`."""
    child_symbols = []
    for m in matches:
        if m.children:
            child_symbols.append(
                emit_match_arrays(out, m.children, "%s_c%d" % (prefix, counter[0]), counter)
            )
            counter[0] += 1
        else:
            child_symbols.append("NULL")
    symbol = "%s_m%d" % (prefix, counter[0])
    counter[0] += 1
    out.append("static const struct marcel_match %s[] = {" % symbol)
    for m, csym in zip(matches, child_symbols):
        child_expr = "NULL, 0" if csym == "NULL" else "%s, %d" % (csym, len(m.children))
        if m.value is None:
            value_expr = "NULL, 0"
        else:
            value_expr = c_bytes(m.value)
        range_expr = "SIZE_MAX" if m.range_end is None else str(m.range_end)
        out.append(
            "    {%s, %s, %s, %s}," % (value_expr, m.offset, range_expr, child_expr)
        )
    out.append("};")
    return symbol


def parse_pairs_single(section: str):
    pairs = []
    for line in section.splitlines():
        line = line.strip()
        if not line.startswith("("):
            continue
        m = re.match(r'\("((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\),?$', line)
        if not m:
            raise SystemExit("bad pair line: " + line)
        pairs.append(
            (unescape_rust(m.group(1)), unescape_rust(m.group(2)))
        )
    return pairs


def parse_pairs_list(section: str):
    pairs = []
    for line in section.splitlines():
        line = line.strip()
        if not line.startswith("("):
            continue
        m = re.match(r'\("((?:[^"\\]|\\.)*)", &\[(.*)\]\s*\),?$', line)
        if not m:
            raise SystemExit("bad list pair line: " + line)
        items = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
        pairs.append(
            (
                unescape_rust(m.group(1)),
                [unescape_rust(item) for item in items],
            )
        )
    return pairs


def parse_magic(section: str):
    rows = []
    for line in section.splitlines():
        line = line.strip()
        if not line.startswith("("):
            continue
        m = re.match(r'\("((?:[^"\\]|\\.)*)", &\[(.*)\]\s*\),?$', line)
        if not m:
            raise SystemExit("bad magic line: " + line)
        rows.append((unescape_rust(m.group(1)), parse_matches(m.group(2))))
    return rows


def cstr(data: bytes) -> str:
    """NUL-terminated C string literal for ASCII table keys."""
    text = data.decode("utf-8")
    assert "\x00" not in text
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def main() -> int:
    src_path, out_path = sys.argv[1], sys.argv[2]
    src = open(src_path, "r", encoding="utf-8").read()

    extensions = parse_pairs_single(read_section(src, "EXTENSIONS"))
    type_exts = parse_pairs_list(read_section(src, "TYPE_EXTS"))
    type_parents = parse_pairs_list(read_section(src, "TYPE_PARENTS"))
    magic = parse_magic(read_section(src, "MAGIC"))

    out = []
    out.append("/* @generated by tests/fixtures/tools/marcel_tables.py from the pinned")
    out.append(" * tmp/rust-ref/crates/storage/src/tables.rs (marcel 1.1.0). Do not edit.")
    out.append(" * Regenerate after any pin change; src/storage/marcel.c owns the lookups. */")
    out.append("#ifndef CF_STORAGE_MARCEL_TABLES_H")
    out.append("#define CF_STORAGE_MARCEL_TABLES_H")
    out.append("")
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("struct marcel_match {")
    out.append("    const unsigned char *value; /* NULL when absent */")
    out.append("    size_t value_len;")
    out.append("    size_t offset;")
    out.append("    size_t range_end; /* SIZE_MAX when None */")
    out.append("    const struct marcel_match *children;")
    out.append("    size_t children_len;")
    out.append("};")
    out.append("")
    out.append("static const char *const marcel_extension_keys[] = {")
    for ext, _ in extensions:
        out.append("    %s," % cstr(ext))
    out.append("};")
    out.append("static const char *const marcel_extension_types[] = {")
    for _, ct in extensions:
        out.append("    %s," % cstr(ct))
    out.append("};")
    out.append(
        "#define MARCEL_EXTENSIONS_COUNT %d" % len(extensions)
    )
    out.append("")

    counter = [0]
    for name, table in (("TYPE_EXTS", type_exts), ("TYPE_PARENTS", type_parents)):
        key_sym = "marcel_%s_keys" % name.lower()
        len_sym = "marcel_%s_lens" % name.lower()
        data_sym = "marcel_%s_data" % name.lower()
        out.append("static const char *const %s[] = {" % key_sym)
        for key, _ in table:
            out.append("    %s," % cstr(key))
        out.append("};")
        out.append("static const size_t %s[] = {" % len_sym)
        for _, items in table:
            out.append("    %d," % len(items))
        out.append("};")
        out.append(
            "static const char *const *const %s[] = {" % data_sym
        )
        for i, (_, items) in enumerate(table):
            if not items:
                out.append("    NULL,")
            else:
                out.append("    (const char *const[]){")
                for item in items:
                    out.append("        %s," % cstr(item))
                out.append("    },")
        out.append("};")
        out.append(
            "#define MARCEL_%s_COUNT %d" % (name, len(table))
        )
        out.append("")

    out.append("static const char *const marcel_magic_types[] = {")
    symbols = []
    for ct, matches in magic:
        out.append("    %s," % cstr(ct))
    out.append("};")
    for i, (_, matches) in enumerate(magic):
        symbols.append(
            emit_match_arrays(out, matches, "marcel_magic_%d" % i, counter)
        )
    out.append("static const struct marcel_match *const marcel_magic_matches[] = {")
    for sym in symbols:
        out.append("    %s," % sym)
    out.append("};")
    out.append("static const size_t marcel_magic_match_counts[] = {")
    for _, matches in magic:
        out.append("    %d," % len(matches))
    out.append("};")
    out.append("#define MARCEL_MAGIC_COUNT %d" % len(magic))

    def reach(matches):
        best = 0
        for m in matches:
            value_len = 0 if m.value is None else len(m.value)
            own = (m.range_end + value_len) if m.range_end is not None else (m.offset + value_len)
            best = max(best, own, reach(m.children))
        return best

    out.append(
        "#define MARCEL_MAGIC_PREFIX_LEN %d"
        % max((reach(matches) for _, matches in magic), default=0)
    )
    out.append("")
    out.append("#endif /* CF_STORAGE_MARCEL_TABLES_H */")
    out.append("")

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out))
    print(
        "marcel tables: extensions=%d type_exts=%d type_parents=%d magic=%d"
        % (len(extensions), len(type_exts), len(type_parents), len(magic))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
