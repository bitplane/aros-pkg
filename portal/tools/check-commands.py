#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Every pkg command the portal and the repository's documentation show, judged
by pkg itself.

The portal's pages and installers carry pkg commands of their own, and the
guides carry hundreds more. When the parser changes they must change with it.
This script builds the list from the sources, and asks pkg to judge each
command with PKG_CHECK_WORDS=1, which reads the words exactly as a real run
would and runs nothing: exit 0, or the usage refusal (20) a real run would
give. pkg is the only reference for its own grammar; nothing here keeps a copy.

    python3 portal/tools/check-commands.py [--pkg build/pkg] [--list]

--list prints the whole review list, every command with its file and line.
Without it, only the commands pkg refuses are printed, and the exit status is
1 when there is one.

Only what a person copies is read: code spans and code blocks in Markdown,
<code> and data-copy in the pages, whole lines in the installers. A value the
page fills in (@url, {site}, <drawer>, $name, @@CHANNEL@@) is replaced by a
plain word before pkg judges it.
"""

import argparse
import html
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# A command is a snippet that starts with pkg, after the prompt a transcript
# shows ("$ ", "1.SYS:> "): the program by its name, or by its path (C:Pkg,
# RAM:Pkg-bootstrap), then a verb in capitals that is not an AmigaDOS volume
# (DEPOT:). pkg named in the middle of a line is prose or pkg's own output.
COMMAND = re.compile(r"^(?:\$\s+|\d+\.[\w:]+>\s*)?(?:\w+:(?:[\w/]+/)?)?(?:Pkg|pkg)(?:-bootstrap)?\s+([A-Z][A-Z0-9]+(?![:\w]).*)$")

# A synopsis names the words a verb takes, "[CHANNEL <dir>]", "<name>...", and
# is read by people, not run: pkg HELP MACHINE is where the syntax is checked.
SYNOPSIS = re.compile(r"\[[^\]]*\]|\.\.\.")

# Values a page or a template fills in, which pkg should see as one plain word.
PLACEHOLDER = re.compile(r"@@\w+@@|@\([^)]*\)|@[\w.]+(?:\([^)]*\))?|\{[^}]*\}|<[^>]*>|\$\w+|\$\{[^}]*\}")


def sources():
    """(path, kind) for every file whose commands a person copies."""
    for d, _, names in os.walk(os.path.join(ROOT, "portal", "src", "Portal", "Pages")):
        for n in sorted(names):
            if n.endswith(".cshtml"):
                yield os.path.join(d, n), "page"
    install = os.path.join(ROOT, "portal", "src", "Portal", "Install")
    for n in sorted(os.listdir(install)) if os.path.isdir(install) else []:
        yield os.path.join(install, n), "lines"
    yield os.path.join(ROOT, "README.md"), "markdown"
    for d, _, names in os.walk(os.path.join(ROOT, "docs")):
        for n in sorted(names):
            if n.endswith(".md"):
                yield os.path.join(d, n), "markdown"


def snippets(path, kind):
    """(line number, text) of what a person would copy from this file."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    if kind == "lines":
        for no, line in enumerate(lines, 1):
            yield no, line
        return
    if kind == "page":
        for no, line in enumerate(lines, 1):
            for m in re.finditer(r"<code[^>]*>(.*?)</code>|data-copy=\"([^\"]*)\"", line):
                yield no, html.unescape(m.group(1) or m.group(2) or "")
        return
    fenced = False
    for no, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            yield no, line
        else:
            for m in re.finditer(r"`([^`]+)`", line):
                yield no, m.group(1)


def commands():
    for path, kind in sources():
        if not os.path.exists(path):
            continue
        for no, text in snippets(path, kind):
            m = COMMAND.match(text.strip())
            # A verb named on its own, "pkg PUBLISH writes it", is a mention.
            if m and len(m.group(1).split(" #")[0].split()) > 1 and not SYNOPSIS.search(m.group(1).split(" #")[0]):
                yield os.path.relpath(path, ROOT), no, m.group(1)


def words(command):
    """The words pkg would receive, placeholders made plain, comments cut."""
    command = re.split(r"\s+[#;]\s", command, maxsplit=1)[0]   # "# exits 11", "; a comment"
    command = command.split(" >")[0].split(" |")[0]            # redirections and pipes
    command = PLACEHOLDER.sub("x", command)
    try:
        return shlex.split(command)
    except ValueError:
        return command.split()


def judge(pkg, argv):
    env = dict(os.environ, PKG_CHECK_WORDS="1", PKG_COLOR="never")
    r = subprocess.run([pkg] + argv, capture_output=True, text=True, env=env)
    first = (r.stdout + r.stderr).strip().split("\n")[0]
    return r.returncode, first


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pkg", default=os.path.join(ROOT, "build", "pkg"))
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    code, first = judge(a.pkg, ["VERSION"])
    if code != 0:
        print(f"check-commands: {a.pkg} does not answer: {first}", file=sys.stderr)
        return 2

    seen = refused = 0
    for path, no, command in commands():
        argv = words(command)
        if not argv:
            continue
        seen += 1
        code, first = judge(a.pkg, argv)
        # A transcript may show a wrong command on purpose, "# exits 20". pkg
        # decides some of those from the words and some only once it looks
        # (MOUNTLIST of a package that is no image), so either answer holds.
        # Any other documented exit comes after the words were taken.
        said = re.search(r"#\s*exits\s+(\d+)", command)
        bad = code not in ((0, 20) if said and said.group(1) == "20" else (0,))
        refused += bad
        if bad or a.list:
            print(f"{'FAIL' if bad else 'ok  '} {path}:{no}: pkg {' '.join(argv)}" + (f"\n       {first} (exit {code})" if bad else ""))
    print(f"check-commands: {seen} commands in the portal and the docs, {refused} that pkg refuses "
          f"(judged by {os.path.relpath(a.pkg, ROOT)} with PKG_CHECK_WORDS)")
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main())
