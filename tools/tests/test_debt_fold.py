#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import io
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import debt_fold as df

LEDGER = """\
# kernelx

## Build

None.

## Silent stubs

- `SetThreadName` returns 0 without naming the thread.
- `GetThing` reports success.
  Closes when: a title calls it.

## Unknown function info

None.

## Functional

- `XMemAlloc` ignores its attribute bits.
  Title IDs: 3DD7389B
"""


def text(block):
    return textwrap.dedent(block).lstrip("\n")


class LedgerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / df.CHANGES).mkdir(parents=True)
        self.write_ledger("kernelx", LEDGER)

    def tearDown(self):
        self.directory.cleanup()

    def write_ledger(self, module, body):
        (self.root / df.LEDGER / f"{module}.md").write_text(body, encoding="utf-8", newline="\n")

    def fragment(self, name, body):
        path = self.root / df.CHANGES / name
        path.write_text(text(body), encoding="utf-8", newline="\n")
        return path

    def ledger_text(self, module="kernelx"):
        return (self.root / df.LEDGER / f"{module}.md").read_text(encoding="utf-8")

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = df.main([*argv, "--root", str(self.root)])
        return code, out.getvalue(), err.getvalue()

    def assert_fold_error(self, body, message):
        self.fragment("bad.md", body)
        with self.assertRaises(df.FoldError) as raised:
            df.fold(self.root, write=False)
        self.assertIn(message, str(raised.exception))

    def test_unchanged_ledger_renders_identically(self):
        self.assertEqual(df.load_ledger(self.root, "kernelx").render(), LEDGER)

    def test_add_change_remove(self):
        self.fragment("branch.md", """
            # kernelx

            ## add Unknown function info

            - `Foo` takes a flag.
            - `Bar` returns a count.
              Closes when: measured.

            ## change `XMemAlloc` ignores

            - `XMemAlloc` honours the alignment bits.

            ## remove `SetThreadName`
            """)
        texts, fragments = df.fold(self.root, write=False)
        result = texts["kernelx"]
        self.assertIn("## Unknown function info\n\n- `Foo` takes a flag.\n- `Bar` returns a count.\n  Closes when: measured.\n",
                      result)
        self.assertIn("- `XMemAlloc` honours the alignment bits.\n", result)
        self.assertNotIn("XMemAlloc` ignores", result)
        self.assertNotIn("SetThreadName", result)
        self.assertIn("## Build\n\nNone.\n", result)
        self.assertEqual(len(fragments), 1)
        self.assertEqual(self.ledger_text(), LEDGER)

    def test_removing_the_last_entry_renders_none(self):
        self.fragment("a.md", """
            # kernelx

            ## remove `XMemAlloc`
            """)
        self.assertIn("## Functional\n\nNone.\n", df.fold(self.root, write=False)[0]["kernelx"])

    def test_fragments_apply_in_name_order(self):
        self.fragment("b.md", "# kernelx\n\n## add Build\n\n- second\n")
        self.fragment("a.md", "# kernelx\n\n## add Build\n\n- first\n")
        self.assertIn("## Build\n\n- first\n- second\n", df.fold(self.root, write=False)[0]["kernelx"])

    def test_one_fragment_may_touch_several_modules(self):
        self.write_ledger("xmem", "# xmem\n\n## Build\n\nNone.\n")
        self.fragment("a.md", "# kernelx\n\n## remove `GetThing`\n\n# xmem\n\n## add Functional\n\n- slow\n")
        texts, _ = df.fold(self.root, write=False)
        self.assertEqual(sorted(texts), ["kernelx", "xmem"])
        self.assertIn("## Functional\n\n- slow\n", texts["xmem"])

    def test_write_folds_and_deletes_fragments(self):
        path = self.fragment("branch.md", "# kernelx\n\n## add Build\n\n- needs a tool\n")
        code, out, _ = self.run_main()
        self.assertEqual(code, 0)
        self.assertEqual(out, "folded 1 fragment(s) into 1 ledger file(s)\n")
        self.assertFalse(path.exists())
        self.assertIn("## Build\n\n- needs a tool\n", self.ledger_text())
        self.assertNotIn("\r\n", self.ledger_text())

    def test_check_leaves_files_alone(self):
        path = self.fragment("branch.md", "# kernelx\n\n## add Build\n\n- needs a tool\n")
        self.assertEqual(self.run_main("--check"), (0, "", ""))
        self.assertTrue(path.exists())
        self.assertEqual(self.ledger_text(), LEDGER)

    def test_show_prints_the_folded_ledger(self):
        self.fragment("branch.md", "# kernelx\n\n## add Build\n\n- needs a tool\n")
        code, out, _ = self.run_main("--show", "kernelx")
        self.assertEqual(code, 0)
        self.assertIn("- needs a tool", out)
        self.write_ledger("xmem", "# xmem\n")
        code, out, _ = self.run_main("--show", "xmem")
        self.assertEqual((code, out.count("None.")), (0, 4))

    def test_show_unknown_module(self):
        code, _, err = self.run_main("--show", "nothing")
        self.assertEqual(code, 1)
        self.assertIn("no ledger file for module nothing", err)

    def test_check_reports_a_bad_fragment(self):
        self.fragment("bad.md", "# kernelx\n\n## remove nothing matches this\n")
        code, _, err = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("matches 0 entries", err)

    def test_check_reports_a_bad_ledger(self):
        self.write_ledger("xmem", "# xmem\n\n## Wishes\n\n- a pony\n")
        code, _, err = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("unknown section 'Wishes'", err)

    def test_check_skips_the_readme(self):
        (self.root / df.LEDGER / "README.md").write_text("# Ledger\n\n## Fragments\n\ntext\n", encoding="utf-8")
        self.assertEqual(self.run_main("--check")[0], 0)

    def test_fragment_errors(self):
        cases = [
            ("# nothing\n\n## add Build\n\n- x\n", "no ledger file for module nothing"),
            ("# ../kernelx\n\n## add Build\n\n- x\n", "invalid module name"),
            ("## add Build\n\n- x\n", "operation before a '# <module>' line"),
            ("# kernelx\n\nloose text\n", "text outside an operation"),
            ("# kernelx\n\n### add Build\n", "expected '# <module>'"),
            ("# kernelx\n\n## add Wishes\n\n- x\n", "unknown section 'Wishes'"),
            ("# kernelx\n\n## add Build\n", "'add' takes one or more entries"),
            ("# kernelx\n\n## change `XMemAlloc`\n", "'change' takes one entry"),
            ("# kernelx\n\n## change `XMemAlloc`\n\n- a\n- b\n", "'change' takes one entry"),
            ("# kernelx\n\n## remove `XMemAlloc`\n\n- a\n", "'remove' takes no entry"),
            ("# kernelx\n", "no operations"),
            ("# kernelx\n\n## add Build\n\nnot an entry\n", "expected an entry"),
            ("# kernelx\n\n## add Build\n\n- x\n  Title IDs: 3dd7389b\n", "not an 8-digit hex title ID"),
            ("# kernelx\n\n## remove `\n", "matches 3 entries"),
        ]
        for body, message in cases:
            with self.subTest(body=body):
                self.assert_fold_error(body, message)

    def test_title_id_lists(self):
        self.fragment("a.md", "# kernelx\n\n## add Build\n\n- x\n  Title IDs: 3DD7389B, 7952987A\n")
        self.assertIn("Title IDs: 3DD7389B, 7952987A", df.fold(self.root, write=False)[0]["kernelx"])

    def test_two_fragments_may_not_touch_one_entry(self):
        self.fragment("a.md", "# kernelx\n\n## remove `XMemAlloc`\n")
        self.fragment("b.md", "# kernelx\n\n## change `XMemAlloc`\n\n- replaced\n")
        with self.assertRaises(df.FoldError) as raised:
            df.fold(self.root, write=False)
        self.assertIn("change the same entry", str(raised.exception))

    def test_continuation_after_a_blank_line_is_refused(self):
        self.write_ledger("xmem", "# xmem\n\n## Build\n\n- one\n\n  orphan\n")
        with self.assertRaises(df.FoldError):
            df.check_ledgers(self.root)


if __name__ == "__main__":
    unittest.main()
