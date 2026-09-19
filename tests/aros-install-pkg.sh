#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Install-Pkg, as tools/make-aros-channel.sh writes it, run on hosted AROS
# from the two places people run it:
#
#   drawer   Pkg-<cpu>/, as the portal zips it: one CPU's bare Pkg, the
#            signed pkg package for that CPU, Install-Pkg and ReadMe
#   full     the whole channel, with the macOS, Linux and Windows builds
#            under Bootstrap/ beside the AROS ones; and once more with no T:
#            assign, as after a minimal boot
#
# Both must install Pkg through its own checks (signature, digest, pin), the
# script must probe only the AROS CPUs' bootstraps, never a host build, and
# its closing lines must name where newer versions are, not the drawer.
#
# Needs: make; tools/build-aros.sh (and the host builds: make build/pkg-macos
# build/pkg-linux-x86_64 build/pkg-linux-aarch64 build/pkg.exe).

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-instpkg.XXXXXX")
share="$work/share"
aros_started=0
cleanup() {
    [ "$aros_started" = 0 ] || "$control" stop > /dev/null 2>&1 || true
    [ "${KEEP:-0}" = 1 ] && { echo "kept $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
for need in "$host_pkg" "$repo_root/build/aros/Pkg" "$repo_root/build/pkg-linux-aarch64" "$control"; do
    [ -e "$need" ] || { echo "aros-install-pkg: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "aros-install-pkg: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }
host_names='macos-arm64|macos-x86_64|linux-x86_64|linux-arm64|windows-x86_64'

echo "aros-install-pkg: the channel, the drawer, and the script"
mkdir -p "$share/out" "$share/r1" "$share/r2" "$share/r3"
"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
PKG_SIGNKEY="$work/dev.key" sh "$repo_root/tools/make-aros-channel.sh" "$share/full" > "$work/mk" 2>&1
ok $? "make-aros-channel.sh makes the full channel"
ls "$share/full/Bootstrap" | grep -q -E "^($host_names)\$";  ok $? "the full channel holds host builds under Bootstrap/"
! grep -E "Bootstrap/($host_names)/" "$share/full/Install-Pkg" > /dev/null
ok $? "Install-Pkg names no host build"
grep -q 'Bootstrap/aarch64/Pkg' "$share/full/Install-Pkg";   ok $? "and probes the aarch64 bootstrap"
! grep -q 'UPGRADE pkg ROOT <ROOT> CHANNEL <CHANNEL>' "$share/full/Install-Pkg"
ok $? "its closing hint no longer names the channel it was run from"
grep -q 'Newer versions of Pkg: https://' "$share/full/Install-Pkg";  ok $? "it names where newer versions are"
has "$share/full/Bootstrap/SHA256SUMS" '  Install-Pkg$';        ok $? "SHA256SUMS lists this Install-Pkg"
(cd "$share/full" && shasum -a 256 -c Bootstrap/SHA256SUMS > /dev/null)
ok $? "and matches it"

# The check above must catch the script as it was: generate it with the
# previous make-aros-channel.sh and require host builds in it.
git -C "$repo_root" show 4d6095d:tools/make-aros-channel.sh > "$work/old.sh" 2> /dev/null && {
    mkdir -p "$work/oldrepo/tools"
    cp "$work/old.sh" "$work/oldrepo/tools/make-aros-channel.sh"
    cp "$repo_root/tools/pkg-about.txt" "$repo_root/tools/pkg-changes.txt" "$work/oldrepo/tools/"
    ln -s "$repo_root/build" "$work/oldrepo/build"
    PKG_SIGNKEY="$work/dev.key" sh "$work/oldrepo/tools/make-aros-channel.sh" "$work/oldch" > /dev/null 2>&1
    grep -E "Bootstrap/($host_names)/" "$work/oldch/Install-Pkg" > /dev/null
    ok $? "control: the previous script probed host builds, which the check above catches"
}

# The drawer, as the portal's Get Pkg zip lays it out for aarch64.
d="$share/drawer"
mkdir -p "$d/Bootstrap/aarch64" "$d/objects"
cp "$share/full/Bootstrap/aarch64/Pkg" "$d/Bootstrap/aarch64/"
cp "$share/full/Install-Pkg" "$share/full/ReadMe" "$d/"
awk '$1=="pkg" && $3=="aarch64"' "$share/full/index" | tail -1 > "$d/index"
m=$(awk '{print $4}' "$d/index")
p=$(awk '/^Payload:/{print $2}' "$share/full/objects/$m.manifest")
cp "$share/full/objects/$m.manifest" "$share/full/objects/$m.sig" "$share/full/objects/$p.pkg" "$d/objects/"
[ -n "$m" ] && [ -n "$p" ];                                   ok $? "the drawer holds pkg for aarch64 and nothing else"

# What Install-Pkg prints cannot be captured: Execute's own redirection does
# not reach the commands of the script. So the runs are judged by $RC, by the
# files they leave and by VERIFY. The third run is a boot without T:, where
# AmigaDOS's Execute cannot start any script that takes arguments (it writes
# the substituted script to T:): it must fail cleanly and install nothing.
script="C:Execute MacRW:drawer/Install-Pkg MacRW:drawer MacRW:r1
C:Echo \"\$RC\" >MacRW:out/drawer.rc
C:Execute MacRW:full/Install-Pkg MacRW:full MacRW:r2
C:Echo \"\$RC\" >MacRW:out/full.rc
C:Assign T:
C:Execute MacRW:full/Install-Pkg MacRW:full MacRW:r3
C:Echo \"\$RC\" >MacRW:out/not.rc
C:Assign T: RAM:
MacRW:r1/C/Pkg VERIFY pkg ROOT MacRW:r1 MACHINE >MacRW:out/v1.o
C:Echo \"\$RC\" >MacRW:out/v1.rc
MacRW:r2/C/Pkg VERIFY pkg ROOT MacRW:r2 MACHINE >MacRW:out/v2.o
C:Echo \"\$RC\" >MacRW:out/v2.rc
MacRW:r1/C/Pkg LIST ROOT MacRW:r1 >MacRW:out/l1.o
C:Echo done >MacRW:done"

echo "aros-install-pkg: hosted AROS"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$script" "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 180 ]; do sleep 1; w=$((w + 1)); done
"$control" stop > /dev/null 2>&1 || true
aros_started=0
[ -f "$share/done" ];                                         ok $? "the AmigaDOS script ran to its end"
O="$share/out"
code() { tr -d ' \r' < "$O/$1.rc" 2>/dev/null; }
for w in drawer full; do
    [ "$(code $w)" = 0 ];                                     ok $? "$w: Install-Pkg ends with \$RC 0 (got $(code $w))"
done
[ "$(code not)" != 0 ] && [ ! -e "$share/r3/C/Pkg" ]
ok $? "without T:, Execute cannot start the script: it fails (got $(code not)) and nothing is installed"
for n in 1 2; do
    cmp -s "$share/r$n/C/Pkg" "$repo_root/build/aros/Pkg";    ok $? "r$n: C/Pkg is the aarch64 build, byte for byte"
    [ "$(code v$n)" = 0 ] && has "$O/v$n.o" '^result: intact$'
    ok $? "r$n: VERIFY pkg on AROS: intact against its signed manifest"
done
has "$O/l1.o" '^pkg ';                                        ok $? "r1: LIST names pkg"

echo
echo "aros-install-pkg: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
