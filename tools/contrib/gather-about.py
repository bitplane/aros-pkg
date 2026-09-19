#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""What each contrib component says about itself, read from the nightly archive
once: its Aminet .readme (Short, Author, Type and text), another readme (its
first paragraphs, as the description), and its licence file, named as SPDX
only when the text is recognised. Nothing is guessed: a field no file states
is left out.

    gather-about.py <archive> <top dir> <split table> <out dir>

Writes <out dir>/<name>.readme (an Aminet readme, for PUBLISH README),
<out dir>/<name>.description (for DESCRIPTION), and <out dir>/about.tsv:
name, readme file, description file, licence, author, the archive paths used."""
import os, re, subprocess, sys, tarfile

archive, top, table, out = sys.argv[1:5]
os.makedirs(out, exist_ok=True)

comps = []
for line in open(table):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    name, kind, paths = line.split(None, 2)
    comps.append((name, [p.strip() for p in paths.split(",")]))

README = re.compile(r"(\.readme|/readme(\.txt|\.md)?|/read\.?me)$", re.I)
LICENCE = re.compile(r"/(copying|license|licence|copyright)(\.txt|\.md)?$|/(gpl|lgpl)-[0-9.]+\.txt$", re.I)

def owner(path):
    """The component a path belongs to, and the path inside the component's drawer."""
    for name, paths in comps:
        for p in paths:
            if path == p or path.startswith(p.rstrip("/") + "/"):
                return name, path[len(p.rstrip("/")) + 1:]
    return None, None

wanted = {}
listing = subprocess.run(["bsdtar", "-tf", archive], capture_output=True, text=True, errors="replace").stdout
for member in listing.splitlines():
    if not member.startswith(top + "/"):
        continue
    rel = member[len(top) + 1:]
    name, inner = owner(rel)
    if name is None or name == "sdk":
        continue
    # a component's own files: at the top of its drawer, or in its Docs/
    depth_ok = inner is not None and (inner.count("/") == 0 or (inner.count("/") == 1 and inner.lower().startswith("docs/")))
    if depth_ok and (README.search("/" + rel) or LICENCE.search("/" + rel)):
        wanted.setdefault(name, []).append(rel)

members = [top + "/" + r for rs in wanted.values() for r in rs]
text = {}
if members:
    tmp = os.path.join(out, ".extract")
    os.makedirs(tmp, exist_ok=True)
    subprocess.run(["bsdtar", "-xf", archive, "-C", tmp] + members, check=True)
    for m in members:
        with open(os.path.join(tmp, m), "rb") as f:
            text[m[len(top) + 1:]] = f.read()

def spdx(b):
    # the licence's title, its first lines: the GPL's preamble names the LGPL
    t = b.decode("latin-1")
    head = " ".join(l.strip() for l in t.strip().splitlines()[:4]).upper()
    if "LESSER GENERAL PUBLIC LICENSE" in head:
        return "LGPL-3.0" if "VERSION 3" in head else "LGPL-2.1" if "VERSION 2.1" in head else None
    if "LIBRARY GENERAL PUBLIC LICENSE" in head:
        return "LGPL-2.0" if "VERSION 2" in head else None
    if "GNU GENERAL PUBLIC LICENSE" in head:
        return "GPL-3.0" if "VERSION 3" in head else "GPL-2.0" if "VERSION 2" in head else None
    if "MOZILLA PUBLIC LICENSE" in head and "2.0" in head:
        return "MPL-2.0"
    if head.startswith("MIT LICENSE") or head.startswith("THE MIT LICENSE") or "PERMISSION IS HEREBY GRANTED, FREE OF CHARGE" in head:
        return "MIT"
    if "THE ARTISTIC LICENSE" in head:
        return "Artistic-1.0"
    return None

rows = []
for name, _ in comps:
    files = sorted(wanted.get(name, []))
    readme = desc = licence = author = ""
    used = []
    for rel in files:
        b = text.get(rel, b"")
        if rel.lower().endswith(".readme") and re.search(rb"^Short:\s*\S", b[:2000], re.M) and not readme \
                and os.path.basename(rel).lower().startswith(name.split("-")[0][:4].lower()):
            readme = os.path.join(out, name + ".readme")
            open(readme, "wb").write(b)
            m = re.search(rb"^Author:\s*(.+)$", b[:2000], re.M)
            if m:
                a = m.group(1).decode("latin-1").strip()
                inparen = re.match(r"^\S+@\S+\s*\((.+)\)$", a)          # "mail (Name)"
                a = inparen.group(1) if inparen else re.sub(r"\s*<[^>]*>|\s*\S+@\S+", "", a)
                a = re.sub(r"\s*\(https?://[^)]*\)", "", a).strip(" ,")
                author = a
            used.append(rel)
    for rel in files:
        b = text.get(rel, b"")
        if readme or desc or not README.search("/" + rel) or rel.lower().endswith(".readme"):
            continue
        # the first paragraphs of a plain readme, markdown headings dropped
        lines, paras = [], 0
        for l in b.decode("latin-1").splitlines():
            l = l.rstrip()
            if l.startswith("#") or set(l) <= set("=-*_ "):
                if lines and not l.strip():
                    paras += 1
                continue
            if not l.strip():
                if lines and lines[-1] != "":
                    lines.append("")
                    paras += 1
                if paras >= 2:
                    break
                continue
            lines.append(l.strip())
        if sum(1 for l in lines if l) >= 1:
            desc = os.path.join(out, name + ".description")
            open(desc, "w").write("\n".join(lines).strip() + "\n")
            used.append(rel)
    for rel in files:
        if LICENCE.search("/" + rel) and not licence:
            licence = spdx(text.get(rel, b"")) or ""
            if licence:
                used.append(rel)
    rows.append((name, os.path.basename(readme), os.path.basename(desc), licence, author, ",".join(used)))

with open(os.path.join(out, "about.tsv"), "w") as f:
    for r in rows:
        f.write("\t".join(r) + "\n")
print("gather-about: %d components; an Aminet readme for %d, a description from a readme for %d, "
      "a licence for %d, an author for %d" % (len(rows), sum(1 for r in rows if r[1]),
      sum(1 for r in rows if r[1] or r[2]), sum(1 for r in rows if r[3]), sum(1 for r in rows if r[4])))
