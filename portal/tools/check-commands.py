#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Every pkg command the portal and the repository's documentation show, held
to the grammar pkg itself prints.

The portal's pages and installers carry pkg commands of their own, and the
guides carry many more. When the parser changes they must change with it, and
nobody can keep that list in their head. This script builds the list from the
sources, reads the grammar from the pkg binary (`pkg HELP MACHINE` as records
when the binary speaks them, its usage text otherwise), and reports every line
whose verb does not exist or which gives a verb a keyword it does not take.

    python3 portal/tools/check-commands.py [--pkg build/pkg] [--list]

--list prints the whole review list, every command with its file and line.
Without it, only the lines that do not fit the grammar are printed, and the
exit status is 1 when there is one.
"""

import argparse
import html
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Where commands are written for people to copy. The portal's pages and
# installers, and the repository's own documentation.
SOURCES = [
    ("portal/src/Portal/Pages", (".cshtml",)),
    ("portal/src/Portal/Install", ("",)),
    ("docs", (".md",)),
    ("README.md", (".md",)),
]

# pkg as the program: the word pkg or Pkg anywhere, never inside another name
# (Install-Pkg, SYS:.pkg), followed by a verb in capitals that is not an
# AmigaDOS volume (DEPOT:). A program given by its path (C:Pkg,
# RAM:Pkg-bootstrap) counts only as the first word of a line: elsewhere it is
# the argument of another command, as in "Delete RAM:Pkg-bootstrap QUIET".
COMMAND = re.compile(r"(?<![-.:/\w])(?:Pkg|pkg)\s+([A-Z][A-Z0-9]+)(?![:\w])([^\n<>\"`|]*)")
AT_START = re.compile(r"^\s*[\w]+:(?:[\w/]+/)?(?:Pkg|pkg)(?:-bootstrap)?\s+([A-Z][A-Z0-9]+)(?![:\w])([^\n<>\"`|]*)")
WORD = re.compile(r"^[A-Z][A-Z0-9]+$")


def files():
    for rel, exts in SOURCES:
        path = os.path.join(ROOT, rel)
        if os.path.isfile(path):
            yield path
            continue
        for d, _, names in os.walk(path):
            if os.sep + "bin" in d or os.sep + "obj" in d:
                continue
            for n in sorted(names):
                if any(n.endswith(e) for e in exts):
                    yield os.path.join(d, n)


def commands():
    """(file, line, verb, rest) for every command a source shows."""
    for path in files():
        with open(path, encoding="utf-8", errors="replace") as f:
            for no, line in enumerate(f, 1):
                text = html.unescape(line)
                first = AT_START.match(text)
                if first:
                    yield os.path.relpath(path, ROOT), no, first.group(1), first.group(2).strip()
                    text = text[first.end():]
                for m in COMMAND.finditer(text):
                    yield os.path.relpath(path, ROOT), no, m.group(1), m.group(2).strip()


def grammar(pkg):
    """verb -> set of keywords it takes, from the binary."""
    out = subprocess.run([pkg, "HELP", "MACHINE"], capture_output=True, text=True).stdout
    verbs = {}
    # Records, when this pkg prints them: `verb: NAME` then `keyword: WORD` lines.
    current = None
    for line in out.splitlines():
        k, _, v = line.partition(": ")
        if k == "verb":
            current = v.strip().upper()
            verbs.setdefault(current, set())
        elif k in ("keyword", "switch") and current:
            verbs[current].add(v.strip().split()[0].upper())
    if verbs:
        return verbs, "records"
    # Otherwise the usage text, which is written for people and so only
    # mostly regular: a verb line starts two spaces in; its template and the
    # continuation lines name its keywords in capitals; "On any verb" lists
    # keywords every verb takes; and a few verbs are only named in the notes
    # at the end ("pkg PORT [<portname>]", "pkg ENV ADD ...").
    current, section = None, ""
    anyverb = set()
    for line in out.splitlines():
        if not line.startswith(" ") and line.strip():
            section = line.strip()
            current = None
            # A note names the verb once and its forms after it, "pkg ENV ADD
            # ...; ENV LIST; ENV REMOVE ...": the whole line belongs to it.
            for v in re.findall(r"\bpkg ([A-Z][A-Z0-9]+)\b", line):
                tail = line.split("pkg " + v, 1)[1]
                verbs.setdefault(v, set()).update(re.findall(r"\b[A-Z][A-Z0-9]+\b", tail))
            continue
        m = re.match(r"^  ([A-Z][A-Z0-9]+)\s+(.*)$", line)
        if m and section.startswith("On any verb"):
            anyverb.add(m.group(1))
        elif m:
            current = m.group(1)
            verbs.setdefault(current, set()).update(re.findall(r"\b[A-Z][A-Z0-9]+\b", m.group(2)))
        elif current and re.match(r"^\s{4,}\[", line):
            verbs[current].update(re.findall(r"\b[A-Z][A-Z0-9]+\b", line))
    for v in verbs.values():
        v.update(anyverb)
    return verbs, "usage text"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pkg", default=os.path.join(ROOT, "build", "pkg"))
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    verbs, how = grammar(a.pkg)
    if not verbs:
        print(f"check-commands: {a.pkg} printed no grammar", file=sys.stderr)
        return 2
    keywords = set().union(*verbs.values())

    problems = 0
    seen = 0
    for path, no, verb, rest in commands():
        seen += 1
        why = None
        if verb not in verbs:
            why = f"no verb {verb}"
        else:
            for w in rest.split():
                w = w.strip("[](),.;:")
                if WORD.match(w) and w in keywords and w not in verbs[verb]:
                    why = f"{verb} does not take {w}"
                    break
        if why:
            problems += 1
        if why or a.list:
            print(f"{'FAIL' if why else 'ok  '} {path}:{no}: pkg {verb} {rest}".rstrip() + (f"   <- {why}" if why else ""))
    print(f"check-commands: {seen} commands in the portal and the docs, {problems} that do not fit "
          f"the grammar of {os.path.relpath(a.pkg, ROOT)} ({how})")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
