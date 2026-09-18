#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""What a nightly contrib archive does that makes packaging it harder, for
cleaning up upstream. Pkg packages it anyway; this only lists what it met.

    python3 tools/contrib/audit.py <archive>.pkgidx <split table>

Reads the archive's metadata index (Pkg writes it beside the archive on the
first publish) and the split table, and prints per package: no $VER at all,
or $VER cookies under other names than the package's.
"""
import collections
import sys

def main():
    idx, table = sys.argv[1], sys.argv[2]
    files = []
    with open(idx) as f:
        next(f)
        for line in f:
            parts = line.rstrip("\n").split(" ", 6)
            if len(parts) == 7:
                digest, size, mode, arch, cname, cver, path = parts
                files.append((path.split("/", 1)[1] if "/" in path else path, arch, cname, cver))
    none, other = [], []
    for line in open(table):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, kind, paths = line.split(None, 2)
        prefixes = [p.rstrip("/") for p in paths.split(",")]
        mine = [f for f in files if any(f[0] == p or f[0].startswith(p + "/") for p in prefixes)]
        cookies = collections.Counter(f"{c} {v}" for _, _, c, v in mine if c != "-")
        squash = lambda s: "".join(ch for ch in s.lower() if ch.isalnum())
        if any(squash(k.split()[0]) == squash(name) for k in cookies):
            continue
        if len(cookies) == 1:
            continue        # one program in it: Pkg takes that cookie's version
        if not cookies:
            none.append(name)
        else:
            other.append((name, ", ".join(k for k, _ in cookies.most_common(3))))
    print(f"no $VER in any file ({len(none)}):")
    print("  " + " ".join(none))
    print(f"several $VER cookies, none named like the package, so no version ({len(other)}):")
    for n, c in other:
        print(f"  {n}: {c}")

main()
