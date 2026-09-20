#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The list of channels a root keeps, and what reads it.
#
# CHANNEL ADD, LIST and REMOVE write and read <root>/.pkg/channels; the file
# itself is the oracle, read with cat, so the order and the one-per-line
# shape are checked outside the code. A command without CHANNEL then has to
# behave exactly as the same command with CHANNEL <that one> does, and an
# explicit CHANNEL has to ignore the list entirely.
#
# The safety rule is the point of the rest: two listed channels offering the
# same package name under different keys is refused (14), naming both
# channels and both keys, and the control is that pinning the key first (by
# installing from one channel with CHANNEL) makes the refusal disappear and
# the pinned channel win. There is no env var to switch the check off and
# this test does not invent one.

set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-channels.XXXXXX")
trap '[ "${KEEP:-0}" = 1 ] || rm -rf "$T"' EXIT

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

$PKG KEYGEN FILE "$T/a.key" > /dev/null
$PKG KEYGEN FILE "$T/b.key" > /dev/null
APUB=$(awk '/^Public:/{print $2}' "$T/a.key")
BPUB=$(awk '/^Public:/{print $2}' "$T/b.key")

pub() {  # pub <channel> <key> <name> <version> [more words...]
    c=$1 k=$2 n=$3 v=$4
    shift 4
    rm -rf "$T/src"
    mkdir -p "$T/src/Libs"
    printf 'lib %s %s from %s\n' "$n" "$v" "$c" > "$T/src/Libs/$n.library"
    $PKG PUBLISH "$T/src" CHANNEL "$c" SIGN "$k" NAME "$n" VERSION "$v" KIND library "$@" 2>/dev/null
}

# ---- the list itself ---------------------------------------------------
R=$T/root
mkdir -p "$R"
mkdir -p "$T/one" "$T/two"
pub "$T/one" "$T/a.key" hello 1.0 > /dev/null
pub "$T/two" "$T/a.key" hello 2.0 > /dev/null

$PKG CHANNEL LIST ROOT "$R" > "$T/list0" 2>&1
ok $? "CHANNEL LIST on a root with no list succeeds"
has "$T/list0" "lists no channel" || ok 1 "empty list says so"
ok $? "empty list says so"

$PKG CHANNEL ADD "$T/one" ROOT "$R" > "$T/add1" 2>&1
ok $? "CHANNEL ADD"
has "$T/add1" "offers 1 package" || ok 1 "ADD says how many packages the channel offers"
ok $? "ADD says how many packages the channel offers"
$PKG CHANNEL ADD "$T/two" ROOT "$R" > /dev/null 2>&1
ok $? "CHANNEL ADD, second"

cat "$R/.pkg/channels" > "$T/file"
printf '%s\n%s\n' "$T/one" "$T/two" > "$T/want"
cmp -s "$T/file" "$T/want"
ok $? ".pkg/channels holds one channel per line, in the order added"

$PKG CHANNEL ADD "$T/one" ROOT "$R" > "$T/dup" 2>&1
[ $? = 15 ]
ok $? "a duplicate channel is refused with 15"
has "$T/dup" "already lists" || ok 1 "the duplicate refusal says so"
ok $? "the duplicate refusal says so"
cmp -s "$R/.pkg/channels" "$T/want"
ok $? "the refused duplicate changed nothing"

$PKG CHANNEL ADD "$T/nowhere" ROOT "$R" > "$T/bad" 2>&1
[ $? = 11 ]
ok $? "a channel that cannot be read is refused with 11"

$PKG CHANNEL LIST ROOT "$R" MACHINE > "$T/listm" 2>&1
ok $? "CHANNEL LIST MACHINE"
[ "$(awk '/^count:/{print $2}' "$T/listm")" = 2 ]
ok $? "CHANNEL LIST MACHINE gives count 2"

# ---- a command with no CHANNEL uses the list ---------------------------
# Two channels, the same key, hello 1.0 in the first and 2.0 in the second:
# the newest wins whatever the order.
$PKG INSTALL hello ROOT "$R" > "$T/i1" 2>&1
ok $? "INSTALL with no CHANNEL uses the list"
has "$T/i1" "hello 2.0" || ok 1 "the newest version across the channels wins"
ok $? "the newest version across the channels wins"
grep -q "/two$" "$R/Libs/hello.library"
ok $? "the file installed is the one in the second channel"

# an explicit CHANNEL means that channel alone
R2=$T/root2
mkdir -p "$R2"
$PKG CHANNEL ADD "$T/two" ROOT "$R2" > /dev/null 2>&1
$PKG INSTALL hello ROOT "$R2" CHANNEL "$T/one" > "$T/i2" 2>&1
ok $? "INSTALL with an explicit CHANNEL"
has "$T/i2" "hello 1.0" || ok 1 "an explicit CHANNEL overrides the list"
ok $? "an explicit CHANNEL overrides the list"

# order breaks a tie: the same version in both channels
R3=$T/root3
mkdir -p "$R3" "$T/tieA" "$T/tieB"
pub "$T/tieA" "$T/a.key" tied 1.0 > /dev/null
pub "$T/tieB" "$T/a.key" tied 1.0 > /dev/null
$PKG CHANNEL ADD "$T/tieA" ROOT "$R3" > /dev/null 2>&1
$PKG CHANNEL ADD "$T/tieB" ROOT "$R3" > /dev/null 2>&1
$PKG INSTALL tied ROOT "$R3" > /dev/null 2>&1
ok $? "INSTALL of a package both channels offer at the same version"
grep -q "/tieA$" "$R3/Libs/tied.library"
ok $? "list order breaks the tie: the first channel wins"

# ---- the two-keys refusal ----------------------------------------------
R4=$T/root4
mkdir -p "$R4" "$T/mine" "$T/theirs"
pub "$T/mine"   "$T/a.key" tool 1.0 > /dev/null
pub "$T/theirs" "$T/b.key" tool 9.0 > /dev/null
$PKG CHANNEL ADD "$T/mine" ROOT "$R4" > /dev/null 2>&1
$PKG CHANNEL ADD "$T/theirs" ROOT "$R4" > /dev/null 2>&1
$PKG INSTALL tool ROOT "$R4" > "$T/clash" 2>&1
[ $? = 14 ]
ok $? "two channels, two keys, no pin: refused with 14"
has "$T/clash" "$T/mine" || ok 1 "the refusal names the first channel"
ok $? "the refusal names the first channel"
has "$T/clash" "$T/theirs" || ok 1 "the refusal names the second channel"
ok $? "the refusal names the second channel"
has "$T/clash" "$APUB" || ok 1 "the refusal names the first key"
ok $? "the refusal names the first key"
has "$T/clash" "$BPUB" || ok 1 "the refusal names the second key"
ok $? "the refusal names the second key"
[ ! -f "$R4/Libs/tool.library" ]
ok $? "nothing was installed"
$PKG INSTALL tool ROOT "$R4" MACHINE > "$T/clashm" 2>&1
[ "$(awk '/^next:/{print $2}' "$T/clashm")" = ask-requester ]
ok $? "the refusal's next is ask-requester"

# the control: pin a key first, and the refusal goes
$PKG INSTALL tool ROOT "$R4" CHANNEL "$T/mine" > "$T/pinned" 2>&1
ok $? "installing from one channel alone pins its key"
$PKG UPGRADE tool ROOT "$R4" > "$T/after" 2>&1
ok $? "with a key pinned, the list no longer refuses"
has "$T/after" "already at 1.0" || ok 1 "the pinned key filters the other channel out"
ok $? "the pinned key filters the other channel out"
$PKG STATUS tool ROOT "$R4" MACHINE > "$T/stat4" 2>&1
grep -q "^package: tool 1.0 1.0 current" "$T/stat4"
ok $? "STATUS agrees: current, not upgradable to the other key's 9.0"

# ---- dependencies across the list --------------------------------------
R5=$T/root5
mkdir -p "$R5" "$T/apps" "$T/libs"
pub "$T/libs" "$T/a.key" base 1.0 > /dev/null
pub "$T/apps" "$T/a.key" app 1.0 DEPENDS "base >= 1.0" > /dev/null
$PKG CHANNEL ADD "$T/apps" ROOT "$R5" > /dev/null 2>&1
$PKG CHANNEL ADD "$T/libs" ROOT "$R5" > /dev/null 2>&1
$PKG INSTALL app ROOT "$R5" > "$T/dep" 2>&1
ok $? "a dependency is found in the second channel"
[ -f "$R5/Libs/base.library" ]
ok $? "the dependency was installed"

# ---- STATUS and UPGRADE ALL across two channels ------------------------
pub "$T/libs" "$T/a.key" base 2.0 > /dev/null
pub "$T/apps" "$T/a.key" app 2.0 DEPENDS "base >= 2.0" > /dev/null
$PKG STATUS ROOT "$R5" MACHINE > "$T/st5" 2>&1
ok $? "STATUS across two channels"
grep -q "^package: base 1.0 2.0 upgradable $T/libs\$" "$T/st5"
ok $? "STATUS names the channel the newer version comes from"
$PKG UPGRADE ALL ROOT "$R5" MACHINE > "$T/up5" 2>&1
ok $? "UPGRADE ALL across two channels"
grep -q "^package: base 1.0 2.0 $T/libs\$" "$T/up5"
ok $? "UPGRADE ALL names the channel each version comes from"
grep -q "^package: app 1.0 2.0 $T/apps\$" "$T/up5"
ok $? "UPGRADE ALL upgraded the package from the other channel too"

# ---- ROLLBACK finds the channel that holds the installed version -------
$PKG ROLLBACK base ROOT "$R5" > "$T/rb" 2>&1
ok $? "ROLLBACK with no CHANNEL finds the right channel in the list"
has "$T/rb" "from 2.0 to 1.0" || ok 1 "ROLLBACK went back a version"
ok $? "ROLLBACK went back a version"
grep -q "/libs$" "$R5/Libs/base.library"
ok $? "the file put back came from the channel that holds it"

# ---- REMOVE ------------------------------------------------------------
$PKG CHANNEL REMOVE "$T/libs" ROOT "$R5" > "$T/rm" 2>&1
ok $? "CHANNEL REMOVE"
printf '%s\n' "$T/apps" > "$T/want5"
cmp -s "$R5/.pkg/channels" "$T/want5"
ok $? "the removed channel is gone and the order of the rest is kept"
$PKG CHANNEL REMOVE "$T/libs" ROOT "$R5" > "$T/rm2" 2>&1
[ $? = 11 ]
ok $? "removing a channel that is not listed is refused with 11"
[ -f "$R5/Libs/base.library" ]
ok $? "removing a channel removes nothing from the root"

# ---- no channel at all -------------------------------------------------
R6=$T/root6
mkdir -p "$R6"
$PKG INSTALL hello ROOT "$R6" > "$T/none" 2>&1
[ $? = 20 ]
ok $? "no CHANNEL and an empty list is refused with 20"
has "$T/none" "CHANNEL ADD" || ok 1 "the refusal says how to add a channel"
ok $? "the refusal says how to add a channel"

# ---- a listed channel that is gone -------------------------------------
R7=$T/root7
mkdir -p "$R7" "$T/gone"
$PKG CHANNEL ADD "$T/gone" ROOT "$R7" > /dev/null 2>&1
rm -rf "$T/gone"
$PKG STATUS ROOT "$R7" > "$T/lost" 2>&1
[ $? = 11 ]
ok $? "a listed channel that is no longer there is refused with 11"
has "$T/lost" "CHANNEL REMOVE" || ok 1 "the refusal says how to take it off the list"
ok $? "the refusal says how to take it off the list"

echo "channels: $checks checks, $fails failed"
[ "$fails" = 0 ]
