#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_conventions as cc

SPDX_CPP = "// SPDX-FileCopyrightText: 2026 someone\n// SPDX-License-Identifier: GPL-3.0-or-later\n"
SPDX_HASH = "# SPDX-FileCopyrightText: 2026 someone\n# SPDX-License-Identifier: GPL-3.0-or-later\n"
SIGN_OFF = "Signed-off-by: A Person <a@example.com>"
GIT_ENV = {
    "GIT_AUTHOR_NAME": "A Person",
    "GIT_AUTHOR_EMAIL": "a@example.com",
    "GIT_COMMITTER_NAME": "A Person",
    "GIT_COMMITTER_EMAIL": "a@example.com",
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_CONFIG_GLOBAL": os.devnull,
}


def rules(problems):
    return [p.rule for p in problems]


def denylist_with(*phrases):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "titles.txt"
        path.write_text("\n".join(phrases) + "\n", encoding="utf-8")
        with mock.patch.object(cc, "DENYLIST", path):
            return cc.load_denylist()


class MessageTests(unittest.TestCase):
    def test_valid_message_passes(self):
        self.assertEqual(cc.check_message("m", f"feat(core): read the header\n\nBody.\n\n{SIGN_OFF}\n", []), [])

    def test_comment_lines_and_leading_blanks_are_ignored(self):
        message = f"# a comment git adds\n\nfix: keep it\n\n{SIGN_OFF}\n# trailing comment\n"
        self.assertEqual(cc.check_message("m", message, []), [])

    def test_subject_must_be_conventional(self):
        self.assertEqual(rules(cc.check_message("m", f"Add a thing\n\n{SIGN_OFF}", [])), ["commit-subject"])
        self.assertEqual(rules(cc.check_message("m", f"feat(Core): x\n\n{SIGN_OFF}", [])), ["commit-subject"])
        self.assertEqual(rules(cc.check_message("m", f"feat(core):x\n\n{SIGN_OFF}", [])), ["commit-subject"])
        self.assertEqual(cc.check_message("m", f"feat(core)!: break it\n\n{SIGN_OFF}", []), [])

    def test_subject_length_limit(self):
        at_limit = "feat: " + "a" * (cc.MAX_SUBJECT - len("feat: "))
        self.assertEqual(cc.check_message("m", f"{at_limit}\n\n{SIGN_OFF}", []), [])
        problems = cc.check_message("m", f"{at_limit}a\n\n{SIGN_OFF}", [])
        self.assertEqual(rules(problems), ["commit-subject"])
        self.assertIn("longer", problems[0].message)

    def test_platform_names_are_refused_in_subjects_only(self):
        self.assertEqual(rules(cc.check_message("m", f"feat(core): support Xbox paths\n\n{SIGN_OFF}", [])),
                         ["commit-subject"])
        self.assertEqual(cc.check_message("m", f"feat(core): support boxes\n\nxbox in the body\n{SIGN_OFF}", []), [])
        self.assertEqual(cc.check_message("m", f"feat(core): handle xboxone2 ids\n\n{SIGN_OFF}", []), [])

    def test_sign_off_is_required(self):
        self.assertEqual(rules(cc.check_message("m", "fix: x\n", [])), ["sign-off"])
        self.assertEqual(rules(cc.check_message("m", "fix: x\n\nSigned-off-by: nobody\n", [])), ["sign-off"])

    def test_denylisted_title_in_body(self):
        denylist = denylist_with("Example Quest")
        problems = cc.check_message("m", f"fix: x\n\nworks in example_quest now\n{SIGN_OFF}", denylist)
        self.assertEqual(rules(problems), ["title"])

    def test_empty_message(self):
        self.assertEqual(sorted(rules(cc.check_message("m", "", []))), ["commit-subject", "sign-off"])


class DenylistTests(unittest.TestCase):
    def test_missing_file_gives_no_patterns(self):
        with mock.patch.object(cc, "DENYLIST", Path(tempfile.gettempdir()) / "no-such-dir" / "titles.txt"):
            self.assertEqual(cc.load_denylist(), [])

    def test_blank_and_comment_lines_are_skipped(self):
        patterns = denylist_with("", "# not a title", "Example Quest", "   ")
        self.assertEqual([phrase for phrase, _ in patterns], ["Example Quest"])

    def test_separators_and_case_do_not_hide_a_title(self):
        (_, pattern), = denylist_with("Example Quest: Return")
        for text in ("example quest return", "EXAMPLE_QUEST-RETURN", "Example.Quest;Return", "ExampleQuestReturn"):
            self.assertTrue(pattern.search(text), text)

    def test_titles_inside_longer_words_are_not_matched(self):
        (_, pattern), = denylist_with("Quest")
        self.assertIsNone(pattern.search("Questions"))
        self.assertIsNone(pattern.search("SideQuest"))
        self.assertIsNotNone(pattern.search("a quest begins"))


class FileKindTests(unittest.TestCase):
    def test_kinds(self):
        cases = {
            "src/a.cpp": "cpp", "src/a.HPP": "cpp", "src/a.inl": "cpp", "CMakeLists.txt": "cmake",
            "cmake/x.cmake": "cmake", "tools/x.py": "python", "hle/x/x.def": "def", "run.sh": "shell",
            "tools/hooks/pre-commit": "shell", "docs/a.md": None, "hle/a.hlsl": None,
        }
        for path, kind in cases.items():
            self.assertEqual(cc.file_kind(path), kind, path)


class CppTests(unittest.TestCase):
    def check(self, body):
        return cc.check_file("a.cpp", (SPDX_CPP + body).encode("utf-8"), [])

    def test_clean_file(self):
        self.assertEqual(self.check("int x = 1;\n"), [])

    def test_line_and_block_comments_are_refused(self):
        problems = self.check("int x; // why\n/* block\n spans */ int y;\n")
        self.assertEqual(rules(problems), ["comment", "comment"])
        self.assertEqual([p.where for p in problems], ["a.cpp:3", "a.cpp:4"])

    def test_endif_and_namespace_markers_are_allowed(self):
        body = "#ifdef A\n#endif  // A\nnamespace a {\n}  // namespace a\n}; // namespace b\n"
        self.assertEqual(self.check(body), [])

    def test_comment_after_a_brace_that_is_not_a_namespace_end_is_refused(self):
        self.assertEqual(rules(self.check("void f() {\n}  // end of f\n")), ["comment"])

    def test_markers_in_strings_are_not_comments(self):
        body = 'const char* a = "http://x"; const char* b = "/* no */";\nchar c = \'/\'; int d = 1\'000;\n'
        self.assertEqual(self.check(body), [])

    def test_raw_strings_are_skipped(self):
        body = 'auto a = R"x(// not a comment\n/* nor this */)x";\nauto b = u8R"(//)";\n'
        self.assertEqual(self.check(body), [])

    def test_spdx_only_counts_in_the_first_two_lines(self):
        body = "int x;\n// SPDX-License-Identifier: GPL-3.0-or-later\n"
        self.assertEqual(rules(self.check(body)), ["comment"])

    def test_missing_spdx(self):
        problems = cc.check_file("a.cpp", b"int x;\n", [])
        self.assertEqual(rules(problems), ["spdx"])
        problems = cc.check_file("a.cpp", b"// SPDX-License-Identifier: MIT\nint x;\n", [])
        self.assertEqual(rules(problems), ["spdx"])

    def test_unterminated_block_comment_does_not_hang(self):
        self.assertEqual(rules(self.check("/* open\n")), ["comment"])


class OtherKindTests(unittest.TestCase):
    def test_cmake(self):
        clean = SPDX_HASH + 'set(a "#not a comment")\nset(b [[# nor this]])\n'
        self.assertEqual(cc.check_file("CMakeLists.txt", clean.encode(), []), [])
        dirty = SPDX_HASH + "set(a 1) # why\n#[[ bracket\ncomment ]]\n"
        self.assertEqual(rules(cc.check_file("x.cmake", dirty.encode(), [])), ["comment", "comment"])

    def test_python(self):
        clean = "#!/usr/bin/env python3\n" + SPDX_HASH + 'x = "# not a comment"\n'
        self.assertEqual(cc.check_file("t.py", clean.encode(), []), [])
        dirty = SPDX_HASH + "x = 1  # why\n"
        self.assertEqual(rules(cc.check_file("t.py", dirty.encode(), [])), ["comment"])

    def test_python_syntax_error_is_reported(self):
        problems = cc.check_file("t.py", (SPDX_HASH + 'x = """unterminated\n').encode(), [])
        self.assertEqual(rules(problems), ["syntax"])

    def test_shell(self):
        clean = "#!/bin/sh\n" + SPDX_HASH + 'exec tool "$@"\n'
        self.assertEqual(cc.check_file("tools/hooks/pre-push", clean.encode(), []), [])
        dirty = "#!/bin/sh\n" + SPDX_HASH + "# step one\nexec tool # inline\n"
        self.assertEqual(rules(cc.check_file("tools/hooks/pre-push", dirty.encode(), [])), ["comment", "comment"])

    def test_def(self):
        self.assertEqual(cc.check_file("x.def", b"EXPORTS\n    A=B\n", []), [])
        self.assertEqual(rules(cc.check_file("x.def", b"EXPORTS\n    A ; why\n", [])), ["comment"])

    def test_other_files_need_no_spdx(self):
        self.assertEqual(cc.check_file("docs/a.md", b"# Heading\n", []), [])

    def test_binary_and_non_utf8(self):
        self.assertEqual(cc.check_file("a.cpp", b"\0\1\2", []), [])
        self.assertEqual(rules(cc.check_file("a.cpp", b"\xff\xfe", [])), ["encoding"])

    def test_denylisted_title_in_path_and_text(self):
        denylist = denylist_with("Example Quest")
        problems = cc.check_file("docs/example-quest.md", b"one\nabout Example Quest\n", denylist)
        self.assertEqual(rules(problems), ["title", "title"])
        self.assertEqual(problems[1].where, "docs/example-quest.md:2")

    def test_binary_file_still_has_its_path_checked(self):
        denylist = denylist_with("Example Quest")
        self.assertEqual(rules(cc.check_file("example_quest.bin", b"\0", denylist)), ["title"])


def git(repo, *args):
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, env={**os.environ, **GIT_ENV})


class RepositoryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.repo = Path(self.directory.name)
        git(self.repo, "init", "-q")
        git(self.repo, "config", "core.autocrlf", "false")
        self.patches = [
            mock.patch.object(cc, "ROOT", self.repo),
            mock.patch.object(cc, "DENYLIST", self.repo / ".local" / "titles.txt"),
            mock.patch.dict(os.environ, GIT_ENV),
        ]
        for patch in self.patches:
            patch.start()

    def tearDown(self):
        for patch in reversed(self.patches):
            patch.stop()
        self.directory.cleanup()

    def write(self, name, text):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))

    def commit(self, message):
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", message)

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cc.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_staged_checks_only_staged_content(self):
        self.write("a.cpp", SPDX_CPP + "int x; // why\n")
        git(self.repo, "add", "a.cpp")
        self.write("a.cpp", SPDX_CPP + "int x;\n")
        code, out, err = self.run_main("--staged")
        self.assertEqual(code, 1)
        self.assertIn("a.cpp:3: comment", out)
        self.assertIn("title check skipped", err)

    def test_staged_deletions_are_not_checked(self):
        self.write("a.cpp", "int x; // why\n")
        self.commit(f"fix: add\n\n{SIGN_OFF}")
        git(self.repo, "rm", "-q", "a.cpp")
        self.assertEqual(self.run_main("--staged")[0], 0)

    def test_all_checks_tracked_files_only(self):
        self.write("a.cpp", SPDX_CPP + "int x;\n")
        self.commit(f"fix: add\n\n{SIGN_OFF}")
        self.write("untracked.cpp", "int y; // why\n")
        self.assertEqual(self.run_main("--all")[:2], (0, ""))
        self.write("a.cpp", "int x;\n")
        code, out, _ = self.run_main("--all")
        self.assertEqual(code, 1)
        self.assertIn("a.cpp:1: spdx", out)

    def test_range_checks_messages_and_files_of_each_commit(self):
        self.write("a.cpp", SPDX_CPP + "int x;\n")
        self.commit(f"feat(core): first\n\n{SIGN_OFF}")
        self.write("b.cpp", SPDX_CPP + "int y; // why\n")
        self.commit("second without sign-off")
        code, out, _ = self.run_main("--range", "HEAD")
        self.assertEqual(code, 1)
        self.assertIn("commit-subject", out)
        self.assertIn("sign-off", out)
        self.assertIn("b.cpp:3: comment", out)
        self.assertEqual(self.run_main("--range", "HEAD~1..HEAD~1")[0], 0)

    def test_range_uses_the_denylist(self):
        self.write(".local/titles.txt", "Example Quest\n")
        self.write("a.md", "plain\n")
        self.commit(f"docs: mention Example Quest\n\n{SIGN_OFF}")
        code, out, err = self.run_main("--range", "HEAD")
        self.assertEqual(code, 1)
        self.assertIn("title", out)
        self.assertNotIn("title check skipped", err)

    def test_commit_msg_file(self):
        good = self.repo / "good.txt"
        good.write_text(f"docs: explain\n\n{SIGN_OFF}\n", encoding="utf-8")
        bad = self.repo / "bad.txt"
        bad.write_text("explain\n", encoding="utf-8")
        self.assertEqual(self.run_main("--commit-msg", str(good))[0], 0)
        code, out, _ = self.run_main("--commit-msg", str(bad))
        self.assertEqual(code, 1)
        self.assertIn("commit-subject", out)

    def test_git_failure_exits_with_two(self):
        with contextlib.redirect_stderr(io.StringIO()) as err:
            with self.assertRaises(SystemExit) as raised:
                cc.main(["--range", "no-such-ref"])
        self.assertEqual(raised.exception.code, 2)
        self.assertTrue(err.getvalue())

    def test_a_mode_is_required(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                cc.main([])
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
