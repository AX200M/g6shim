#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import os
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EQUATIONS = Path("hle") / "xg_xs" / "gfx10_xor.inc"
REFERENCE = Path("tests") / "unit" / "gfx10_reference.inc"
GENERATOR = Path(__file__).with_name("gfx10_reference_gen.cpp")
MESA_VERSION = "26.2.7"
MESA_SHA256 = "04294f75d22e9f61cd1e050206bc48b3d39366e788f1803a3368e3d5a872ce18"
MESA_URL = f"https://archive.mesa3d.org/mesa-{MESA_VERSION}.tar.xz"
SPDX = (
    "// SPDX-FileCopyrightText: 2007-2026 Advanced Micro Devices, Inc.\n"
    "// SPDX-License-Identifier: MIT\n"
)
SAMPLE_LOG2 = {1: 0, 2: 1, 4: 2, 8: 3}


class ReferenceError(Exception):
    pass


def parse_dump(text):
    equations, sizes, mips, addresses, missing = [], [], [], [], []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        parts = lines[index].split()
        index += 1
        if not parts:
            continue
        kind, nums = parts[0], [int(part) for part in parts[1:]]
        if kind == "EQ":
            if len(nums) != 8:
                raise ReferenceError("equation header")
            masks = []
            for _ in range(4):
                if index >= len(lines):
                    raise ReferenceError("truncated equation")
                row = lines[index].split()
                index += 1
                if len(row) != 17 or row[0] not in "XYZS":
                    raise ReferenceError("equation mask")
                masks.append([int(part) for part in row[1:]])
            equations.append((nums, masks))
        elif kind == "SIZE" and len(nums) == 20:
            sizes.append(nums)
        elif kind == "MIP" and len(nums) == 17:
            mips.append(nums)
        elif kind == "ADDR" and len(nums) == 17:
            addresses.append(nums)
        elif kind == "MISS":
            missing.append(nums)
        else:
            raise ReferenceError(f"bad row {kind}")
    return equations, sizes, mips, addresses, missing


def mask_text(masks):
    return "{" + ",".join("{" + ",".join(f"{value}u" for value in mask) + "}" for mask in masks) + "}"


def equation_key(nums):
    pipes, packers, swizzle, element, samples, thick, dimension, _bits = nums
    return pipes | (packers << 3) | (swizzle << 6) | (element << 11) | (SAMPLE_LOG2[samples] << 14) | (thick << 16) | (dimension << 17)


def render_equations(equations):
    lines = [SPDX.rstrip("\n"), "struct XorEquation {", "    std::uint32_t key;",
             "    std::uint8_t bits;", "    std::uint16_t mask[4][16];", "};",
             "const XorEquation kXorEquations[] = {"]
    ordered = sorted(equations, key=lambda item: equation_key(item[0]))
    for nums, masks in ordered:
        bits = nums[7]
        lines.append(f"    {{{equation_key(nums)}u, {bits}, {mask_text(masks)}}},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def row_text(row, wide):
    return ",".join(str(value) + ("ull" if index in wide else "u") for index, value in enumerate(row))


def render_reference(sizes, mips, addresses):
    lines = [SPDX.rstrip("\n"), "struct ReferenceSize {",
             "    std::uint32_t pipes_log2, packers_log2, fragments_log2, swizzle, dimension, element_log2, samples;",
             "    std::uint32_t width, height, depth, mips;", "    std::uint64_t bytes, slice;",
             "    std::uint32_t pitch, rows, block_w, block_h, block_d, alignment, first_tail;", "};",
             "const ReferenceSize kReferenceSizes[] = {"]
    lines += [f"    {{{row_text(row, {11, 12})}}}," for row in sizes]
    lines += ["};", "struct ReferenceMip {",
              "    std::uint32_t pipes_log2, packers_log2, fragments_log2, swizzle, dimension, element_log2, samples;",
              "    std::uint32_t width, height, depth, mips, mip;", "    std::uint64_t offset;",
              "    std::uint32_t tail_offset, tail_x, tail_y, in_tail;", "};",
              "const ReferenceMip kReferenceMips[] = {"]
    lines += [f"    {{{row_text(row, {12})}}}," for row in mips]
    lines += ["};", "struct ReferenceAddress {",
              "    std::uint32_t pipes_log2, packers_log2, fragments_log2, swizzle, dimension, element_log2, samples;",
              "    std::uint32_t width, height, depth, mips, mip, x, y, z, sample;", "    std::uint64_t offset;", "};",
              "const ReferenceAddress kReferenceAddresses[] = {"]
    lines += [f"    {{{row_text(row, {16})}}}," for row in addresses]
    lines.append("};")
    return "\n".join(lines) + "\n"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_tarball(cache):
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"mesa-{MESA_VERSION}.tar.xz"
    if not archive.is_file() or sha256(archive) != MESA_SHA256:
        urllib.request.urlretrieve(MESA_URL, archive)
    if sha256(archive) != MESA_SHA256:
        raise ReferenceError("Mesa tarball hash does not match the pin")
    return archive


def extract_addrlib(archive, destination):
    root = destination / f"mesa-{MESA_VERSION}" / "src" / "amd" / "addrlib"
    marker = root / "src" / "gfx10" / "gfx10addrlib.cpp"
    if marker.is_file():
        return root
    with tarfile.open(archive, "r:xz") as tar:
        members = [member for member in tar.getmembers() if "/src/amd/addrlib/" in member.name.replace("\\", "/")]
        tar.extractall(destination, members)
    if not marker.is_file():
        raise ReferenceError("address library sources were not in the tarball")
    return root


def compile_and_run(addrlib, work):
    tool = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not tool.is_file():
        raise ReferenceError("vswhere was not found")
    install = subprocess.check_output(
        [str(tool), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"], text=True).strip()
    vcvars = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    (work / "stubs.cpp").write_text(
        "namespace Addr { class Lib; struct Client;\n"
        "Lib* SiHwlInit(const Client*) { return nullptr; }\n"
        "Lib* CiHwlInit(const Client*) { return nullptr; }\n"
        "Lib* Gfx9HwlInit(const Client*) { return nullptr; }\n"
        "Lib* Gfx11HwlInit(const Client*) { return nullptr; }\n"
        "Lib* Gfx12HwlInit(const Client*) { return nullptr; }\n}\n", encoding="utf-8")
    sources = [
        "src/addrinterface.cpp", "src/core/addrelemlib.cpp", "src/core/addrlib.cpp", "src/core/addrlib1.cpp",
        "src/core/addrlib2.cpp", "src/core/addrlib3.cpp", "src/core/addrobject.cpp", "src/core/addrswizzler.cpp",
        "src/core/coord.cpp", "src/gfx10/gfx10addrlib.cpp",
    ]
    includes = " ".join(f'/I"{addrlib / path}"' for path in ("inc", "src", "src/core", "src/chip/gfx10", "src/gfx10"))
    files = " ".join(f'"{addrlib / path}"' for path in sources)
    command = (
        f'call "{vcvars}" >nul && cl /nologo /EHsc /std:c++17 /O2 /D LITTLEENDIAN_CPU /D DEBUG=0 /D ADDR_FASTCALL= '
        f'{includes} /Fe:"{work / "gen.exe"}" "{GENERATOR}" "{work / "stubs.cpp"}" {files}'
    )
    (work / "build.bat").write_text(command + "\n", encoding="utf-8")
    built = subprocess.run(["cmd", "/c", str(work / "build.bat")], cwd=work, capture_output=True, text=True)
    if built.returncode != 0:
        raise ReferenceError((built.stdout + built.stderr)[-2500:])
    ran = subprocess.run([str(work / "gen.exe")], cwd=work, capture_output=True, text=True)
    if ran.returncode != 0:
        raise ReferenceError((ran.stderr or ran.stdout)[-2500:])
    if ran.stderr:
        print(ran.stderr, file=sys.stderr, end="")
    return ran.stdout


def generate(cache):
    archive = fetch_tarball(cache)
    source = cache / "src"
    build = cache / "build"
    source.mkdir(parents=True, exist_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    dump = compile_and_run(extract_addrlib(archive, source), build)
    equations, sizes, mips, addresses, _missing = parse_dump(dump)
    if not equations or not addresses:
        raise ReferenceError("the address library produced no reference rows")
    keys = [equation_key(nums) for nums, _masks in equations]
    if len(keys) != len(set(keys)):
        raise ReferenceError("equation keys are not unique")
    return render_equations(equations), render_reference(sizes, mips, addresses)


def main(argv=None):
    parser = argparse.ArgumentParser(description="build GFX10 XOR reference addresses into committed test data")
    parser.add_argument("--check", action="store_true", help="exit 1 when the committed data is stale")
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--cache", type=Path, default=Path(os.environ.get("TEMP", ".")) / "g6shim-addrlib")
    args = parser.parse_args(argv)
    try:
        equations, reference = generate(args.cache)
    except ReferenceError as error:
        print(error, file=sys.stderr)
        return 2
    targets = ((EQUATIONS, equations), (REFERENCE, reference))
    if args.check:
        stale = False
        for relative, text in targets:
            path = args.root / relative
            current = path.read_text(encoding="utf-8").replace("\r\n", "\n") if path.is_file() else None
            if current != text:
                print(f"stale: {relative.as_posix()}; run: python tools/gfx10_reference.py")
                stale = True
        return 1 if stale else 0
    for relative, text in targets:
        path = args.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        print(f"wrote {relative.as_posix()} ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
