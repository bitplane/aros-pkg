#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# One program, several AROS machines: a developer on a Mac builds the same
# tool for aarch64 (hosted) and x86_64 (native) and publishes both into one
# channel, with a library it needs, also built twice, and a generic data
# package. Each root then gets the build for its own machine.

set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-crossarch.XXXXXX")
trap 'rm -rf "$T"' EXIT
checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

$PKG KEYGEN FILE "$T/k" > /dev/null
export PKG_SIGNKEY="$T/k"
CH="$T/ch"

# An ELF header is enough for Pkg to tell the CPU: e_machine 183 aarch64, 62 x86_64.
elf() {  # elf <file> <e_machine> <tag>
    python3 -c "
import sys
b = bytearray(64); b[0:4] = b'\x7fELF'; b[4] = 2; b[5] = 1; b[18] = int(sys.argv[2])
open(sys.argv[1], 'wb').write(bytes(b) + sys.argv[3].encode())
" "$1" "$2" "$3"
}
for cpu in aarch64:183 x86_64:62; do
    n=${cpu%%:*} m=${cpu#*:}
    mkdir -p "$T/tool-$n/C" "$T/lib-$n/Libs" "$T/tool2-$n/C"
    elf "$T/tool-$n/C/Tool" "$m" "tool 1.0 $n"
    elf "$T/tool2-$n/C/Tool" "$m" "tool 1.1 $n"
    elf "$T/lib-$n/Libs/helper.library" "$m" "helper $n"
done
mkdir -p "$T/data/Docs"; printf 'manual\n' > "$T/data/Docs/Manual"

echo "publish"
$PKG PUBLISH "$T/lib-aarch64" CHANNEL "$CH" NAME helper VERSION 1.0 KIND library MACHINE > "$T/p1"
has "$T/p1" '^result: published$';                    ok $? "helper 1.0 for aarch64"
$PKG PUBLISH "$T/lib-x86_64" CHANNEL "$CH" NAME helper VERSION 1.0 KIND library > /dev/null
                                                      ok $? "helper 1.0 for x86_64, the same version, another CPU"
for n in aarch64 x86_64; do
    $PKG PUBLISH "$T/tool-$n" CHANNEL "$CH" NAME tool VERSION 1.0 KIND application DEPENDS helper > /dev/null
                                                      ok $? "tool 1.0 for $n"
done
$PKG PUBLISH "$T/data" CHANNEL "$CH" NAME manual VERSION 1 KIND data > /dev/null
                                                      ok $? "a generic package"
cp -R "$T/lib-x86_64" "$T/lib-x86_64b"; printf 'x' >> "$T/lib-x86_64b/Libs/helper.library"
$PKG PUBLISH "$T/lib-x86_64b" CHANNEL "$CH" NAME helper VERSION 1.0 KIND library > "$T/p2" 2>&1
[ $? -eq 15 ];                                        ok $? "other bytes for the same version and CPU are refused"
[ "$(awk '$1=="tool"{print $3}' "$CH/index" | sort | tr '\n' ' ')" = "aarch64 x86_64 " ]
                                                      ok $? "the index names each entry's CPU"
$PKG SHOW tool CHANNEL "$CH" MACHINE > "$T/sh"
has "$T/sh" '^entry: tool 1.0 application aarch64 ok ' && has "$T/sh" '^entry: tool 1.0 application x86_64 ok '
                                                      ok $? "SHOW lists both builds"

echo "install"
R="$T/native"
$PKG INSTALL tool ROOT "$R" CHANNEL "$CH" MACHINE > "$T/i0" 2>&1
[ $? -eq 20 ] && has "$T/i0" 'several CPUs (aarch64, x86_64)' && has "$T/i0" '^next: fix-command$' \
    && [ ! -e "$R/C/Tool" ];                          ok $? "with no machine known, the choice is refused and nothing placed"
$PKG INSTALL tool ROOT "$R" CHANNEL "$CH" ARCH x86_64 MACHINE > "$T/i1" 2>&1
[ $? -eq 0 ] && cmp -s "$R/C/Tool" "$T/tool-x86_64/C/Tool" && cmp -s "$R/Libs/helper.library" "$T/lib-x86_64/Libs/helper.library"
                                                      ok $? "ARCH x86_64 installs the x86_64 tool, and the x86_64 helper with it"
[ "$(cat "$R/.pkg/arch")" = x86_64 ];                 ok $? "the root records its machine"
$PKG INSTALL manual ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
[ -f "$R/Docs/Manual" ];                              ok $? "a generic package installs on it"
$PKG INSTALL tool ROOT "$R" CHANNEL "$CH" ARCH aarch64 > "$T/i2" 2>&1
[ $? -eq 20 ] && has "$T/i2" 'root for x86_64';       ok $? "ARCH naming another CPU than the root's is refused"
for n in aarch64 x86_64; do
    $PKG PUBLISH "$T/tool2-$n" CHANNEL "$CH" NAME tool VERSION 1.1 KIND application DEPENDS helper > /dev/null
done
$PKG UPGRADE tool ROOT "$R" CHANNEL "$CH" MACHINE > "$T/u1" 2>&1
[ $? -eq 0 ] && cmp -s "$R/C/Tool" "$T/tool2-x86_64/C/Tool"
                                                      ok $? "UPGRADE, with no ARCH, stays on the root's CPU"
$PKG ROLLBACK tool ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
cmp -s "$R/C/Tool" "$T/tool-x86_64/C/Tool";           ok $? "and ROLLBACK too"

H="$T/hosted"
$PKG INSTALL tool VERSION 1.0 ROOT "$H" CHANNEL "$CH" ARCH aarch64 > /dev/null 2>&1
cmp -s "$H/C/Tool" "$T/tool-aarch64/C/Tool" && cmp -s "$H/Libs/helper.library" "$T/lib-aarch64/Libs/helper.library"
                                                      ok $? "another root gets the aarch64 builds"

echo "withdraw"
$PKG WITHDRAW tool VERSION 1.1 CHANNEL "$CH" > "$T/w0" 2>&1
[ $? -eq 20 ] && has "$T/w0" 'several CPUs';          ok $? "WITHDRAW of a version built for two CPUs asks which"
$PKG WITHDRAW tool VERSION 1.1 CHANNEL "$CH" ARCH x86_64 > /dev/null 2>&1
                                                      ok $? "WITHDRAW ARCH x86_64"
$PKG INSTALL tool ROOT "$T/n2" CHANNEL "$CH" ARCH x86_64 MACHINE > "$T/w1" 2>&1
has "$T/w1" '^version: 1.0$';                         ok $? "x86_64 roots no longer get 1.1"
$PKG INSTALL tool ROOT "$T/h2" CHANNEL "$CH" ARCH aarch64 MACHINE > "$T/w2" 2>&1
has "$T/w2" '^version: 1.1$';                         ok $? "aarch64 roots still do"

echo "crossarch: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
