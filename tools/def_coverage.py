#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STUB_PREFIX = "g6shim_stub_"
HEADS_ENDING_EXPORTS = {"HEAPSIZE", "STACKSIZE", "SECTIONS", "VERSION", "DESCRIPTION", "STUB"}
MAX_DEF_BYTES = 4 << 20


def parse_def(path):
    text = path.read_bytes()[:MAX_DEF_BYTES].decode("utf-8", "replace")
    library, exports, in_exports = None, [], False
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        line = re.sub(r"\s*=\s*", "=", line)
        tokens = line.split()
        head = tokens[0].upper()
        if head == "EXPORTS":
            in_exports, tokens = True, tokens[1:]
            if not tokens:
                continue
        elif head in ("LIBRARY", "NAME"):
            library = tokens[1].strip('"') if len(tokens) > 1 else library
            in_exports = False
            continue
        elif head in HEADS_ENDING_EXPORTS:
            in_exports = False
            continue
        if in_exports:
            exports.append(tokens[0])
    return library or f"{path.stem}.dll", exports


def classify(export):
    entry, _, internal = export.partition("=")
    target = internal or entry
    if target.startswith(STUB_PREFIX):
        return "stubbed"
    if "." in internal:
        return "forwarded"
    return "implemented"


def coverage(hle):
    rows = []
    for path in sorted(hle.glob("*/*.def")):
        dll, exports = parse_def(path)
        counts = {"implemented": 0, "forwarded": 0, "stubbed": 0}
        for export in exports:
            counts[classify(export)] += 1
        rows.append({"dll": dll, **counts, "total": len(exports)})
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description="count implemented, forwarded and stubbed exports per replacement DLL")
    parser.add_argument("--hle", type=Path, default=ROOT / "hle", help="folder holding the replacement DLL projects")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args(argv)
    rows = coverage(args.hle)
    if args.json:
        print(json.dumps(rows, indent=2))
        return 0
    width = max([len(r["dll"]) for r in rows] + [3])
    print(f"{'DLL':<{width}}  implemented  forwarded  stubbed  total")
    totals = {"implemented": 0, "forwarded": 0, "stubbed": 0, "total": 0}
    for r in rows:
        print(f"{r['dll']:<{width}}  {r['implemented']:>11}  {r['forwarded']:>9}  {r['stubbed']:>7}  {r['total']:>5}")
        for key in totals:
            totals[key] += r[key]
    print(f"{'all':<{width}}  {totals['implemented']:>11}  {totals['forwarded']:>9}  {totals['stubbed']:>7}  {totals['total']:>5}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
