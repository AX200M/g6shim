#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
import argparse
import io
import re
import subprocess
import sys
import tokenize
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
DENYLIST = ROOT / ".local" / "titles.txt"

TYPES = ("feat", "fix", "docs", "style", "refactor", "perf", "test", "build", "ci", "chore", "revert")
SUBJECT = re.compile(r"^(?:%s)(?:\([a-z0-9][a-z0-9_.-]*\))?!?: [^\s].*$" % "|".join(TYPES))
MAX_SUBJECT = 72
SIGN_OFF = re.compile(r"^Signed-off-by: \S.* <[^<>\s]+@[^<>\s]+>$", re.M)
PLATFORM_NAMES = re.compile(r"(?i)(?<![a-z0-9])(?:xbox|kinect)(?![a-z0-9])")
SPDX = re.compile(r"SPDX-(?:License-Identifier|FileCopyrightText): \S")
LICENSE_LINE = re.compile(r"SPDX-License-Identifier: \S")
COPYRIGHT_LINE = re.compile(r"SPDX-FileCopyrightText: \S")
HEADER_LINES = 3

CPP = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}
ALLOWED_CPP_CODE = re.compile(r"^\s*(?:#\s*endif\b.*|}\s*;?\s*)$")
NAMESPACE_END = re.compile(r"^\s*namespace\b")


@dataclass
class Problem:
    where: str
    rule: str
    message: str

    def __str__(self):
        return f"{self.where}: {self.rule}: {self.message}"


def git(*args):
    result = subprocess.run(["git", "-c", "core.quotePath=false", *args], cwd=ROOT, capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode("utf-8", "replace"))
        raise SystemExit(2)
    return result.stdout


def load_denylist():
    if not DENYLIST.is_file():
        return []
    patterns = []
    for line in DENYLIST.read_text(encoding="utf-8").splitlines():
        phrase = line.strip()
        if not phrase or phrase.startswith("#"):
            continue
        words = [re.escape(w) for w in re.split(r"[\s_\-.:;]+", phrase) if w]
        if words:
            body = r"[\s_\-.:;]*".join(words)
            patterns.append((phrase, re.compile(r"(?<![A-Za-z0-9])" + body + r"(?![A-Za-z0-9])", re.I)))
    return patterns


def file_kind(path):
    p = PurePosixPath(path)
    if p.suffix.lower() in CPP:
        return "cpp"
    if p.name == "CMakeLists.txt" or p.suffix.lower() == ".cmake":
        return "cmake"
    if p.suffix.lower() == ".py":
        return "python"
    if p.suffix.lower() == ".def":
        return "def"
    if p.suffix.lower() == ".sh" or str(p.parent) == "tools/hooks":
        return "shell"
    return None


def cpp_comments(text):
    i, n, line = 0, len(text), 1
    comments = []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            comments.append((line, text[i:end], text.rfind("\n", 0, i) + 1))
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            comments.append((line, text[i:end], text.rfind("\n", 0, i) + 1))
            line += text.count("\n", i, end)
            i = end
        else:
            raw = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\v\f\n]{0,16})\(', text[i:i + 24])
            if raw and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
                close = text.find(")" + raw.group(1) + '"', i + raw.end())
                end = n if close < 0 else close + len(raw.group(1)) + 2
                line += text.count("\n", i, end)
                i = end
            elif c in "\"'" and not (c == "'" and i > 0 and text[i - 1].isdigit()):
                j = i + 1
                while j < n and text[j] != c and text[j] != "\n":
                    j += 2 if text[j] == "\\" else 1
                i = j + 1
            else:
                i += 1
    return comments


def check_cpp(path, text):
    problems = []
    for line, comment, line_start in cpp_comments(text):
        code = text[line_start:text.find(comment, line_start)]
        body = comment[2:].strip() if comment.startswith("//") else comment
        if line <= 2 and comment.startswith("//") and SPDX.match(body):
            continue
        if comment.startswith("//") and ALLOWED_CPP_CODE.match(code):
            if code.strip().startswith("#") or NAMESPACE_END.match(body):
                continue
        problems.append(Problem(f"{path}:{line}", "comment", "no comments except the SPDX lines, #endif and namespace ends"))
    return problems


def cmake_comments(text):
    token = re.compile(r'\[(=*)\[.*?\]\1\]|"(?:\\.|[^"\\])*"|(?P<comment>#\[(?P<level>=*)\[.*?\](?P=level)\]|#[^\n]*)', re.S)
    return [(text.count("\n", 0, m.start()) + 1, m.group("comment")) for m in token.finditer(text) if m.group("comment")]


def check_cmake(path, text):
    problems = []
    for line, comment in cmake_comments(text):
        if line <= 2 and SPDX.match(comment[1:].strip()):
            continue
        problems.append(Problem(f"{path}:{line}", "comment", "no comments except the SPDX lines"))
    return problems


def check_python(path, text):
    problems = []
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, SyntaxError) as error:
        return [Problem(path, "syntax", str(error))]
    for tok in tokens:
        if tok.type != tokenize.COMMENT:
            continue
        line = tok.start[0]
        if line == 1 and tok.string.startswith("#!"):
            continue
        if line <= HEADER_LINES and SPDX.match(tok.string[1:].strip()):
            continue
        problems.append(Problem(f"{path}:{line}", "comment", "no comments except the shebang and SPDX lines"))
    return problems


def check_shell(path, text):
    problems = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if number == 1 and stripped.startswith("#!"):
            continue
        if number <= HEADER_LINES and stripped.startswith("#") and SPDX.match(stripped[1:].strip()):
            continue
        if stripped.startswith("#") or re.search(r"\s#", line):
            problems.append(Problem(f"{path}:{number}", "comment", "no comments except the shebang and SPDX lines"))
    return problems


def check_def(path, text):
    return [Problem(f"{path}:{n}", "comment", "no comments in .def files")
            for n, line in enumerate(text.splitlines(), 1) if ";" in line]


def has_spdx(kind, text):
    marker = "//" if kind == "cpp" else "#"
    lines = [l[len(marker):].strip() for l in text.splitlines()[:HEADER_LINES] if l.startswith(marker)]
    return any(LICENSE_LINE.match(l) for l in lines) and any(COPYRIGHT_LINE.match(l) for l in lines)


def check_file(path, data, denylist):
    problems = []
    for phrase, pattern in denylist:
        if pattern.search(path):
            problems.append(Problem(path, "title", f"path names a title from the denylist ({phrase})"))
    if b"\0" in data:
        return problems
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return problems + [Problem(path, "encoding", "not UTF-8")]
    for phrase, pattern in denylist:
        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            problems.append(Problem(f"{path}:{line}", "title", f"names a title from the denylist ({phrase})"))
    kind = file_kind(path)
    checks = {"cpp": check_cpp, "cmake": check_cmake, "python": check_python, "shell": check_shell, "def": check_def}
    if kind in checks:
        problems += checks[kind](path, text)
    if kind in ("cpp", "cmake", "python", "shell") and not has_spdx(kind, text):
        problems.append(Problem(f"{path}:1", "spdx", "source files start with SPDX-FileCopyrightText and SPDX-License-Identifier lines"))
    return problems


def message_lines(message):
    return [l for l in message.replace("\r\n", "\n").split("\n") if not l.startswith("#")]


def check_message(where, message, denylist):
    problems = []
    lines = message_lines(message)
    while lines and not lines[0].strip():
        lines.pop(0)
    body = "\n".join(lines)
    subject = lines[0] if lines else ""
    if not SUBJECT.match(subject):
        problems.append(Problem(where, "commit-subject", f"not a Conventional Commit subject: {subject!r}"))
    if len(subject) > MAX_SUBJECT:
        problems.append(Problem(where, "commit-subject", f"subject longer than {MAX_SUBJECT} characters"))
    if PLATFORM_NAMES.search(subject):
        problems.append(Problem(where, "commit-subject", "no platform names in commit subjects"))
    if not SIGN_OFF.search(body):
        problems.append(Problem(where, "sign-off", "commits carry a DCO sign-off (git commit -s)"))
    for phrase, pattern in denylist:
        if pattern.search(body):
            problems.append(Problem(where, "title", f"message names a title from the denylist ({phrase})"))
    return problems


def staged(denylist):
    names = git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").decode("utf-8").split("\0")
    problems = []
    for name in filter(None, names):
        problems += check_file(name, git("show", f":{name}"), denylist)
    return problems


def tracked(denylist):
    names = git("ls-files", "-z").decode("utf-8").split("\0")
    problems = []
    for name in filter(None, names):
        path = ROOT / name
        if path.is_file():
            problems += check_file(name, path.read_bytes(), denylist)
    return problems


def commits(spec):
    return git("rev-list", "--reverse", spec).decode("utf-8").split()


def history(spec, denylist):
    problems = []
    for commit in commits(spec):
        short = commit[:10]
        problems += check_message(f"commit {short}", git("log", "-1", "--format=%B", commit).decode("utf-8"), denylist)
        names = git("diff-tree", "--root", "--no-commit-id", "--name-only", "-r", "-z", "--diff-filter=ACMR", commit)
        for name in filter(None, names.decode("utf-8").split("\0")):
            for p in check_file(name, git("show", f"{commit}:{name}"), denylist):
                p.where = f"{short} {p.where}"
                problems.append(p)
    return problems


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description="check g6shim's conventions")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--staged", action="store_true", help="check the staged files")
    mode.add_argument("--commit-msg", metavar="FILE", help="check a commit message file")
    mode.add_argument("--range", metavar="REVS", help="check every commit in REVS (a..b, or a ref for its whole history)")
    mode.add_argument("--all", action="store_true", help="check every tracked file")
    args = parser.parse_args(argv)

    denylist = load_denylist()
    if args.staged:
        problems = staged(denylist)
    elif args.commit_msg:
        problems = check_message(args.commit_msg, Path(args.commit_msg).read_text(encoding="utf-8"), denylist)
    elif args.range:
        problems = history(args.range, denylist)
    else:
        problems = tracked(denylist)
    for problem in problems:
        print(problem)
    if not denylist:
        print("note: no .local/titles.txt, title check skipped", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
