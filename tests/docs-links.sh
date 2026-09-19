#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Every relative link in the documentation points at a file that exists, and
# at a heading that exists when it names one. Fenced code is not read.
here=$(dirname "$0")
exec python3 - "$here/.." <<'PY'
import os, re, sys
repo = os.path.abspath(sys.argv[1])
files = [os.path.join(repo, "README.md"), os.path.join(repo, "GOAL.md"), os.path.join(repo, "OPEN.md")]
for d, _, fs in os.walk(os.path.join(repo, "docs")):
    files += [os.path.join(d, f) for f in fs if f.endswith(".md")]
def anchors(text):
    out = set()
    for h in re.findall(r"^#+ +(.*)$", text, re.M):
        a = re.sub(r"[^\w\s-]", "", h.lower().replace("`", "")).strip().replace(" ", "-")
        out.add(a)
    return out
texts = {f: open(f).read() for f in files}
bad = 0
for f, text in texts.items():
    prose = re.sub(r"```.*?```", "", text, flags=re.S)
    for target in re.findall(r"\]\(([^)\s]+)\)", prose):
        if re.match(r"[a-z]+:", target) or target.startswith("#"):
            if target.startswith("#") and target[1:] not in anchors(text):
                print("%s: no heading %s" % (os.path.relpath(f, repo), target)); bad += 1
            continue
        path, _, frag = target.partition("#")
        full = os.path.normpath(os.path.join(os.path.dirname(f), path))
        if not os.path.exists(full):
            print("%s: missing %s" % (os.path.relpath(f, repo), target)); bad += 1
        elif frag and full in texts and frag not in anchors(texts[full]):
            print("%s: no heading %s in %s" % (os.path.relpath(f, repo), frag, path)); bad += 1
print("docs-links: %d files, %d broken" % (len(files), bad))
sys.exit(1 if bad else 0)
PY
