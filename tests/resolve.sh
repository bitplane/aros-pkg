#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# RESOLVE on a host: the loader's order (the program's directory, its
# libs/, then LIBS:), the checks it makes on each file (the CPU, a resident
# tag, the type), the verdicts and exit codes, the package that owns a file,
# a program's libraries in one table; and what PUBLISH derives: Provides:
# lines, and a warning for a library a program opens that nothing provides.
# The libraries are real aarch64 AROS libraries (tools/build-aros-testlib.sh)
# when built, else stand-ins with a resident tag laid out the same way.

set -u
PKG=${PKG:-./build/pkg}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
here=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-resolve.XXXXXX")
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# lib <file> <version> <revision> [noresident|x86_64]: an aarch64 AROS library as
# far as the loader's checks go: ELF header, $VER, a resident tag of type library
python3 - <<'PY'
import struct, sys
def lib(path, ver, rev, how=""):
    h = bytearray(64); h[0:4] = b"\x7fELF"; h[4] = 2; h[5] = 1
    struct.pack_into("<H", h, 18, 62 if how == "x86_64" else 183)
    body = bytearray(b"\0$VER: pkgtest.library %d.%d (19.9.2026)\0" % (ver, rev))
    while (len(h) + len(body)) % 8: body.append(0)
    rt = bytearray(48)
    if how != "noresident": rt[0:2] = b"\xfc\x4a"
    rt[24] = 0x80; rt[25] = ver; rt[26] = 9
    open(path, "wb").write(bytes(h) + bytes(body) + bytes(rt))
import os
os.makedirs("root/Libs", exist_ok=True); os.makedirs("game/libs", exist_ok=True)
os.makedirs("bad/libs", exist_ok=True); os.makedirs("cpu/libs", exist_ok=True)
lib("root/Libs/pkgtest.library", 2, 30)
lib("game/libs/pkgtest.library", 1, 0)
lib("bad/pkgtest.library", 2, 30, "noresident")
lib("cpu/libs/pkgtest.library", 2, 30, "x86_64")
lib("root/Libs/good.library", 2, 30)
h = bytearray(64); h[0:4] = b"\x7fELF"; h[4] = 2; h[5] = 1; h[18] = 183
open("game/Game", "wb").write(bytes(h) + b"\0good.library\0pkgtest.library\0absent.library\0dos.library\0")
PY
if [ -f "$here/../build/aros/pkgtest-2.30.library" ]; then
    cp "$here/../build/aros/pkgtest-2.30.library" root/Libs/pkgtest.library
    cp "$here/../build/aros/pkgtest-1.0.library" game/libs/pkgtest.library
    real=" (real AROS builds)"
else
    real=""
fi
R() { $PKG RESOLVE "$@" ARCH aarch64 MACHINE; }

echo "order$real"
R pkgtest.library ROOT root FROM empty > o1 2>&1
[ $? -eq 0 ] && has o1 '^winner: root/Libs/pkgtest.library 2.30$' && has o1 '^candidate: root/Libs/pkgtest.library yes 2.30 - chosen$'
                                                      ok $? "with nothing else, LIBS: (SYS:Libs) gives the library"
R pkgtest.library ROOT root FROM game > o2 2>&1
[ $? -eq 0 ] && has o2 '^winner: game/libs/pkgtest.library 1.0$' && has o2 '^candidate: root/Libs/pkgtest.library yes 2.30 - -$'
                                                      ok $? "PROGDIR:libs comes before LIBS:, whatever its version"
has o2 'hides the newer root/Libs/pkgtest.library 2.30' && has o2 '^next-step: game/libs/pkgtest.library remove game/libs/pkgtest.library'
                                                      ok $? "and a copy that hides a newer one is named, with what to do"
[ "$(grep -n '^candidate: ' o2 | cut -d: -f3- | awk '{print $1}' | tr '\n' ' ')" = "game/pkgtest.library game/libs/pkgtest.library root/Libs/pkgtest.library root/Classes/pkgtest.library " ]
                                                      ok $? "every place in the loader's order: PROGDIR:, PROGDIR:libs/, SYS:Libs, SYS:Classes"

echo "versions"
R pkgtest.library VERSION 2.30 ROOT root FROM game > o3 2>&1
[ $? -eq 18 ] && has o3 '^satisfies: no$' && has o3 'too old (1.0, 2.30 needed)'
                                                      ok $? "the first file found is taken though too old: exit 18, saying why"
R pkgtest.library VERSION 2.30 ROOT root FROM empty > o4 2>&1
[ $? -eq 0 ] && has o4 '^satisfies: yes$';           ok $? "a new enough copy satisfies: exit 0"
R nothere.library ROOT root FROM game > o5 2>&1
[ $? -eq 11 ] && has o5 'found nowhere';             ok $? "a library nowhere: exit 11"

echo "checks"
R pkgtest.library ROOT root FROM bad > o6 2>&1
[ $? -eq 0 ] && has o6 '^winner: root/Libs/pkgtest.library 2.30$' && has o6 'bad/pkgtest.library not a library (no resident tag)'
                                                      ok $? "a file first in the path with no resident tag is passed over, not taken"
R pkgtest.library ROOT root FROM cpu > o7 2>&1
[ $? -eq 0 ] && has o7 '^winner: root/Libs/pkgtest.library 2.30$' && has o7 'wrong CPU (x86_64 build on a aarch64 system)'
                                                      ok $? "a build for another CPU is passed over, as LoadSeg refuses it"

echo "program"
R game/Game ROOT root > o8 2>&1
[ $? -eq 11 ] && has o8 '^library: good.library root/Libs/good.library 2.30$' \
  && has o8 '^library: pkgtest.library game/libs/pkgtest.library 1.0$' && has o8 '^library: absent.library - -$' \
  && has o8 '^next-step: absent.library install absent.library' && ! has o8 '^next-step: dos.library'
                                                      ok $? "a program's libraries in one table: one fine, one shadowed, one absent; AROS's own not counted"

echo "packages"
$PKG KEYGEN FILE key > /dev/null
export PKG_SIGNKEY="$T/key"
mkdir -p libpkg/Libs app/C
cp root/Libs/pkgtest.library libpkg/Libs/
$PKG PUBLISH libpkg CHANNEL ch NAME pkgtest VERSION 2.30 KIND library > /dev/null 2>&1
grep -q '^Provides: pkgtest.library$' ch/objects/*.manifest;  ok $? "PUBLISH derives Provides: from the libraries shipped in Libs"
$PKG INSTALL pkgtest ROOT sys CHANNEL ch > /dev/null 2>&1
R pkgtest.library ROOT sys FROM empty > o9 2>&1
has o9 '^candidate: sys/Libs/pkgtest.library yes 2.30 pkgtest 2.30 chosen$';  ok $? "RESOLVE names the package a file belongs to"
cp game/Game app/C/Game
$PKG PUBLISH app CHANNEL ch NAME game VERSION 1 KIND application MACHINE > o10 2>&1
has o10 'C/Game opens good.library, pkgtest.library and absent.library and no dependency provides them' && ! has o10 'dos.library'
                                                      ok $? "PUBLISH warns about each library no dependency provides, AROS's own left out"
$PKG PUBLISH app CHANNEL ch NAME game VERSION 1 KIND application DEPENDS pkgtest DRYRUN MACHINE > o11 2>&1
has o11 'opens good.library and absent.library and' && ! grep 'opens' o11 | grep -q 'pkgtest'
                                                      ok $? "a dependency that provides a library quiets the warning for it"
$PKG RESOLVE absent.library ROOT root FROM game CHANNEL ch ARCH aarch64 MACHINE > o12 2>&1
R pkgtest.library ROOT empty FROM empty CHANNEL ch > o13 2>&1
[ $? -eq 11 ] && has o13 'package pkgtest 2.30 provides pkgtest.library (pkg INSTALL pkgtest'
                                                      ok $? "with CHANNEL, a missing library names the package that provides it"

echo
echo "resolve: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
