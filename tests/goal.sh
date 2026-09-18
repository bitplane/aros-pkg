#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The goal, as one sequence that passes or fails:
#
#   1. On macOS: the AFS+ handler at two versions, published into a channel that
#      is a directory, signed with a development key.
#   2. On hosted AROS: Pkg bootstraps from a plain archive, with no package
#      manager present, then Install v1, Verify, Upgrade v2, Rollback v1.
#   3. One ARexx script drives all of it through the PKG port and exits
#      non-zero at the first disagreement.
#   4. A tampered payload and a substituted publisher key are both refused,
#      inside that same sequence.
#
# The bootstrap archive is the Pkg binary gzip-compressed, expanded by the
# minigzip AROS ships. Pkg is one self-contained file, so that file compressed
# is its plain archive. AROS's own C:Unpack was the first choice, since it reads
# the container this tool adopted, but on hosted aarch64 AROS it does not load:
# the shell answers "file is not executable" for the shipped binary and for one
# rebuilt from its sources. Recorded as an AROS defect.
#
# Once running, the bootstrapped Pkg installs the "pkg" package from the signed
# channel into the root, and the port is served by that managed copy: from the
# first command on, the package manager manages itself. Nothing is copied into
# the shared hosted tree.
#
#   PKG_HANDLER_V14=<dir> PKG_HANDLER_V15=<dir> sh tests/goal.sh

set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
v14="${PKG_HANDLER_V14:?set PKG_HANDLER_V14 to a revision 14 handler package}"
v15="${PKG_HANDLER_V15:?set PKG_HANDLER_V15 to a revision 15 handler package}"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-goal.XXXXXX")
share="$work/share"
aros_started=0

cleanup() {
    status=$?
    [ "$aros_started" = 0 ] || "$control" stop >/dev/null 2>&1 || true
    if [ "$status" -ne 0 ] && [ "${PKG_KEEP_FAILURE:-0}" = 1 ]; then
        echo "goal: keeping $work" >&2
        return
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

for need in "$host_pkg" "$repo_root/build/aros/Pkg" "$repo_root/build/aros/PkgHandlerRev" \
            "$repo_root/build/aros/rexx" "$v14/afsplus-handler" "$v15/afsplus-handler" \
            "$v14/Unit19" "$v14/AFSPLUS19" "$control"; do
    [ -e "$need" ] || { echo "goal: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "goal: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# ---- 1. macOS ----------------------------------------------------------

echo "goal 1: macOS publishes"
mkdir -p "$share/bin" "$share/images" "$work/d14/L" "$work/d15/L" "$work/d16/L" "$work/boot/C"
cp "$v14/afsplus-handler" "$work/d14/L/afsplus-handler"
cp "$v15/afsplus-handler" "$work/d15/L/afsplus-handler"
cp "$v15/afsplus-handler" "$work/d16/L/afsplus-handler"
printf 'attacker bytes\n' >> "$work/d16/L/afsplus-handler"

"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
"$host_pkg" KEYGEN FILE "$work/attacker.key" > /dev/null
PUB=$(awk '/^Public:/{print $2}' "$work/dev.key")
for v in 14 15; do
    PKG_SIGNKEY="$work/dev.key" "$host_pkg" PUBLISH "$work/d$v" CHANNEL "$share/channel" \
        NAME afsplus-handler VERSION "$v" KIND device > /dev/null
                                                      ok $? "publish afsplus-handler $v, signed with the development key"
done
ATT=$(awk '/^Public:/{print $2}' "$work/attacker.key")
PKG_SIGNKEY="$work/attacker.key" "$host_pkg" PUBLISH "$work/d16" CHANNEL "$share/channel" ACCEPTKEY "$ATT" \
    NAME afsplus-handler VERSION 16 KIND device > /dev/null
                                                      ok $? "an attacker publishes 16 into the same channel with another key"

cp -R "$share/channel" "$share/tampered"
m15=$(awk '$1=="afsplus-handler" && $2=="15"{print $4}' "$share/tampered/index")
p15=$(awk '/^Payload:/{print $2}' "$share/tampered/objects/$m15.manifest")
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=1; open(p,'wb').write(b)
" "$share/tampered/objects/$p15.pkg"

# The bootstrap: the binary, gzip-compressed. And Pkg as an ordinary signed
# package in the channel, which the bootstrapped copy installs.
gzip -c "$repo_root/build/aros/Pkg" > "$share/Pkg.gz"
                                                      ok $? "the bootstrap archive is the one file, gzip-compressed"
cp "$repo_root/build/aros/Pkg" "$work/boot/C/Pkg"
PKG_SIGNKEY="$work/dev.key" "$host_pkg" PUBLISH "$work/boot" CHANNEL "$share/channel" \
    NAME pkg VERSION 1 KIND application > /dev/null
                                                      ok $? "Pkg itself is published as a signed package"

cp "$repo_root/build/aros/rexx" "$share/bin/rexx"
cp "$repo_root/build/aros/PkgHandlerRev" "$share/bin/PkgHandlerRev"
cp "$repo_root/tests/goal.rexx" "$share/goal.rexx"
cp "$v14/Unit19" "$share/images/Unit19"
sed 's|^FileSystem *=.*|FileSystem      = MacRW:sys/L/afsplus-handler|' \
    "$v14/AFSPLUS19" > "$share/AFSPKG19"

# ---- 2 to 4. hosted AROS, one boot -------------------------------------

echo "goal 2-4: hosted AROS, one boot, one ARexx script"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA='C:FailAt 21
C:MakeDir RAM:boot
C:Copy MacRW:Pkg.gz RAM:boot/Pkg.gz
C:minigzip -d RAM:boot/Pkg.gz
C:List RAM:boot >MacRW:boot-list.out
RAM:boot/Pkg INSTALL pkg ROOT MacRW:sys CHANNEL MacRW:channel >MacRW:self-install.out
Run >NIL: MacRW:sys/C/Pkg PORT
MacRW:bin/rexx MacRW:goal.rexx >MacRW:goal.out
If ERROR
    C:Echo fail >MacRW:goal.rc
Else
    C:Echo pass >MacRW:goal.rc
EndIf
Assign "FDSK:" "MacRW:images"
C:Mount MacRW:AFSPKG19 >MacRW:mount.out
MacRW:bin/PkgHandlerRev AFSPKG19: >MacRW:rev.out
C:Echo done >MacRW:done' "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 180 ]; do sleep 1; w=$((w + 1)); done
"$control" stop > /dev/null 2>&1 || true
aros_started=0
[ -f "$share/done" ];                                 ok $? "the boot ran its startup to the end"

has "$share/boot-list.out" '^Pkg ';                   ok $? "minigzip bootstrapped Pkg into RAM: from the plain archive"
has "$share/self-install.out" 'installed pkg 1';      ok $? "the bootstrapped Pkg installed itself as a signed package"
cmp -s "$repo_root/build/aros/Pkg" "$share/sys/C/Pkg"; ok $? "the managed copy is byte-identical to the build"
[ "$(cat "$share/goal.rc" 2>/dev/null)" = pass ];     ok $? "the ARexx script returned success to AmigaDOS"
has "$share/goal.out" '^port PKG is open';            ok $? "the PKG ARexx port opened"
has "$share/goal.out" '^ok install 14';               ok $? "ARexx: install 14"
has "$share/goal.out" '^ok verify 14';                ok $? "ARexx: verify"
has "$share/goal.out" '^ok upgrade to 15';            ok $? "ARexx: upgrade to 15"
has "$share/goal.out" '^ok rollback';                 ok $? "ARexx: rollback to 14"
has "$share/goal.out" '^ok a tampered payload refused'; ok $? "ARexx: the tampered payload refused, for that reason"
has "$share/goal.out" '^ok a substituted publisher key refused'; ok $? "ARexx: the substituted key refused, for that reason"
has "$share/goal.out" '^GOAL PASS';                   ok $? "ARexx: GOAL PASS"

cmp -s "$work/d14/L/afsplus-handler" "$share/sys/L/afsplus-handler"; ok $? "on disk, after rollback: the 14 bytes, compared on the host"
[ ! -e "$share/other/L/afsplus-handler" ];            ok $? "nothing installed from the tampered channel"
[ "$(head -c 64 "$share/sys/.pkg/keys/afsplus-handler")" = "$PUB" ]; ok $? "the pinned key is still the development key"
has "$share/rev.out" '^revision 14$';                 ok $? "AROS mounts the volume with the handler Pkg left in place: revision 14"

cp "$share/goal.out" "$work/goal-main.out" 2>/dev/null
cp "$share/rev.out" "$work/rev-main.out" 2>/dev/null

# ---- the control: the same sequence must be able to fail --------------

echo "goal control: a sabotaged expectation must stop the script with an error"
rm -rf "$share/sys" "$share/other" "$share"/*.out "$share"/*.rc "$share/done"
cp "$v14/Unit19" "$share/images/Unit19"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA='C:FailAt 21
C:MakeDir RAM:boot
C:Copy MacRW:Pkg.gz RAM:boot/Pkg.gz
C:minigzip -d RAM:boot/Pkg.gz
RAM:boot/Pkg INSTALL pkg ROOT MacRW:sys CHANNEL MacRW:channel >MacRW:self-install.out
Run >NIL: MacRW:sys/C/Pkg PORT
MacRW:bin/rexx MacRW:goal.rexx SABOTAGE >MacRW:goal.out
If ERROR
    C:Echo fail >MacRW:goal.rc
Else
    C:Echo pass >MacRW:goal.rc
EndIf
C:Echo done >MacRW:done' "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 180 ]; do sleep 1; w=$((w + 1)); done
"$control" stop > /dev/null 2>&1 || true
aros_started=0
[ "$(cat "$share/goal.rc" 2>/dev/null)" = fail ];     ok $? "control: the sabotaged run returns an error to AmigaDOS"
has "$share/goal.out" 'GOAL FAIL: expected afsplus-handler 13'; ok $? "control: and stops at that line, saying why"
! has "$share/goal.out" '^ok upgrade to 15';          ok $? "control: nothing after the first disagreement ran"

echo
echo "--- the ARexx script's own report, main run"
cat "$work/goal-main.out" 2>/dev/null
echo "--- the handler serving the volume afterwards"
cat "$work/rev-main.out" 2>/dev/null
echo "--- the control run's report"
cat "$share/goal.out" 2>/dev/null
echo
echo "goal: $checks checks, $fails failures"
if [ "$fails" -ne 0 ]; then
    for f in "$share"/*.out "$share"/*.rc; do
        [ -f "$f" ] && { echo "--- $(basename "$f")"; cat "$f"; }
    done
fi
[ "$fails" -eq 0 ]
