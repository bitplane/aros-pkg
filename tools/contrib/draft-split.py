#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Draft the split of a nightly contrib archive into packages, from the list
of its paths (one per line, relative to its top directory). Prints one line
per package: `<name> <kind> <path>[,<path>...]`, then the paths no rule
claims, for a person to review. Rules, most specific first:

- a SYS/Packages registration names a package: its drawer and itself;
- Extras/<Category>/<Program> for categories that group programs, and
  Extras/<Program> for the programs that sit directly in Extras;
- the loose files of C, Libs, Classes, Devs, Fonts, Locale, WBStartup, S
  and Prefs, by the names below;
- Developer, the toolchain and SDK, as one package until upstream splits it.
"""
import collections
import re
import sys

GROUPED = {"Demos", "Developer", "Games", "MultiMedia", "Networking", "Utilities", "Zune", "System"}
DEEPER = {"Games", "MultiMedia", "Networking", "Utilities"}      # Extras/<Cat>/<Genre>/<Program>
COLLECTIONS = {"Extras/Misc/fish": "fish-collection", "Extras/Misc/aminet": "aminet-collection"}
LOOSE = [
    (r"^C/RexxMast$|^Libs/regina\.library$", "regina"),
    (r"^C/(Zip|ZipCloak|ZipNote|ZipSplit|UnZip|UnZipSfx|fUnZip)$", "info-zip"),
    (r"^C/xad|^Libs/xadmaster\.library$|^Libs/XAD/", "xadmaster"),
    (r"^Libs/SDL\.library$", "sdl"), (r"^Libs/SDL2\.library$", "sdl2"), (r"^Libs/SDL3\.library$", "sdl3"),
    (r"^Libs/Warp3D\.library$", "wazp3d"),
    (r"^Libs/(guigfx|render|mysticview|openal|pixman|lcms2|gtlayout|ptplay|xprzmodem)\.library$", None),
    (r"^C/iperf3$", "iperf3"), (r"^C/sockperf$", "sockperf"),
    (r"^Prefs/Env-Archive/Scalos/|^Prefs/Env-Archive/deficons\.prefs$", "scalos"),
    (r"^Prefs/Env-Archive/wgetcfg$", "wget"),
    (r"^S/DeveloperShell-Startup$|^Developer/", "sdk"),
    (r"^Fonts/TrueType/", "fonts-truetype"),
]

def name_of(s):
    return re.sub(r"[^a-z0-9+._-]", "-", s.lower()).strip("-")

def main():
    paths = [l.strip() for l in open(sys.argv[1]) if l.strip()]
    units = collections.OrderedDict()
    claimed = set()

    def add(name, kind, prefix):
        u = units.setdefault(name, [kind, []])
        if prefix not in u[1]:
            u[1].append(prefix)

    regs = [p for p in paths if p.startswith("Prefs/Env-Archive/SYS/Packages/")]
    for p in paths:
        parts = p.split("/")
        if parts[0] == "Extras" and len(parts) >= 3:
            prefix = "/".join(parts[:2])
            for coll, cname in COLLECTIONS.items():
                if p.startswith(coll + "/"):
                    add(cname, "application", coll); claimed.add(p); break
            else:
                if parts[1] in DEEPER and len(parts) >= 5:
                    prefix = "/".join(parts[:4])
                elif parts[1] in GROUPED and len(parts) >= 4:
                    prefix = "/".join(parts[:3])
                add(name_of(prefix.split("/")[-1]), "application", prefix)
                claimed.add(p)
            continue
        for rx, nm in LOOSE:
            if re.search(rx, p):
                if nm is None:
                    nm = name_of(parts[-1].replace(".library", ""))
                kind = "sdk" if nm == "sdk" else "library" if p.startswith("Libs/") else "font" if p.startswith("Fonts/") else "application"
                add(nm, kind, p)
                claimed.add(p)
                break
    for r in regs:
        nm = name_of(r.split("/")[-1])
        if nm.startswith("mcc_"):
            nm = nm.replace("_", "-")
        target = nm if nm in units else None
        if target is None:
            for u in units:
                if u.replace("-", "").replace("_", "") == nm.replace("-", "").replace("_", ""):
                    target = u
        add(target or nm, units.get(target, ["application"])[0] if target else "application", r)
        claimed.add(r)
    for n, (k, ps) in units.items():
        print(n, k, ",".join(ps))
    rest = [p for p in paths if p not in claimed and p not in ("LICENSE", "ACKNOWLEDGEMENTS")]
    print("# unassigned:", len(rest))
    for p in rest:
        print("#  ", p)

main()
