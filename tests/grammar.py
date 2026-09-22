#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""docs/reference.md held to what pkg says it takes.

`pkg HELP MACHINE` is pkg's own account of its grammar: one `verb:` record
per verb, followed by its `keyword:` and `switch:` records, drawn from the
templates it reads its words with. This test keeps no copy of that grammar.
Each verb's row in the reference must name exactly those keywords and
switches: a keyword the reference promises and
the verb refuses is a lie, and one the verb takes and the reference omits is
a secret. The global words (MACHINE, TRACE, LOG, ENVIRONMENT) are described
once, in their own table, and are not repeated per verb."""
import os, re, subprocess, sys

pkg = os.environ.get("PKG", "./build/pkg")
repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = subprocess.run([pkg, "HELP", "MACHINE"], capture_output=True, text=True).stdout
# the records pkg prints about itself: verb:, then its keyword: and switch:
taken = {}
cur = None
for line in out.splitlines():
    k, _, v = line.partition(": ")
    if k == "verb":
        cur = v
        key = v.split()[0]
        taken.setdefault(key, set())
        if len(v.split()) == 2:
            taken[key].add(v.split()[1])          # ADD, LIST: the row names them
    elif k in ("keyword", "switch") and cur is not None:
        taken[cur.split()[0]].add(v.split()[0])
for apart in ("HELP", "VERSION", "PORT"):
    taken.pop(apart, None)                        # rows of their own, not templates
if not taken:
    print("grammar: pkg HELP MACHINE listed no verb"); sys.exit(1)
rows = {}
for line in open(os.path.join(repo, "docs/reference.md")):
    m = re.match(r"\| (\[`[A-Z]+[^|]*)\| `([^`]*)`", line)
    if m:
        for v in re.findall(r"\[`([A-Z]+)(?: [A-Z]+)?`\]", m.group(1)):
            rows.setdefault(v, set()).update(set(re.findall(r"\b[A-Z][A-Z]+\b", m.group(2))))
checks = fails = 0
def ok(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1; print("  FAIL", what)
for key, kw in sorted(taken.items()):
    row = rows.get(key)
    ok(row is not None, f"{key} has a row in docs/reference.md")
    if row is None: continue
    row = row - {key} - {"PUBLISH", "PACKAGE"}   # the two names of one verb share a row
    missing = kw - row
    extra = row - kw - {"ALL", "ORPHANS"} if key not in ("UPGRADE", "VERIFY", "REPAIR", "REMOVE") else row - kw
    ok(not missing, f"{key}: the reference omits {sorted(missing)}, which {key} takes")
    ok(not extra, f"{key}: the reference promises {sorted(extra)}, which {key} refuses")
# each verb's page opens with its syntax, in the first plain code block
pages = os.path.join(repo, "docs/commands")
for key, kw in sorted(taken.items()):
    page = os.path.join(pages, key.lower() + ".md")
    if key == "PACKAGE": page = os.path.join(pages, "publish.md")
    if not os.path.exists(page):
        ok(False, f"{key} has a page, docs/commands/{key.lower()}.md"); continue
    text = open(page).read()
    m = re.search(r"^```\n(.*?)^```", text, flags=re.M | re.S)
    ok(m is not None, f"docs/commands/{os.path.basename(page)} opens with its syntax")
    if m is None: continue
    said = set(re.findall(r"\b[A-Z][A-Z]+\b", m.group(1))) - {key, "PUBLISH", "PACKAGE", "ALL", "ORPHANS"}
    missing = kw - said - {"ALL", "ORPHANS"}
    ok(not missing, f"docs/commands/{os.path.basename(page)}: the syntax omits {sorted(missing)}")
    ok(not (said - kw), f"docs/commands/{os.path.basename(page)}: the syntax promises {sorted(said - kw)}, which {key} refuses")
print(f"grammar: {checks} checks, {fails} failures")
sys.exit(1 if fails else 0)
