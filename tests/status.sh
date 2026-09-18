#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Keeping a root current, unattended: STATUS and UPGRADE ALL on the host.
#
# STATUS gives each installed package one state (current, upgradable,
# withdrawn, not-offered, edited) and must choose as UPGRADE chooses: the
# `available` version is compared with what `UPGRADE <name> DRYRUN` answers,
# a newer version withdrawn by its publisher is skipped, and a version
# published for another CPU is not offered. UPGRADE ALL upgrades in
# dependency order (names chosen so that name order is the wrong order),
# never downgrades, stops at the first refusal with nothing further changed
# and says what it had done, and under DRYRUN changes nothing at all, the
# root's .pkg included.
#
# Oracles outside the code: the installed bytes are read back with cat, a
# root's whole content is compared through shasum before and after, a
# channel entry is taken out of the index with grep, and whether stdin was
# read is seen from the offset of a file descriptor the shell shares with
# the command.

set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-status.XXXXXX")
trap '[ "${KEEP:-0}" = 1 ] || rm -rf "$T"' EXIT

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }
snap() { (cd "$1" && find . -type f -exec shasum -a 256 {} + | sort); }

$PKG KEYGEN FILE "$T/dev.key" > /dev/null
$PKG KEYGEN FILE "$T/other.key" > /dev/null
OTHERPUB=$(awk '/^Public:/{print $2}' "$T/other.key")
export PKG_SIGNKEY="$T/dev.key"

pub() {  # pub <channel> <name> <version> <depends|-> [more words...]
    c=$1 n=$2 v=$3 d=$4
    shift 4
    rm -rf "$T/src"
    mkdir -p "$T/src/Libs"
    printf 'lib %s %s\n' "$n" "$v" > "$T/src/Libs/$n.library"
    if [ "$d" = - ]; then
        $PKG PUBLISH "$T/src" CHANNEL "$c" NAME "$n" VERSION "$v" KIND library "$@"
    else
        $PKG PUBLISH "$T/src" CHANNEL "$c" NAME "$n" VERSION "$v" KIND library DEPENDS "$d" "$@"
    fi
}
elf() {  # elf <file> <e_machine> <tag>: enough of a header for Pkg to tell the CPU
    python3 -c "
import sys
b = bytearray(64); b[0:4] = b'\x7fELF'; b[4] = 2; b[5] = 1; b[18] = int(sys.argv[2])
open(sys.argv[1], 'wb').write(bytes(b) + sys.argv[3].encode())
" "$1" "$2" "$3"
}

echo "status"
CH="$T/ch"
R="$T/root"
pub "$CH" cur 1.0 - > /dev/null
pub "$CH" zlib 1.0 - > /dev/null
pub "$CH" app 1.0 zlib > /dev/null
pub "$CH" wd 1.0 - > /dev/null
pub "$CH" wd 1.1 - > /dev/null
pub "$CH" gone 1.0 - > /dev/null
pub "$CH" ed 1.0 - > /dev/null
for cpu in x86_64:62 aarch64:183; do
    mkdir -p "$T/cpu-${cpu%%:*}/C"
    elf "$T/cpu-${cpu%%:*}/C/Cpu" "${cpu#*:}" "cpu 1.0"
    $PKG PUBLISH "$T/cpu-${cpu%%:*}" CHANNEL "$CH" NAME cpu VERSION 1.0 KIND application > /dev/null
done
$PKG INSTALL cpu ROOT "$R" CHANNEL "$CH" ARCH x86_64 > /dev/null 2>&1; ok $? "install cpu 1.0 for x86_64"
for n in cur app wd gone ed; do
    $PKG INSTALL "$n" ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1;         ok $? "install $n"
done
# Then the channel moves on.
pub "$CH" zlib 2.0 - > /dev/null
pub "$CH" zlib 3.0 - > /dev/null
$PKG WITHDRAW zlib VERSION 3.0 CHANNEL "$CH" > /dev/null;            ok $? "zlib 3.0 published and withdrawn"
pub "$CH" app 2.0 "zlib >= 2.0" > /dev/null;                         ok $? "app 2.0 needs zlib >= 2.0"
$PKG WITHDRAW wd VERSION 1.1 CHANNEL "$CH" > /dev/null;              ok $? "wd 1.1, the installed one, withdrawn"
mkdir -p "$T/cpu2/C"; elf "$T/cpu2/C/Cpu" 183 "cpu 1.1"
$PKG PUBLISH "$T/cpu2" CHANNEL "$CH" NAME cpu VERSION 1.1 KIND application > /dev/null
                                                                     ok $? "cpu 1.1 for aarch64 only"
grep -v '^gone ' "$CH/index" > "$T/ix" && cp "$T/ix" "$CH/index";    ok $? "gone taken out of the index"
printf 'my own settings\n' > "$R/Libs/ed.library"

before=$(snap "$R")
$PKG STATUS ROOT "$R" CHANNEL "$CH" MACHINE > "$T/st" 2> "$T/st.err"
[ $? -eq 0 ];                                                        ok $? "STATUS exits 0 with updates waiting"
[ ! -s "$T/st.err" ];                                                ok $? "and writes nothing to stderr under MACHINE"
has "$T/st" '^result: shown$';                                       ok $? "result shown"
has "$T/st" '^package: cur 1.0 1.0 current$';                        ok $? "current: nothing newer offered"
has "$T/st" '^package: zlib 1.0 2.0 upgradable$';                    ok $? "upgradable, to 2.0: the withdrawn 3.0 is skipped"
has "$T/st" '^package: app 1.0 2.0 upgradable$';                     ok $? "app upgradable to 2.0"
has "$T/st" '^package: wd 1.1 1.0 withdrawn$';                       ok $? "withdrawn: the installed 1.1 was withdrawn, only 1.0 is offered"
has "$T/st" '^package: gone 1.0 - not-offered$';                     ok $? "not-offered: the channel has no version of it"
has "$T/st" '^package: ed 1.0 1.0 edited$';                          ok $? "edited: a file changed since install"
has "$T/st" '^package: cpu 1.0 1.0 current$';                        ok $? "cpu current on an x86_64 root: 1.1 is for aarch64 only"
has "$T/st" '^count: 7$' && has "$T/st" '^upgradable: 2$';           ok $? "count 7, upgradable 2"
[ "$(snap "$R")" = "$before" ];                                      ok $? "STATUS changes nothing in the root"
# app alone cannot be the oracle: UPGRADE app is refused while zlib is 1.0.
for n in zlib cpu; do
    $PKG UPGRADE "$n" ROOT "$R" CHANNEL "$CH" DRYRUN MACHINE > "$T/one" 2>&1
    want=$(awk '/^result: unchanged$/{u=1} /^version:/{v=$2} END{print u ? "same" : v}' "$T/one")
    got=$(awk -v n="$n" '$1=="package:" && $2==n {print ($3==$4) ? "same" : $4}' "$T/st")
    [ -n "$want" ] && [ "$want" = "$got" ];                         ok $? "STATUS offers $n what UPGRADE $n would take ($want)"
done
$PKG STATUS app ROOT "$R" CHANNEL "$CH" MACHINE > "$T/st1"
has "$T/st1" '^package: app 1.0 2.0 upgradable$' && has "$T/st1" '^count: 1$'
                                                                     ok $? "STATUS <name> gives that package alone"
$PKG STATUS ROOT "$R" CHANNEL "$CH" > "$T/sth" 2>&1
[ $? -eq 0 ];                                                        ok $? "STATUS in text exits 0"
has "$T/sth" '^zlib  *1.0  *upgradable to 2.0$' && has "$T/sth" '^gone  *1.0  *no longer offered'
                                                                     ok $? "one readable line per package"
has "$T/sth" '^7 packages in .*, 2 upgradable from ';                ok $? "a summary line"
has "$T/sth" 'hint: UPGRADE ALL ROOT .* CHANNEL ';                   ok $? "and a hint naming UPGRADE ALL"
$PKG STATUS ROOT "$R" CHANNEL "$T/nosuch" MACHINE > "$T/stn" 2>&1
[ $? -eq 11 ] && has "$T/stn" '^class: not-found$';                  ok $? "a channel that is not there is refused (11), not read as offering nothing"

echo "upgrade_all_dryrun"
before=$(snap "$R")
$PKG UPGRADE ALL ROOT "$R" CHANNEL "$CH" DRYRUN MACHINE > "$T/ud" 2> "$T/ud.err"
[ $? -eq 0 ] && has "$T/ud" '^result: would-upgrade$' && has "$T/ud" '^count: 2$'
                                                                     ok $? "DRYRUN: would-upgrade, count 2"
awk '/^package: zlib 1.0 2.0$/{z=NR} /^package: app 1.0 2.0$/{a=NR} END{exit !(z && a && z<a)}' "$T/ud"
                                                                     ok $? "DRYRUN lists zlib before app, which needs zlib >= 2.0"
[ "$(snap "$R")" = "$before" ];                                      ok $? "DRYRUN changes nothing in the root, .pkg included"

echo "upgrade_all"
printf 'y\ny\ny\n' > "$T/answers"
exec 5< "$T/answers"
$PKG UPGRADE ALL ROOT "$R" CHANNEL "$CH" MACHINE > "$T/ua" 2> "$T/ua.err" <&5
[ $? -eq 0 ] && has "$T/ua" '^result: upgraded$' && has "$T/ua" '^count: 2$'
                                                                     ok $? "UPGRADE ALL: result upgraded, count 2, exit 0"
IFS= read -r left <&5
[ "$left" = y ];                                                     ok $? "unattended: stdin never read"
exec 5<&-
[ ! -s "$T/ua.err" ];                                                ok $? "nothing on stderr"
awk '/^package: zlib 1.0 2.0$/{z=NR} /^package: app 1.0 2.0$/{a=NR} END{exit !(z && a && z<a)}' "$T/ua"
                                                                     ok $? "zlib upgraded before app, which depends on it"
[ "$(cat "$R/Libs/zlib.library")" = "lib zlib 2.0" ] && [ "$(cat "$R/Libs/app.library")" = "lib app 2.0" ]
                                                                     ok $? "the new bytes are in place"
[ "$(cat "$R/Libs/wd.library")" = "lib wd 1.1" ];                   ok $? "never a downgrade: wd stays at the withdrawn 1.1"
has "$T/ua" '^note: wd 1.1 was withdrawn by its publisher';          ok $? "and that is reported as a note"
[ "$(cat "$R/Libs/ed.library")" = "my own settings" ];               ok $? "the edited file, in a package with nothing newer, is untouched"
$PKG UPGRADE ALL ROOT "$R" CHANNEL "$CH" MACHINE > "$T/ua2" 2>&1
[ $? -eq 0 ] && has "$T/ua2" '^result: unchanged$' && has "$T/ua2" '^count: 0$'
                                                                     ok $? "nothing upgradable: result unchanged, exit 0"
$PKG UPGRADE ALL ROOT "$R" CHANNEL "$CH" > "$T/ua3" 2>&1
[ $? -eq 0 ] && has "$T/ua3" '^nothing to upgrade in ';              ok $? "and in text says so"

echo "stop_at_first_refusal"
C2="$T/ch2"
R2="$T/root2"
R3="$T/root3"
for n in aa bb cc; do pub "$C2" "$n" 1.0 - > /dev/null; done
for n in aa bb cc; do $PKG INSTALL "$n" ROOT "$R2" CHANNEL "$C2" > /dev/null 2>&1; done
$PKG INSTALL cc ROOT "$R3" CHANNEL "$C2" > /dev/null 2>&1
pub "$C2" aa 2.0 - > /dev/null
(PKG_SIGNKEY="$T/other.key"; export PKG_SIGNKEY; pub "$C2" bb 2.0 - ACCEPTKEY "$OTHERPUB" > /dev/null)
                                                                     ok $? "bb 2.0 signed by another key"
pub "$C2" cc 2.0 - > /dev/null
before=$(snap "$R2")
$PKG UPGRADE ALL ROOT "$R2" CHANNEL "$C2" DRYRUN MACHINE > "$T/pd" 2>&1
[ $? -eq 14 ] && has "$T/pd" '^package: aa 1.0 2.0$' && has "$T/pd" '^partial: no$'
                                                                     ok $? "DRYRUN meets the key change too (14), partial no"
[ "$(snap "$R2")" = "$before" ];                                     ok $? "and changes nothing"
printf 'y\n' > "$T/answers"
exec 5< "$T/answers"
$PKG UPGRADE ALL ROOT "$R2" CHANNEL "$C2" MACHINE > "$T/pr" 2> "$T/pr.err" <&5
rc=$?
IFS= read -r left <&5
exec 5<&-
[ "$rc" -eq 14 ];                                                    ok $? "a key change stops it, exit 14, the refusal's class"
[ "$left" = y ] && [ ! -s "$T/pr.err" ];                             ok $? "no prompt, stdin unread, stderr empty"
has "$T/pr" '^result: refused$' && has "$T/pr" '^class: key$' && has "$T/pr" '^next: ask-requester$'
                                                                     ok $? "the refusal's normal records: class key, next ask-requester"
awk '/^package: aa 1.0 2.0$/{p=NR} /^result: refused$/{r=NR} END{exit !(p && r && p<r)}' "$T/pr"
                                                                     ok $? "aa, done before it, is listed before the refusal"
has "$T/pr" '^upgraded: 1$' && has "$T/pr" '^untouched: 2$' && has "$T/pr" '^partial: yes$'
                                                                     ok $? "upgraded 1, untouched 2, partial yes"
! has "$T/pr" '^package: bb' && ! has "$T/pr" '^package: cc';        ok $? "bb and cc are not reported as done"
[ "$(cat "$R2/Libs/aa.library")" = "lib aa 2.0" ];                   ok $? "aa is at 2.0"
[ "$(cat "$R2/Libs/bb.library")" = "lib bb 1.0" ] && [ "$(cat "$R2/Libs/cc.library")" = "lib cc 1.0" ] \
    && [ "$(head -c 64 "$R2/.pkg/keys/bb")" != "$OTHERPUB" ]
                                                                     ok $? "bb and cc unchanged, bb's key still the first one"
$PKG UPGRADE ALL ROOT "$R2" CHANNEL "$C2" > "$T/prt" 2> "$T/prt.err"
[ $? -eq 14 ] && has "$T/prt.err" '0 of 2 upgrades done before this refusal'
                                                                     ok $? "in text, the partial account goes with the refusal"
$PKG UPGRADE bb ROOT "$R2" CHANNEL "$C2" ACCEPTKEY "$OTHERPUB" > /dev/null 2>&1
                                                                     ok $? "the requester accepts bb's new key, for bb alone"
$PKG UPGRADE ALL ROOT "$R2" CHANNEL "$C2" MACHINE > "$T/pr2" 2>&1
[ $? -eq 0 ] && has "$T/pr2" '^package: cc 1.0 2.0$' && has "$T/pr2" '^count: 1$'
                                                                     ok $? "UPGRADE ALL again goes on from there: cc"

printf 'my cc\n' > "$R3/Libs/cc.library"
$PKG STATUS ROOT "$R3" CHANNEL "$C2" MACHINE > "$T/s3"
has "$T/s3" '^package: cc 1.0 2.0 edited$' && has "$T/s3" '^upgradable: 1$'
                                                                     ok $? "edited with a newer version offered: state edited, counted upgradable"
$PKG UPGRADE ALL ROOT "$R3" CHANNEL "$C2" MACHINE > "$T/pe" 2>&1
[ $? -eq 15 ] && has "$T/pe" '^class: conflict$' && has "$T/pe" '^partial: no$' && has "$T/pe" '^upgraded: 0$'
                                                                     ok $? "an upgrade that would replace an edited file is refused (15), as UPGRADE refuses it"
[ "$(cat "$R3/Libs/cc.library")" = "my cc" ];                        ok $? "and the edit is kept"

echo "usage"
$PKG UPGRADE ALL ROOT "$R" CHANNEL "$CH" VERSION 2.0 MACHINE > "$T/u1" 2>&1
[ $? -eq 20 ];                                                       ok $? "ALL with VERSION is a wrong command (20)"
$PKG UPGRADE ALL ROOT "$R" CHANNEL "$CH" DOWNGRADE MACHINE > "$T/u2" 2>&1
[ $? -eq 20 ];                                                       ok $? "ALL with DOWNGRADE is refused (20): never for everything at once"
$PKG UPGRADE ALL ROOT "$R" CHANNEL "$CH" ACCEPTKEY "$OTHERPUB" MACHINE > "$T/u3" 2>&1
[ $? -eq 20 ];                                                       ok $? "ALL with ACCEPTKEY is refused (20)"
$PKG UPGRADE ALL zlib ROOT "$R" CHANNEL "$CH" MACHINE > "$T/u4" 2>&1
[ $? -eq 20 ];                                                       ok $? "ALL with a name is refused (20)"
$PKG INSTALL all ROOT "$R" CHANNEL "$CH" MACHINE > "$T/u5" 2>&1
[ $? -eq 11 ];                                                       ok $? "for INSTALL, all is a package name, not the switch"

echo
echo "$checks checks, $fails failures"
[ "$fails" -eq 0 ]
