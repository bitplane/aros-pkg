#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""The catalogue fields of each contrib component, from the best source: its
Aminet .readme, then what it states elsewhere (about-stated.txt: descriptions
quoted from its readme, authors, links), its licence file, and last the table
of what is written here for it (about.txt). Writes, for publish-nightly.sh:

    <out>/<name>.args      PUBLISH keywords, one word per line
    <out>/summary.txt      how many components got each field, from which source
    <out>/missing.txt      the components with no Short or Category of their own

    about.py <archive> <top dir> <split table> <out dir>"""
import os, re, subprocess, sys

archive, top, table, out = sys.argv[1:5]
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(out, exist_ok=True)
subprocess.run([sys.executable, os.path.join(here, "gather-about.py"), archive, top, table, out], check=True)

TYPES = {"biz", "comm", "demo", "dev", "disk", "docs", "driver", "game", "gfx", "hard", "misc", "mods",
         "mus", "pix", "text", "util"}
def valid_category(c):
    m = re.fullmatch(r"([a-z]+)/([a-z0-9]{1,8})", c or "")
    return bool(m) and m.group(1) in TYPES

def rows(path, n):
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        cells = [c.strip() for c in line.split("|")]
        yield cells + [""] * (n - len(cells))

table_rows = {r[0]: r for r in rows(os.path.join(here, "about.txt"), 5)}
stated = {}
for name, field, value, source in rows(os.path.join(here, "about-stated.txt"), 4):
    stated.setdefault(name, {})[field] = (value, source)
gathered = {}
for line in open(os.path.join(out, "about.tsv")):
    name, readme, desc, licence, author, used = (line.rstrip("\n").split("\t") + [""] * 6)[:6]
    gathered[name] = dict(readme=readme, licence=licence, author=author, used=used)

OPEN = {"GPL-2.0", "GPL-3.0", "LGPL-2.0", "LGPL-2.1", "LGPL-3.0", "MIT", "MPL-2.0", "Artistic-1.0"}
counts = {}
def count(field, source):
    counts.setdefault(field, {}).setdefault(source, 0)
    counts[field][source] += 1

missing = []
comps = [l.split()[0] for l in open(table) if l.strip() and not l.startswith("#")]
for name in comps:
    t = table_rows.get(name, [name, "", "", "", ""])
    s = stated.get(name, {})
    g = gathered.get(name, {})
    args = []
    readme_short = readme_type = ""
    if g.get("readme"):
        body = open(os.path.join(out, g["readme"]), "rb").read(3000).decode("latin-1")
        m = re.search(r"^Short:\s*(.+?)\s*$", body, re.M)
        readme_short = m.group(1) if m else ""
        m = re.search(r"^Type:\s*(\S+)", body, re.M)
        readme_type = m.group(1) if m else ""
        args += ["README", os.path.join(out, g["readme"])]
    # Short: the readme's when it fits, else the table's
    if readme_short and len(readme_short) <= 40:
        count("short", "Aminet readme")
    else:
        args += ["SHORT", t[3]]
        count("short", "written for it (about.txt)")
        missing.append((name, "short", t[3]))
    if valid_category(readme_type):
        count("category", "Aminet readme")
    else:
        args += ["CATEGORY", t[1]]
        count("category", "written for it (about.txt)")
        missing.append((name, "category", t[1]))
    if t[2]:
        args += ["TAGS", t[2]]
        count("tags", "about.txt")
    if "description" in s:
        args += ["DESCRIPTION", os.path.join(here, s["description"][0])]
        count("description", "its readme, quoted (about-stated.txt)")
    elif g.get("readme"):
        count("description", "Aminet readme")
    if "author" in s:
        args += ["AUTHOR", s["author"][0]]
        count("author", "stated in its files (about-stated.txt)")
    elif g.get("readme") and g.get("author"):
        count("author", "Aminet readme")
    if "homepage" in s:
        args += ["HOMEPAGE", s["homepage"][0]]
        count("homepage", "stated in its files")
    elif t[4]:
        args += ["HOMEPAGE", t[4]]
        count("homepage", "the project's known site (about.txt)")
    if "repository" in s:
        args += ["REPOSITORY", s["repository"][0]]
        count("repository", "stated in its files")
    if g.get("licence"):
        args += ["LICENSE", g["licence"]]
        count("license", "its licence file")
        if g["licence"] in OPEN:
            args += ["DISTRIBUTION", "open-source"]
            count("distribution", "its licence file")
    with open(os.path.join(out, name + ".args"), "w", encoding="utf-8") as f:
        f.write("\n".join(args) + ("\n" if args else ""))

with open(os.path.join(out, "summary.txt"), "w") as f:
    for field in ("short", "description", "category", "tags", "author", "homepage", "repository",
                  "license", "distribution"):
        per = counts.get(field, {})
        f.write("%-12s %3d of %d: %s\n" % (field, sum(per.values()), len(comps),
                "; ".join("%d %s" % (v, k) for k, v in sorted(per.items(), key=lambda x: -x[1])) or "none"))
with open(os.path.join(out, "missing.txt"), "w") as f:
    for name, field, value in missing:
        f.write("%s\t%s\t%s\n" % (name, field, value))
print(open(os.path.join(out, "summary.txt")).read(), end="")
