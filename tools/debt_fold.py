#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEDGER = Path("docs/dev/debt")
CHANGES = LEDGER / "changes"
SECTIONS = ("Build", "Silent stubs", "Unknown function info", "Functional")
MODULE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*")
OPERATION = re.compile(r"## (add|change|remove) (.+)")
TRAILER = re.compile(r"^  (Closes when|Title IDs): (.+)$")
TITLE_ID = re.compile(r"[0-9A-F]{8}")


class FoldError(Exception):
    pass


@dataclass
class Operation:
    where: str
    module: str
    kind: str
    target: str
    entries: list = field(default_factory=list)


def ledger_path(root, module):
    if not MODULE.fullmatch(module) or ".." in module:
        raise FoldError(f"invalid module name: {module!r}")
    return root / LEDGER / f"{module}.md"


def split_entries(lines, where):
    entries, current = [], None
    for number, line in enumerate(lines, 1):
        if line.startswith("- "):
            current = [line]
            entries.append(current)
        elif line.startswith("  ") and current is not None and line.strip():
            current.append(line)
        elif line.strip():
            raise FoldError(f"{where}:{number}: expected an entry ('- ...') or its indented continuation")
        else:
            current = None
    for entry in entries:
        check_entry(entry, where)
    return ["\n".join(e) for e in entries]


def check_entry(entry, where):
    for line in entry[1:]:
        trailer = TRAILER.match(line)
        if trailer and trailer.group(1) == "Title IDs":
            for title in re.split(r"[,\s]+", trailer.group(2).strip()):
                if title and not TITLE_ID.fullmatch(title):
                    raise FoldError(f"{where}: '{title}' is not an 8-digit hex title ID")


def parse_fragment(path, root):
    where = path.relative_to(root).as_posix()
    operations, module, current, body = [], None, None, []

    def finish():
        if current is not None:
            current.entries = split_entries(body, where)

    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if line.startswith("# "):
            finish()
            module, current, body = line[2:].strip(), None, []
            if not ledger_path(root, module).is_file():
                raise FoldError(f"{where}:{number}: no ledger file for module {module}")
        elif match := OPERATION.fullmatch(line.strip()):
            finish()
            if module is None:
                raise FoldError(f"{where}:{number}: operation before a '# <module>' line")
            kind, target = match.group(1), match.group(2).strip()
            if kind == "add" and target not in SECTIONS:
                raise FoldError(f"{where}:{number}: unknown section '{target}'; use one of {', '.join(SECTIONS)}")
            current, body = Operation(f"{where}:{number}", module, kind, target), []
            operations.append(current)
        elif line.startswith("#"):
            raise FoldError(f"{where}:{number}: expected '# <module>' or '## add|change|remove ...'")
        elif current is None:
            if line.strip():
                raise FoldError(f"{where}:{number}: text outside an operation")
        else:
            body.append(line)
    finish()
    for op in operations:
        wanted = {"add": len(op.entries) >= 1, "change": len(op.entries) == 1, "remove": not op.entries}[op.kind]
        if not wanted:
            raise FoldError(f"{op.where}: '{op.kind}' takes {'one or more entries' if op.kind == 'add' else 'one entry' if op.kind == 'change' else 'no entry'}")
    if not operations:
        raise FoldError(f"{where}: no operations")
    return operations


@dataclass
class Ledger:
    head: list
    sections: dict

    @staticmethod
    def parse(text, where):
        head, sections, name = [], {}, None
        lines = text.splitlines()
        body = []
        for line in lines:
            if line.startswith("## "):
                if name is not None:
                    sections[name] = split_entries(body, where)
                name, body = line[3:].strip(), []
                if name not in SECTIONS:
                    raise FoldError(f"{where}: unknown section '{name}'")
            elif name is None:
                head.append(line)
            else:
                body.append(line)
        if name is not None:
            sections[name] = split_entries(body, where)
        return Ledger(head, sections)

    def render(self):
        out = list(self.head)
        while out and not out[-1].strip():
            out.pop()
        for name in SECTIONS:
            out += ["", f"## {name}", ""]
            out += self.sections.get(name) or ["None."]
        return "\n".join(out) + "\n"

    def find(self, target, where):
        hits = [(name, i) for name, entries in self.sections.items() for i, e in enumerate(entries) if target in e]
        if len(hits) != 1:
            raise FoldError(f"{where}: '{target}' matches {len(hits)} entries, not one")
        return hits[0]


def load_ledger(root, module):
    path = ledger_path(root, module)
    text = path.read_text(encoding="utf-8").replace("\n\nNone.\n", "\n\n")
    return Ledger.parse(text, path.relative_to(root).as_posix())


def fold(root, write):
    fragments = sorted(p for p in (root / CHANGES).glob("*.md"))
    operations = [op for path in fragments for op in parse_fragment(path, root)]
    ledgers, claimed = {}, {}
    for op in operations:
        if op.module not in ledgers:
            ledgers[op.module] = load_ledger(root, op.module)
        if op.kind != "add":
            key = (op.module, ledgers[op.module].find(op.target, op.where))
            if key in claimed and claimed[key].where.split(":")[0] != op.where.split(":")[0]:
                raise FoldError(f"{op.where} and {claimed[key].where} change the same entry; merge them")
            claimed[key] = op
    for op in operations:
        ledger = ledgers[op.module]
        if op.kind == "add":
            ledger.sections.setdefault(op.target, []).extend(op.entries)
            continue
        name, index = ledger.find(op.target, op.where)
        if op.kind == "change":
            ledger.sections[name][index] = op.entries[0]
        else:
            del ledger.sections[name][index]
    texts = {module: ledger.render() for module, ledger in ledgers.items()}
    if write:
        for module, text in texts.items():
            ledger_path(root, module).write_text(text, encoding="utf-8", newline="\n")
        for path in fragments:
            path.unlink()
    return texts, fragments


def check_ledgers(root):
    for path in sorted((root / LEDGER).glob("*.md")):
        if path.name == "README.md":
            continue
        Ledger.parse(path.read_text(encoding="utf-8").replace("\n\nNone.\n", "\n\n"), path.relative_to(root).as_posix())


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description="fold the ledger fragments in docs/dev/debt/changes/ into the ledger")
    parser.add_argument("--check", action="store_true", help="check the ledger and that every fragment applies")
    parser.add_argument("--show", metavar="MODULE", help="print MODULE's ledger with the pending fragments applied")
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        if args.check:
            check_ledgers(args.root)
        texts, fragments = fold(args.root, write=not (args.check or args.show))
        if args.show:
            path = ledger_path(args.root, args.show)
            if not path.is_file():
                raise FoldError(f"no ledger file for module {args.show}")
            sys.stdout.write(texts.get(args.show, load_ledger(args.root, args.show).render()))
        elif not args.check:
            print(f"folded {len(fragments)} fragment(s) into {len(texts)} ledger file(s)")
    except FoldError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
