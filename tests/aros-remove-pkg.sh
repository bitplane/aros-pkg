#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# docs/removing.md, run on hosted AROS. The AROS commands are not typed here:
# they are taken out of the guide's first two ```amigados blocks, with the root
# named as this test's root instead of SYS:, so the guide and this test cannot
# drift apart.
#
#   1  remove Pkg only   C/Pkg is gone, the package's files and its record in
#                        .pkg/db are not, and a Pkg installed again lists it
#   2  remove every trace .pkg is gone, the package's files are not; Pkg
#                        installed again and the package installed again adopt
#                        every file already on the disk, placing none
#
# The negative control runs step 2 without its Delete line on a second root:
# the "every trace" check must then fail.
#
# Needs: make build/pkg; sh tools/build-aros.sh (build/aros/Pkg);
# tools/make-aros-channel.sh's other builds, as tests/aros-install-pkg.sh does.

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
doc="$repo_root/docs/removing.md"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-rmpkg.XXXXXX")
share="$work/share"
aros_started=0
cleanup() {
    [ "$aros_started" = 0 ] || "$control" stop > /dev/null 2>&1 || true
    [ "${KEEP:-0}" = 1 ] && { echo "kept $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
for need in "$host_pkg" "$repo_root/build/aros/Pkg" "$doc" "$control"; do
    [ -e "$need" ] || { echo "aros-remove-pkg: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "aros-remove-pkg: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# The guide's AROS commands. block <n> <root>: the nth ```amigados block of
# docs/removing.md, as AmigaDOS lines that name <root> and the Pkg in it.
block() {
    awk -v want="$1" -v root="$2" '
        /^```amigados$/ { n++; inb = (n == want); next }
        /^```$/         { inb = 0; next }
        inb {
            sub(/ROOT SYS:/, "ROOT " root)
            gsub(/SYS:/, root "/")
            sub(/^Pkg /, root "/C/Pkg ")
            sub(/^Delete /, "C:Delete ")
            print
        }' "$doc"
}

echo "aros-remove-pkg: the guide's AROS commands"
b1=$(block 1 MacRW:r1)
b2=$(block 2 MacRW:r1)
printf '%s\n' "$b1" | grep -q '^MacRW:r1/C/Pkg REMOVE pkg ROOT MacRW:r1$'
ok $? "block 1 of the guide is the removal of Pkg alone"
[ "$(printf '%s\n' "$b2" | wc -l | tr -d ' ')" = 2 ] &&
    printf '%s\n' "$b2" | grep -q '^C:Delete MacRW:r1/\.pkg ALL FORCE$'
ok $? "block 2 is that removal followed by the Delete of the whole .pkg"

mkdir -p "$share/out" "$share/r1" "$share/r2"
"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
PKG_SIGNKEY="$work/dev.key" sh "$repo_root/tools/make-aros-channel.sh" "$share/full" > "$work/mk" 2>&1
ok $? "make-aros-channel.sh makes the channel Install-Pkg installs from"

# One small package of its own, so that what survives Pkg's removal is visible.
mkdir -p "$work/greet/C"
printf 'x\000$VER: greet 1.0 (20.9.2026)\000' > "$work/greet/C/Greet"
PKG_SIGNKEY="$work/dev.key" "$host_pkg" PUBLISH "$work/greet" CHANNEL "$share/ch" \
    NAME greet VERSION 1.0 KIND application > /dev/null 2>&1
ok $? "greet 1.0 published into a channel of its own"

# Install-Pkg's own drawer for aarch64, as the portal's zip lays it out.
d="$share/drawer"
mkdir -p "$d/Bootstrap/aarch64" "$d/objects"
cp "$share/full/Bootstrap/aarch64/Pkg" "$d/Bootstrap/aarch64/"
cp "$share/full/Install-Pkg" "$share/full/ReadMe" "$d/"
awk '$1=="pkg" && $3=="aarch64"' "$share/full/index" | tail -1 > "$d/index"
m=$(awk '{print $4}' "$d/index")
p=$(awk '/^Payload:/{print $2}' "$share/full/objects/$m.manifest")
cp "$share/full/objects/$m.manifest" "$share/full/objects/$m.sig" \
   "$share/full/objects/$p.pkg" "$d/objects/"
[ -n "$m" ] && [ -n "$p" ];                                   ok $? "the drawer holds pkg for aarch64"

# Install-Pkg prints through Execute, whose output cannot be captured, so every
# run is judged by $RC and by the files AROS leaves behind.
step() { s=$1; shift; printf '%s >MacRW:out/%s.o\nC:Echo "$RC" >MacRW:out/%s.rc\n' "$*" "$s" "$s"; }
script="C:Execute MacRW:drawer/Install-Pkg MacRW:drawer MacRW:r1
C:Echo \"\$RC\" >MacRW:out/i1.rc
$(step g1 MacRW:r1/C/Pkg INSTALL greet ROOT MacRW:r1 CHANNEL MacRW:ch MACHINE)
$b1
C:Echo \"\$RC\" >MacRW:out/rm1.rc
C:List MacRW:r1/C >MacRW:out/s1c.txt
C:List MacRW:r1/.pkg/db >MacRW:out/s1db.txt
C:List MacRW:r1/.pkg/keys >MacRW:out/s1k.txt
C:Execute MacRW:drawer/Install-Pkg MacRW:drawer MacRW:r1
C:Echo \"\$RC\" >MacRW:out/i2.rc
$(step l1 MacRW:r1/C/Pkg LIST ROOT MacRW:r1 MACHINE)
$b2
C:Echo \"\$RC\" >MacRW:out/rm2.rc
C:List MacRW:r1 >MacRW:out/s2r.txt
C:List MacRW:r1/C >MacRW:out/s2c.txt
C:Execute MacRW:drawer/Install-Pkg MacRW:drawer MacRW:r1
C:Echo \"\$RC\" >MacRW:out/i3.rc
$(step g2 MacRW:r1/C/Pkg INSTALL greet ROOT MacRW:r1 CHANNEL MacRW:ch MACHINE)
$(step v2 MacRW:r1/C/Pkg VERIFY ALL ROOT MacRW:r1 MACHINE)
C:Execute MacRW:drawer/Install-Pkg MacRW:drawer MacRW:r2
$(step g3 MacRW:r2/C/Pkg INSTALL greet ROOT MacRW:r2 CHANNEL MacRW:ch MACHINE)
MacRW:r2/C/Pkg REMOVE pkg ROOT MacRW:r2 >MacRW:out/rm3.o
C:Echo \"\$RC\" >MacRW:out/rm3.rc
C:List MacRW:r2 >MacRW:out/s3r.txt
C:List MacRW:r2/C >MacRW:out/s3c.txt
C:Echo done >MacRW:done"

echo
echo "aros-remove-pkg: hosted AROS"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$script" "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 240 ]; do sleep 1; w=$((w + 1)); done
"$control" stop > /dev/null 2>&1 || true
aros_started=0
[ -f "$share/done" ];                                         ok $? "the AmigaDOS script ran to its end"
O="$share/out"
code() { tr -d ' \r' < "$O/$1.rc" 2>/dev/null; }

[ "$(code i1)" = 0 ];                                         ok $? "Install-Pkg put Pkg in r1 (\$RC $(code i1))"
[ "$(code g1)" = 0 ] && has "$O/g1.o" '^result: installed$'
ok $? "greet 1.0 installed by the Pkg in r1"

# 1. Pkg alone.
[ "$(code rm1)" = 0 ]
ok $? "the guide's REMOVE pkg, run by the very file it deletes, ends with \$RC 0 (got $(code rm1))"
! has "$O/s1c.txt" '^Pkg' && has "$O/s1c.txt" '^Greet'
ok $? "C/Pkg is gone and C/Greet is not"
has "$O/s1db.txt" '^greet' && ! has "$O/s1db.txt" '^pkg'
ok $? "greet's record stayed; only pkg's record left the database"
has "$O/s1k.txt" '^pkg';                                      ok $? "the key pinned for pkg stayed, as the guide says"
[ "$(code i2)" = 0 ] && has "$O/l1.o" '^package: greet 1.0'
ok $? "Pkg installed again lists greet: the later Pkg takes over where this one stopped"

# 2. Every trace.
[ "$(code rm2)" = 0 ];                                        ok $? "the guide's full removal ends with \$RC 0 (got $(code rm2))"
# What the guide promises full removal leaves, read from the listings AROS
# wrote at that moment: no .pkg, no C/Pkg, greet's file still there.
every_trace() {  # every_trace <root listing> <C listing>
    ! has "$O/$1" '^\.pkg' && ! has "$O/$2" '^Pkg' && has "$O/$2" '^Greet'
}
every_trace s2r.txt s2c.txt
ok $? ".pkg and C/Pkg are gone and greet's file is still on the disk"
[ "$(code i3)" = 0 ];                                         ok $? "Pkg installs again into a root it knows nothing about"
[ "$(code g2)" = 0 ] && has "$O/g2.o" '^adopted: 1$' && has "$O/g2.o" '^files: 0$'
ok $? "greet installed again adopts the file already there and places none"
[ "$(code v2)" = 0 ] && has "$O/v2.o" '^result: intact$'
ok $? "and VERIFY finds the adopted root intact"
has "$O/rm3.o" 'hint:.*still holds'
ok $? "REMOVE pkg says what .pkg still holds and points at the guide"

# Negative control: step 2 without its Delete line, on r2.
[ "$(code rm3)" = 0 ];                                        ok $? "control: REMOVE pkg alone ran on r2 (\$RC $(code rm3))"
echo "  negative control: the same check on a root where the Delete line was skipped"
if every_trace s3r.txt s3c.txt; then
    echo "  FAIL control: r2 passed the every-trace check although .pkg was never deleted"
    checks=$((checks + 1)); fails=$((fails + 1))
else
    echo "  ok   control: r2 fails it; its listing still holds:"
    grep '^\.pkg' "$O/s3r.txt" | sed 's/^/         /'
    checks=$((checks + 1))
fi

echo
echo "aros-remove-pkg: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
