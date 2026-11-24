#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gfx10_fixtures as gf

CODE = struct.pack("<3I", 0xBE850380, 0x7E000301, 0xBF810000)


def elf(sections):
    names = b"\0" + b"".join(name + b"\0" for name, _ in sections) + b".shstrtab\0"
    blobs, offsets, offset = [], [], gf.ELF_HEADER.size
    for _, data in sections:
        offsets.append(offset)
        blobs.append(data)
        offset += len(data)
    names_offset = offset
    offset += len(names)
    shoff = offset
    headers = [gf.SECTION_HEADER.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    name_at = 1
    for (name, data), data_offset in zip(sections, offsets):
        headers.append(gf.SECTION_HEADER.pack(name_at, 1, 0, 0, data_offset, len(data), 0, 0, 4, 0))
        name_at += len(name) + 1
    headers.append(gf.SECTION_HEADER.pack(name_at, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0))
    ident = gf.ELF_MAGIC + bytes([gf.ELF_CLASS_64, gf.ELF_LITTLE_ENDIAN, 1]) + bytes(9)
    count = len(headers)
    header = gf.ELF_HEADER.pack(ident, 1, 224, 1, 0, 0, shoff, 0, gf.ELF_HEADER.size, 0, 0,
                                gf.SECTION_HEADER.size, count, count - 1)
    return header + b"".join(blobs) + names + b"".join(headers)


class ElfTests(unittest.TestCase):
    def test_text_section_is_found_among_others(self):
        self.assertEqual(gf.text_section(elf([(b".note", b"abcd"), (b".text", CODE)])), CODE)

    def test_similar_names_do_not_match(self):
        with self.assertRaisesRegex(gf.FixtureError, "no .text"):
            gf.text_section(elf([(b".text.cold", CODE)]))

    def test_damaged_objects_are_refused(self):
        good = elf([(b".text", CODE)])
        cases = {
            b"not an elf": "not an ELF",
            good[:4] + b"\1" + good[5:]: "64-bit",
            good[:40] + struct.pack("<Q", len(good)) + good[48:]: "outside the object",
            good[:62] + struct.pack("<H", 9) + good[64:]: "out of range",
            good[:-30]: "outside the object",
        }
        for data, message in cases.items():
            with self.subTest(message=message):
                with self.assertRaisesRegex(gf.FixtureError, message):
                    gf.text_section(data)

    def test_words(self):
        self.assertEqual(gf.words_of(CODE), [0xBE850380, 0x7E000301, 0xBF810000])
        with self.assertRaisesRegex(gf.FixtureError, "whole number"):
            gf.words_of(CODE[:-1])


class RenderTests(unittest.TestCase):
    def test_rows_and_header(self):
        text = gf.render([("a", list(range(7))), ("b", [0xFFFFFFFF])])
        self.assertTrue(text.startswith(gf.HEADER))
        self.assertIn('{"a",\n {0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u, 0x00000005u,\n'
                      '  0x00000006u}},\n', text)
        self.assertIn('{"b",\n {0xFFFFFFFFu}},\n', text)

    def test_empty_program_is_refused(self):
        with self.assertRaisesRegex(gf.FixtureError, "no code"):
            gf.render([("a", [])])


class MainTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / gf.SOURCES).mkdir(parents=True)
        (self.root / gf.OUTPUT).parent.mkdir(parents=True)
        for name in ("b_rule", "a_rule"):
            (self.root / gf.SOURCES / f"{name}.s").write_text("s_endpgm\n", encoding="utf-8")

    def tearDown(self):
        self.directory.cleanup()

    def run_main(self, *argv, assembled=None):
        out, err = io.StringIO(), io.StringIO()
        assemble = mock.patch.object(gf, "assemble", side_effect=assembled or (lambda tool, path: elf([(b".text", CODE)])))
        with assemble, contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gf.main([*argv, "--root", str(self.root), "--llvm-mc", "llvm-mc"])
        return code, out.getvalue(), err.getvalue()

    def test_write_then_check(self):
        self.assertEqual(self.run_main("--check")[0], 1)
        code, out, _ = self.run_main()
        self.assertEqual((code, out), (0, "wrote tests/unit/gfx10_fixtures.inc\n"))
        text = (self.root / gf.OUTPUT).read_text(encoding="utf-8")
        self.assertLess(text.index('"a_rule"'), text.index('"b_rule"'))
        self.assertEqual(self.run_main("--check"), (0, "", ""))
        (self.root / gf.OUTPUT).write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
        self.assertEqual(self.run_main("--check")[0], 0)
        code, out, _ = self.run_main("--check", assembled=lambda tool, path: elf([(b".text", CODE[:8])]))
        self.assertEqual(code, 1)
        self.assertIn("stale", out)

    def test_assembler_errors_and_missing_programs(self):
        def fail(tool, path):
            raise gf.FixtureError(f"{path.name}: bad instruction")
        code, _, err = self.run_main(assembled=fail)
        self.assertEqual(code, 2)
        self.assertIn("bad instruction", err)
        for path in (self.root / gf.SOURCES).glob("*.s"):
            path.unlink()
        code, _, err = self.run_main()
        self.assertEqual(code, 2)
        self.assertIn("no programs", err)

    def test_llvm_mc_is_required(self):
        with mock.patch.dict("os.environ", {"LLVM_MC": ""}), mock.patch.object(gf.shutil, "which", return_value=None):
            with self.assertRaisesRegex(gf.FixtureError, "no llvm-mc"):
                gf.find_llvm_mc(None)
            self.assertEqual(gf.find_llvm_mc("x/llvm-mc"), "x/llvm-mc")
        with mock.patch.dict("os.environ", {"LLVM_MC": "env/llvm-mc"}):
            self.assertEqual(gf.find_llvm_mc(None), "env/llvm-mc")


if __name__ == "__main__":
    unittest.main()
