#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import io
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gfx10_reference as gr

DUMP = """EQ 4 3 25 2 1 0 0 16
X 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
Y 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0
Z 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
EQ 2 1 25 2 1 0 0 16
X 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
Y 0 8 0 0 0 0 0 0 0 0 0 0 0 0 0 0
Z 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
SIZE 4 3 3 25 2 2 1 64 48 1 1 65536 65536 64 64 64 64 1 65536 1
MIP 4 3 3 25 2 2 1 64 48 1 1 0 0 0 0 0 0
ADDR 4 3 3 25 2 2 1 64 48 1 1 0 1 3 0 0 52
MISS 4 3 3 23 2 2 1 64 48 1 1
"""


class ParseTests(unittest.TestCase):
    def test_rows(self):
        equations, sizes, mips, addresses, missing = gr.parse_dump(DUMP)
        self.assertEqual(len(equations), 2)
        self.assertEqual(equations[0][0][2], 25)
        self.assertEqual(equations[0][1][0][0], 1)
        self.assertEqual(sizes[0][11], 65536)
        self.assertEqual(mips[0][11], 0)
        self.assertEqual(addresses[0][-1], 52)
        self.assertEqual(missing[0][3], 23)

    def test_bad_row(self):
        with self.assertRaises(gr.ReferenceError):
            gr.parse_dump("NOPE 1\n")


class RenderTests(unittest.TestCase):
    def test_header_and_order(self):
        equations, sizes, mips, addresses, _missing = gr.parse_dump(DUMP)
        text = gr.render_equations(equations)
        self.assertTrue(text.startswith(gr.SPDX))
        self.assertIn("licence MIT", text)
        self.assertLess(text.find("2 |"), text.find("4 |") if "4 |" in text else len(text))
        keys = [line for line in text.splitlines() if line.startswith("    {")]
        self.assertEqual(len(keys), 2)
        reference = gr.render_reference(sizes, mips, addresses)
        self.assertIn("kReferenceAddresses", reference)
        self.assertIn("52ull", reference)
        self.assertIn("licence MIT", reference)


class MainTests(unittest.TestCase):
    def test_check_reports_stale(self):
        with mock.patch.object(gr, "generate", return_value=("eq\n", "ref\n")):
            code = gr.main(["--check", "--root", str(Path(__file__).parent)])
        self.assertEqual(code, 1)

    def test_generate_error(self):
        with mock.patch.object(gr, "generate", side_effect=gr.ReferenceError("missing")):
            err = io.StringIO()
            with mock.patch.object(sys, "stderr", err):
                code = gr.main([])
        self.assertEqual(code, 2)
        self.assertIn("missing", err.getvalue())


if __name__ == "__main__":
    unittest.main()
