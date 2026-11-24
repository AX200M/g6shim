#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import progress_map as pm

README = "# Project\n\n## Progress\n\nTable.\n\n## Build\n\nSteps.\n"
TITLE = "0123ABCD"


def squares(svg, colour):
    return len(re.findall(f'<rect [^>]*fill="{colour}"', svg))


class ProgressMapTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / pm.PROGRESS).mkdir(parents=True)
        (self.root / pm.README).write_text(README, encoding="utf-8")
        self.write_def("alpha", "alpha.dll", ["A=g6shim_A", "B=g6shim_stub_B", "C=kernel32.C", "D=g6shim_D"])
        self.write_def("beta", "beta.dll", ["E=g6shim_stub_E"])

    def tearDown(self):
        self.directory.cleanup()

    def write_def(self, folder, dll, exports):
        path = self.root / pm.HLE / folder / f"{folder}.def"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f'LIBRARY "{dll}"\nEXPORTS\n' + "".join(f"    {e}\n" for e in exports), encoding="utf-8")

    def write_json(self, name, data):
        (self.root / pm.PROGRESS / name).write_text(json.dumps(data), encoding="utf-8")

    def write_imports(self):
        self.write_json(f"{TITLE}.imports.json", {
            "title_id": TITLE,
            "modules": [
                {"dll": "ALPHA.dll", "functions": ["A", "B", "C", "Z"]},
                {"dll": "gamma.dll", "functions": ["G"]},
            ],
        })

    def generate(self):
        return {path.relative_to(self.root).as_posix(): text for path, text in pm.generate(self.root).items()}

    def test_without_titles_only_the_dll_map_and_readme(self):
        texts = self.generate()
        self.assertEqual(sorted(texts), ["README.md", "docs/progress/hle.svg"])
        hle = texts["docs/progress/hle.svg"]
        self.assertIn("+1 forwarded", hle)
        self.assertEqual(squares(hle, pm.COLOURS["implemented"]), 2 + 1)
        self.assertEqual(squares(hle, pm.COLOURS["stubbed"]), 2 + 1)
        self.assertEqual(squares(hle, pm.COLOURS["forwarded"]), 0)
        self.assertIn(">2/4<", hle)
        self.assertIn(">0/1<", hle)

    def test_title_map_and_summary(self):
        self.write_imports()
        texts = self.generate()
        apis = texts[f"docs/progress/{TITLE}.apis.svg"]
        self.assertEqual(squares(apis, pm.COLOURS["implemented"]), 1 + 1)
        self.assertEqual(squares(apis, pm.COLOURS["forwarded"]), 1 + 1)
        self.assertEqual(squares(apis, pm.COLOURS["stubbed"]), 1 + 1)
        self.assertEqual(squares(apis, pm.COLOURS["missing"]), 2 + 1)
        readme = texts["README.md"]
        self.assertIn("1/5 functions implemented, 1 forwarded, 1 stubbed, 2 missing; shaders: no data yet.", readme)
        self.assertIn(pm.NO_SHADER_DATA, texts[f"docs/progress/{TITLE}.shaders.svg"])

    def test_shader_map(self):
        self.write_imports()
        self.write_json(f"{TITLE}.shaders.json", {"stages": {
            "ps": {"total": 10, "translated": 4, "decoded": 3, "failed": 1},
            "vs": {"total": 5, "translated": 5},
            "hs": {"total": 2},
        }})
        texts = self.generate()
        svg = texts[f"docs/progress/{TITLE}.shaders.svg"]
        self.assertLess(svg.index(">vs<"), svg.index(">ps<"))
        self.assertLess(svg.index(">ps<"), svg.index(">hs<"))
        self.assertIn("1 square = 1 shader", svg)
        self.assertIn("shaders: 9/17 translated, 3 decoded.", texts["README.md"])

    def test_large_shader_counts_are_scaled(self):
        rows, scale = pm.shader_rows({"stages": {"cs": {"total": 1000, "translated": 500}}})
        self.assertEqual(scale, 9)
        self.assertEqual(len(rows[0]["items"]), 112)
        self.assertEqual(rows[0]["items"].count("translated"), 56)
        self.assertEqual(pm.scale_note(scale), "1 square = 9 shaders")

    def test_stage_counts_are_clamped(self):
        self.assertEqual(pm.stage_counts({"total": 4, "translated": 9, "decoded": 2}), (4, 4, 0, 0))
        self.assertEqual(pm.stage_counts({"total": -3, "translated": -1}), (0, 0, 0, 0))
        self.assertEqual(pm.stage_counts({"total": 5, "translated": 1, "decoded": 1, "failed": 9}), (5, 1, 1, 3))

    def test_long_rows_wrap(self):
        svg = pm.render_map([{"label": "x", "count": "0/130", "items": ["missing"] * 130}], pm.FUNCTION_LEGEND, "t")
        tops = {int(y) for y in re.findall(r'<rect x="\d+" y="(\d+)" width="8" height="8" fill="#57606a"', svg)}
        self.assertEqual(len(tops), 3 + 1)

    def test_labels_are_escaped(self):
        svg = pm.render_map([{"label": "a&b<c>", "count": "0/0", "items": []}], pm.HLE_LEGEND, "t & u")
        self.assertIn("a&amp;b&lt;c&gt;", svg)
        self.assertIn("<title>t &amp; u</title>", svg)

    def test_readme_section_is_replaced_in_place(self):
        first = pm.render_readme(README, [])
        self.assertLess(first.index(pm.README_BEGIN), first.index("## Build"))
        self.assertEqual(first.count(pm.README_BEGIN), 1)
        second = pm.render_readme(first.replace("\n", "\r\n"), [(TITLE, "summary")])
        self.assertEqual(second.count(pm.README_BEGIN), 1)
        self.assertIn(f"### {TITLE}", second)
        self.assertIn("summary", second)
        self.assertNotIn("\r\n", second)
        self.assertTrue(second.endswith("## Build\n\nSteps.\n"))

    def test_readme_without_anchor_is_refused(self):
        with self.assertRaises(SystemExit):
            pm.render_readme("# Project\n", [])

    def test_stale_files(self):
        texts = pm.generate(self.root)
        self.assertEqual(sorted(p.name for p in pm.stale_files(self.root, texts)), ["README.md", "hle.svg"])
        for path, text in texts.items():
            pm.write_text(path, text)
        self.assertEqual(pm.stale_files(self.root, texts), [])
        (self.root / pm.PROGRESS / "hle.svg").write_bytes(texts[self.root / pm.PROGRESS / "hle.svg"].replace("\n", "\r\n").encode())
        self.assertEqual(pm.stale_files(self.root, texts), [])
        (self.root / pm.PROGRESS / "OLD00000.apis.svg").write_text("<svg/>", encoding="utf-8")
        self.assertEqual([p.name for p in pm.stale_files(self.root, texts)], ["OLD00000.apis.svg"])

    def run_main(self, *argv):
        out = io.StringIO()
        with mock.patch.object(pm, "ROOT", self.root), contextlib.redirect_stdout(out):
            code = pm.main(list(argv))
        return code, out.getvalue()

    def test_main_writes_then_checks_clean(self):
        self.write_imports()
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("stale: README.md", out)
        self.assertIn("run: python tools/progress_map.py", out)
        self.assertEqual(self.run_main(), (0, "wrote 4 file(s)\n"))
        self.assertEqual(self.run_main("--check"), (0, ""))
        self.write_def("beta", "beta.dll", ["E=g6shim_E"])
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("stale: docs/progress/hle.svg", out)


if __name__ == "__main__":
    unittest.main()
