#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = Path("tests") / "gfx10"
OUTPUT = Path("tests") / "unit" / "gfx10_fixtures.inc"
TRIPLE = "amdgcn-amd-amdpal"
CPU = "gfx1030"
HEADER = (
    "// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors\n"
    "// SPDX-License-Identifier: GPL-3.0-or-later\n"
)
WORDS_PER_LINE = 6
ELF_MAGIC = b"\x7fELF"
ELF_CLASS_64 = 2
ELF_LITTLE_ENDIAN = 1
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
TEXT = b".text"


class FixtureError(Exception):
    pass


def section_bytes(elf, header, index):
    entry = header["shoff"] + index * header["shentsize"]
    if header["shentsize"] < SECTION_HEADER.size or entry + SECTION_HEADER.size > len(elf):
        raise FixtureError(f"section header {index} is outside the object")
    name, _, _, _, offset, size, _, _, _, _ = SECTION_HEADER.unpack_from(elf, entry)
    if offset + size > len(elf):
        raise FixtureError(f"section {index} is outside the object")
    return name, elf[offset:offset + size]


def text_section(elf):
    if len(elf) < ELF_HEADER.size or elf[:4] != ELF_MAGIC:
        raise FixtureError("not an ELF object")
    if elf[4] != ELF_CLASS_64 or elf[5] != ELF_LITTLE_ENDIAN:
        raise FixtureError("not a little-endian 64-bit ELF object")
    fields = ELF_HEADER.unpack_from(elf, 0)
    header = {"shoff": fields[6], "shentsize": fields[11], "shnum": fields[12], "shstrndx": fields[13]}
    if header["shstrndx"] >= header["shnum"]:
        raise FixtureError("section name table index is out of range")
    _, names = section_bytes(elf, header, header["shstrndx"])
    for index in range(header["shnum"]):
        name_offset, data = section_bytes(elf, header, index)
        end = names.find(b"\0", name_offset)
        if name_offset < len(names) and end >= 0 and names[name_offset:end] == TEXT:
            return data
    raise FixtureError("no .text section")


def words_of(code):
    if len(code) % 4:
        raise FixtureError(f"code is {len(code)} bytes, not a whole number of dwords")
    return [value for (value,) in struct.iter_unpack("<I", code)]


def render(fixtures):
    lines = [HEADER.rstrip("\n")]
    for name, words in fixtures:
        if not words:
            raise FixtureError(f"{name} has no code")
        lines.append(f'{{"{name}",')
        rows = [words[i:i + WORDS_PER_LINE] for i in range(0, len(words), WORDS_PER_LINE)]
        body = [", ".join(f"0x{w:08X}u" for w in row) for row in rows]
        lines.append(" {" + (",\n  ".join(body)) + "}},")
    return "\n".join(lines) + "\n"


def assemble(llvm_mc, source):
    with tempfile.TemporaryDirectory() as directory:
        target = Path(directory) / "fixture.o"
        result = subprocess.run(
            [llvm_mc, f"-triple={TRIPLE}", f"-mcpu={CPU}", "-filetype=obj", "-o", str(target), str(source)],
            capture_output=True, text=True)
        if result.returncode != 0:
            raise FixtureError(f"{source.name}: {result.stderr.strip()}")
        return target.read_bytes()


def generate(root, llvm_mc):
    sources = sorted((root / SOURCES).glob("*.s"))
    if not sources:
        raise FixtureError(f"no programs in {SOURCES.as_posix()}")
    return render([(path.stem, words_of(text_section(assemble(llvm_mc, path)))) for path in sources])


def find_llvm_mc(given):
    candidate = given or os.environ.get("LLVM_MC") or shutil.which("llvm-mc")
    if not candidate:
        raise FixtureError("no llvm-mc: pass --llvm-mc or set LLVM_MC (it needs the AMDGPU target)")
    return candidate


def main(argv=None):
    parser = argparse.ArgumentParser(description="assemble tests/gfx10/*.s into tests/unit/gfx10_fixtures.inc")
    parser.add_argument("--llvm-mc", help="llvm-mc with the AMDGPU target (default: LLVM_MC, then PATH)")
    parser.add_argument("--check", action="store_true", help="exit 1 when the committed words are stale")
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        text = generate(args.root, find_llvm_mc(args.llvm_mc))
    except FixtureError as error:
        print(error, file=sys.stderr)
        return 2
    output = args.root / OUTPUT
    if args.check:
        current = output.read_bytes().decode("utf-8").replace("\r\n", "\n") if output.is_file() else None
        if current != text:
            print(f"stale: {OUTPUT.as_posix()}; run: python tools/gfx10_fixtures.py")
            return 1
        return 0
    output.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
