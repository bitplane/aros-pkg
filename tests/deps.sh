#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Dependencies, orphans and the image route, on the host. [PKG2] in the
# planning repository asks for five things, each checked here:
#   1. dependencies go in before what needs them;
#   2. a version conflict is reported and refused;
#   3. a cycle is reported, and the run ends;
#   4. removal refuses while something depends on the package, and says what;
#   5. a dependency that does not exist fails the whole install, with nothing
#      partially applied.
# Beyond it: a badly signed dependency refuses the install, a failure while
# placing takes the dependencies already placed back out, orphans are
# reported by REMOVE and taken out by REMOVE ORPHANS only, and an application
# travels as one FFS image whose bytes equal what `pkg IMAGE` writes alone.
#
# "Nothing applied" is checked by listing every file under the root.

set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-deps.XXXXXX")
trap '[ "${KEEP:-0}" = 1 ] || rm -rf "$T"' EXIT

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }
files_in() { (cd "$1" 2>/dev/null && find . -type f ! -path './.pkg/keys/*' | sort); }

$PKG KEYGEN FILE "$T/dev.key" > /dev/null
$PKG KEYGEN FILE "$T/other.key" > /dev/null
export PKG_SIGNKEY="$T/dev.key"
CH="$T/channel"

pub() {  # pub <name> <version> <kind> <depends|-> [file content...]
    n=$1 v=$2 k=$3 d=$4
    shift 4
    rm -rf "$T/src"
    mkdir -p "$T/src/C" "$T/src/Libs"
    case $k in
        library) printf 'lib %s %s\n' "$n" "$v" > "$T/src/Libs/$n.library" ;;
        *)       printf 'binary %s %s\n' "$n" "$v" > "$T/src/C/$n"
                 printf 'docs for %s\n' "$n" > "$T/src/ReadMe" ;;
    esac
    if [ "$d" = - ]; then
        $PKG PUBLISH "$T/src" CHANNEL "$CH" NAME "$n" VERSION "$v" KIND "$k" "$@"
    else
        $PKG PUBLISH "$T/src" CHANNEL "$CH" NAME "$n" VERSION "$v" KIND "$k" DEPENDS "$d" "$@"
    fi
}

echo "publish"
pub base 1.0 library - > /dev/null;                     ok $? "publish base 1.0"
pub mid 1.0 library "base >= 1.0" > /dev/null;          ok $? "publish mid, which needs base"
pub app 1 image "mid, base>=1.0" > "$T/pa" 2>&1;        ok $? "publish app as an image needing mid and base"
m=$(awk '$1=="app" && $2=="1"{print $3}' "$CH/index")
M="$CH/objects/$m.manifest"
has "$M" '^Kind: image$';                               ok $? "the manifest says image"
has "$M" '^Depends: base >= 1.0$' && has "$M" '^Depends: mid$'
                                                        ok $? "and carries both Depends, normalised and sorted"
[ "$(grep -c '^File: ' "$M")" = 1 ] && has "$M" ' app\.hdf$'
                                                        ok $? "the package holds one file, app.hdf"
pub self 1 application "self" > "$T/self" 2>&1
[ $? -eq 20 ] && has "$T/self" 'itself';                ok $? "a package depending on itself is refused at publish, exit 20"

echo "install_in_order"
R="$T/root"
$PKG INSTALL app ROOT "$R" CHANNEL "$CH" > "$T/in" 2>&1; ok $? "install app"
awk '/added +base/{b=NR} /added +mid/{m=NR} /^installed app/{a=NR} END{exit !(b && m && a && b<m && m<a)}' "$T/in"
                                                        ok $? "[PKG2] 1: base, then mid, then app, in that order"
$PKG LIST ROOT "$R" MACHINE > "$T/l1"
has "$T/l1" '^package: base 1.0 library 1 dependency$' && has "$T/l1" '^package: mid 1.0 library 1 dependency$' \
    && has "$T/l1" '^package: app 1 image 1 explicit$'; ok $? "list marks base and mid as dependencies, app as explicit"
rm -rf "$T/src"; mkdir -p "$T/src/C"; printf 'binary %s %s\n' app 1 > "$T/src/C/app"; printf 'docs for %s\n' app > "$T/src/ReadMe"
$PKG IMAGE "$T/src" OUT "$T/alone.hdf" NAME app > /dev/null
cmp -s "$T/alone.hdf" "$R/app.hdf";                     ok $? "the installed image equals the one pkg IMAGE writes from the same drawer"

echo "removal"
$PKG REMOVE base ROOT "$R" > "$T/rb" 2>&1
[ $? -eq 16 ] && has "$T/rb" 'needed by app 1, mid 1.0'
                                                        ok $? "[PKG2] 4: removing base refused with 16, naming both that need it"
[ -f "$R/Libs/base.library" ];                          ok $? "and base is still there"
$PKG REMOVE app ROOT "$R" MACHINE > "$T/ra" 2>&1;       ok $? "remove app"
has "$T/ra" '^orphan: mid 1.0$';                        ok $? "remove reports mid as an orphan"
[ -f "$R/Libs/mid.library" ];                           ok $? "and leaves it: taking orphans out is a separate act"
$PKG REMOVE ORPHANS ROOT "$R" MACHINE > "$T/ro" 2>&1;   ok $? "remove orphans"
has "$T/ro" '^package: mid 1.0$' && has "$T/ro" '^package: base 1.0$' && has "$T/ro" '^count: 2$'
                                                        ok $? "takes out mid, then base, which mid alone needed"
[ -z "$(files_in "$R" | grep -v '^./.pkg/prev/')" ];    ok $? "the root holds nothing but its pinned keys"
$PKG REMOVE ORPHANS ROOT "$R" > "$T/ro2" 2>&1
has "$T/ro2" 'no orphans';                              ok $? "a second pass finds none"

echo "no_false_orphans"
pub loner 1 application - > /dev/null
$PKG INSTALL app ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG INSTALL loner ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG REMOVE loner ROOT "$R" MACHINE > "$T/rl" 2>&1;     ok $? "remove a package nothing else relates to"
! has "$T/rl" '^orphan:';                               ok $? "reports no orphan while app still needs mid and base"
$PKG REMOVE ORPHANS ROOT "$R" MACHINE > "$T/ro3" 2>&1
has "$T/ro3" '^count: 0$' && [ -f "$R/Libs/mid.library" ]; ok $? "and REMOVE ORPHANS takes nothing out"
$PKG REMOVE app ROOT "$R" > /dev/null 2>&1
$PKG REMOVE ORPHANS ROOT "$R" > /dev/null 2>&1

echo "kept_for_itself"
$PKG INSTALL app ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG INSTALL mid ROOT "$R" CHANNEL "$CH" > "$T/k" 2>&1; ok $? "installing mid by name after it came as a dependency"
has "$T/k" 'now kept for itself';                       ok $? "keeps it for itself"
$PKG REMOVE app ROOT "$R" MACHINE > "$T/ra2" 2>&1
! has "$T/ra2" '^orphan: mid';                          ok $? "so removing app does not call mid an orphan"
$PKG REMOVE mid ROOT "$R" > /dev/null 2>&1
$PKG REMOVE ORPHANS ROOT "$R" > /dev/null 2>&1
[ -z "$(files_in "$R" | grep -v '^./.pkg/prev/')" ];    ok $? "and the root is empty again"

echo "refusals_leave_nothing"
pub ghost 1 application "nowhere" > /dev/null
before=$(files_in "$R")
$PKG INSTALL ghost ROOT "$R" CHANNEL "$CH" > "$T/g" 2>&1
[ $? -eq 16 ] && has "$T/g" 'depends on nowhere';       ok $? "[PKG2] 5: a missing dependency refuses with 16, naming it"
[ "$(files_in "$R")" = "$before" ];                     ok $? "and nothing under the root changed"

pub needs2 1 application "base >= 2.0" > /dev/null
$PKG INSTALL needs2 ROOT "$R" CHANNEL "$CH" > "$T/c1" 2>&1
[ $? -eq 16 ] && has "$T/c1" 'highest the channel offers is 1.0'
                                                        ok $? "[PKG2] 2: a version the channel cannot meet refused with 16"
$PKG INSTALL base ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
pub base 2.0 library - > /dev/null
$PKG INSTALL needs2 ROOT "$R" CHANNEL "$CH" > "$T/c2" 2>&1
[ $? -eq 16 ] && has "$T/c2" 'UPGRADE base first';      ok $? "an installed version too old refused with 16, naming the way out"
$PKG UPGRADE base ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG INSTALL needs2 ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
                                                        ok $? "after UPGRADE base, the same install goes through"
$PKG REMOVE needs2 ROOT "$R" > /dev/null 2>&1
$PKG REMOVE base ROOT "$R" > /dev/null 2>&1

pub cyc-a 1 application "cyc-b" > /dev/null
pub cyc-b 1 application "cyc-a" > /dev/null
before=$(files_in "$R")
$PKG INSTALL cyc-a ROOT "$R" CHANNEL "$CH" > "$T/cy" 2>&1
[ $? -eq 16 ] && has "$T/cy" 'cycle: cyc-a -> cyc-b -> cyc-a'
                                                        ok $? "[PKG2] 3: a cycle reported with its path, and the run ends"
[ "$(files_in "$R")" = "$before" ];                     ok $? "nothing applied"

# A dependency with a damaged signature: the whole install is refused.
pub sigdep 1 library - > /dev/null
pub usessig 1 application "sigdep" > /dev/null
d=$(awk '$1=="sigdep"{print $3}' "$CH/index")
cp "$CH/objects/$d.sig" "$T/good.sig"
python3 -c "
import sys
p=sys.argv[1]; s=open(p).read(); i=s.index('Signature: ')+11
c='0' if s[i]!='0' else '1'; open(p,'w').write(s[:i]+c+s[i+1:])
" "$CH/objects/$d.sig"
$PKG INSTALL usessig ROOT "$R" CHANNEL "$CH" > "$T/bs" 2>&1
[ $? -eq 13 ];                                          ok $? "a badly signed dependency refuses the install with 13"
[ "$(files_in "$R")" = "$before" ];                     ok $? "and neither it nor the application was placed"
cp "$T/good.sig" "$CH/objects/$d.sig"

# A dependency signed by another key than the one pinned for it.
$PKG INSTALL sigdep ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG REMOVE sigdep ROOT "$R" > /dev/null 2>&1
rm -rf "$T/src"; mkdir -p "$T/src/Libs"; printf 'swapped\n' > "$T/src/Libs/sigdep.library"
PKG_SIGNKEY="$T/other.key" $PKG PUBLISH "$T/src" CHANNEL "$CH" NAME sigdep VERSION 2 KIND library > /dev/null
before=$(files_in "$R")
$PKG INSTALL usessig ROOT "$R" CHANNEL "$CH" > "$T/bk" 2>&1
[ $? -eq 14 ];                                          ok $? "a dependency signed by another key than the pinned one refused with 14"
[ "$(files_in "$R")" = "$before" ];                     ok $? "and nothing was placed"

# A failure while placing takes the dependencies placed so far back out.
pub clash 1 application "base" > /dev/null
mkdir -p "$R/C"; printf 'mine\n' > "$R/C/clash"
before=$(files_in "$R")
$PKG INSTALL clash ROOT "$R" CHANNEL "$CH" > "$T/cl" 2>&1
[ $? -eq 15 ];                                          ok $? "the application clashing with a file of the person's refused with 15"
[ "$(files_in "$R")" = "$before" ] && [ "$(cat "$R/C/clash")" = mine ]
                                                        ok $? "base, placed first, was taken back out; the person's file is untouched"
rm -rf "$R/C"

echo "upgrade_brings_new_dependencies"
pub extra 1.0 library - > /dev/null
pub app 2 image "extra, mid" > /dev/null
$PKG INSTALL app VERSION 1 ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG UPGRADE app ROOT "$R" CHANNEL "$CH" MACHINE > "$T/up" 2>&1
                                                        ok $? "upgrade app to 2"
has "$T/up" '^dependency: extra 1.0$' && has "$T/up" '^result: upgraded$'
                                                        ok $? "brings extra in as a dependency"
$PKG ROLLBACK app ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
                                                        ok $? "rollback to 1"
$PKG REMOVE app ROOT "$R" MACHINE > "$T/ra3" 2>&1
has "$T/ra3" '^orphan: extra 1.0$';                     ok $? "extra, which only app 2 needed, is an orphan after removal"

echo
echo "$checks checks, $fails failures"
[ "$fails" -eq 0 ]
