#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The catalogue fields at publish: the keywords, an Aminet .readme, what a
# new version takes from the last one (everything but Changes), NONE, the
# refusals, and SHOW reading them back.

set -u
PKG=${PKG:-./build/pkg}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-catalogue.XXXXXX")
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }
man() { f=$(awk -v v="$1" '$1=="tool" && $2==v {print $4}' ch/index); cat "ch/objects/$f.manifest"; }

$PKG KEYGEN FILE key > /dev/null
export PKG_SIGNKEY="$T/key"
mkdir -p d/C d/Docs
printf 'x\000$VER: tool 1.0 (1.1.2026)\000' > d/C/Tool
printf 'png' > d/Docs/shot.png
printf 'Tool does things.\n\n\n\tIndented, and    \nkept in one paragraph.\n' > desc.txt
printf 'First version.\n' > changes.txt
# an Aminet readme, in Latin-1 as many are
printf 'Short:        Does things well\nAuthor:       Jane Roe, Joe Bloggs\nType:         util/misc\nVersion:      1.0\nArchitecture: m68k-amigaos\n\nThe readme text, caf\351.\n' > tool.readme

echo "keywords"
$PKG PUBLISH d CHANNEL ch KIND application SHORT "A tool that does things" DESCRIPTION desc.txt \
    CATEGORY util/misc TAGS "Rexx, cli" AUTHOR "Jane Roe, Joe Bloggs" HOMEPAGE https://example.org/tool \
    REPOSITORY https://example.org/tool.git LICENSE MIT DISTRIBUTION open-source CHANGES changes.txt \
    ICON Docs/shot.png SCREENSHOT Docs/shot.png MACHINE > o1 2>&1
[ $? -eq 0 ];                                         ok $? "PUBLISH takes every catalogue keyword"
man 1.0 > m1
has m1 '^Short: A tool that does things$' && has m1 '^Category: util/misc$' && has m1 '^Tags: rexx, cli$' \
  && has m1 '^Author: Jane Roe$' && has m1 '^Author: Joe Bloggs$' && has m1 '^License: MIT$' \
  && has m1 '^Distribution: open-source$' && has m1 '^Homepage: https://example.org/tool$' \
  && has m1 '^Repository: https://example.org/tool.git$' && has m1 '^Changes: First version.$' \
  && has m1 '^Icon: Docs/shot.png$' && has m1 '^Screenshot: Docs/shot.png$'
                                                      ok $? "and signs each into the manifest, tags in lower case, one line per author"
printf 'Description: Tool does things.\nDescription: \nDescription: Indented, and\nDescription: kept in one paragraph.\n' > want
grep '^Description: ' m1 | cmp -s - want
                                                      ok $? "a description file becomes one line each, blank runs one paragraph break, tabs and trailing spaces gone"

echo "inherit"
printf 'x\000$VER: tool 1.1 (1.1.2026)\000' > d/C/Tool
$PKG PUBLISH d CHANNEL ch MACHINE > o2 2>&1
[ $? -eq 0 ] && has o2 '^about-from: tool 1.0$';     ok $? "a new version takes the catalogue fields of the last one, and says so"
man 1.1 > m2
has m2 '^Short: A tool that does things$' && has m2 '^License: MIT$' && ! has m2 '^Changes:'
                                                      ok $? "everything but Changes, which belong to one version"
printf 'x\000$VER: tool 1.2 (1.1.2026)\000' > d/C/Tool
$PKG PUBLISH d CHANNEL ch HOMEPAGE none SCREENSHOT none LICENSE GPL-2.0-or-later > /dev/null 2>&1
man 1.2 > m3
! has m3 '^Homepage:' && ! has m3 '^Screenshot:' && has m3 '^License: GPL-2.0-or-later$' && has m3 '^Icon: Docs/shot.png$'
                                                      ok $? "NONE drops an inherited field; a keyword replaces one"

echo "readme"
$PKG PUBLISH d CHANNEL ch2 KIND application README tool.readme MACHINE > o4 2>&1
f=$(awk '{print $4}' ch2/index); cp "ch2/objects/$f.manifest" m4
[ $? -eq 0 ] && has m4 '^Short: Does things well$' && has m4 '^Category: util/misc$' \
  && has m4 '^Author: Joe Bloggs$' && grep -q "^Description: The readme text, caf$(printf '\303\251').$" m4
                                                      ok $? "an Aminet readme gives Short, Author, Type and its text, Latin-1 made UTF-8"
$PKG PUBLISH d CHANNEL ch3 KIND application README tool.readme SHORT "Mine" > /dev/null 2>&1
f=$(awk '{print $4}' ch3/index)
grep -q '^Short: Mine$' "ch3/objects/$f.manifest";   ok $? "a keyword wins over the readme"

echo "refusals"
refused() {  # refused <what> <pattern> <keyword words...>
    what=$1 pat=$2; shift 2
    $PKG PUBLISH d CHANNEL bad KIND application "$@" MACHINE > o5 2>&1
    [ $? -eq 20 ] && has o5 "$pat" && [ ! -e bad/index ]
    ok $? "$what"
}
refused "a Short over 40 characters is refused, with the limit" 'takes 40 at most' SHORT "This short description is much longer than forty"
refused "an unknown category is refused, listing the types" 'the types are biz' CATEGORY games/action
refused "a tag with a space is refused" 'TAGS' TAGS "two words"
refused "17 tags are refused" '16 at most' TAGS "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q"
refused "a homepage that is not http is refused" 'HOMEPAGE is an http' HOMEPAGE ftp://example.org
refused "a licence that is no SPDX expression is refused" 'LICENSE' LICENSE "MIT; rm -rf"
refused "an unknown distribution is refused, with the list" 'open-source, freeware' DISTRIBUTION free
refused "an icon that is no file of the package is refused" 'no file of this package' ICON Docs/none.png
$PKG PUBLISH d CHANNEL bad KIND application DESCRIPTION nowhere.txt MACHINE > o6 2>&1
[ $? -eq 11 ] && has o6 'DESCRIPTION names nowhere.txt';  ok $? "a description file that cannot be read is refused with 11"

echo "show"
$PKG SHOW tool CHANNEL ch MACHINE > o7 2>&1
[ $? -eq 0 ] && has o7 '^short: A tool that does things$' && has o7 '^license: GPL-2.0-or-later$' \
  && has o7 '^tag: rexx$' && ! has o7 '^homepage:'
                                                      ok $? "SHOW <name> gives the newest version's fields as records"
$PKG SHOW tool CHANNEL ch > o8 2>&1
has o8 'tool 1.2: A tool that does things' && has o8 'Indented, and'
                                                      ok $? "and as text for a person"

echo
echo "catalogue: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
