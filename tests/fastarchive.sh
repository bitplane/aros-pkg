#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Reading a few files out of a large bzip2 archive without decompressing it
# whole: the block map. A synthetic archive of several MB of incompressible
# data spans many bzip2 blocks, with members straddling boundaries, a member
# in the last block, a member of no bytes, and a second archive of two
# streams back to back.
#
# The oracles are independent of the map: `tar xjf` and the system's own
# bzip2 unpack the archive, and every byte the map delivers is compared
# against what tar wrote. The installs are compared file for file with cmp.

set -u
export COPYFILE_DISABLE=1   # macOS tar would add AppleDouble "._" members
PKG=${PKG:-./build/pkg}
R=${R:-./build/test_archive}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
R=$(cd "$(dirname "$R")" && pwd)/$(basename "$R")
for need in "$PKG" "$R"; do
    [ -x "$need" ] || { echo "fastarchive: missing $need (make build/pkg build/test_archive)" >&2; exit 69; }
done
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-fastarchive.XXXXXX")
trap 'rm -rf "$T"' EXIT

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

# ---- an archive that spans many bzip2 blocks ---------------------------
mkdir -p "$T/src/Top/Extras/P1" "$T/src/Top/Extras/P2" "$T/src/Top/Extras/P3" "$T/src/Top/Fill"
for n in 1 2 3; do
    dd if=/dev/urandom of="$T/src/Top/Extras/P$n/big" bs=1024 count=1300 2>/dev/null
    printf 'notes for %s\n' "$n" > "$T/src/Top/Extras/P$n/ReadMe"
done
: > "$T/src/Top/Extras/P2/nothing"                      # a member of no bytes
for n in 1 2 3 4; do dd if=/dev/urandom of="$T/src/Top/Fill/f$n" bs=1024 count=900 2>/dev/null; done
printf 'last\n' > "$T/src/Top/zz-last"                  # the final member of the tar
(cd "$T/src" && tar --format=pax -cf "$T/a.tar" Top)
bzip2 -9 -c "$T/a.tar" > "$T/a.tar.bz2"

echo "map"
$R MAP "$T/a.tar.bz2" "$T/a.map";                       ok $? "a walk writes the block map"
[ "$(grep -c '^b ' "$T/a.map")" -ge 5 ];                ok $? "the archive spans at least five bzip2 blocks"
[ "$(grep -c '^f ' "$T/a.map")" = "$(cd "$T/src" && find Top -type f | wc -l | tr -d ' ')" ]
                                                        ok $? "every member of the tar is in the map"
has "$T/a.map" '^f [0-9]* 0 [0-7]* Top/Extras/P2/nothing$'
                                                        ok $? "a member of no bytes is in it too"

echo "read"
for f in Top/Extras/P1/big Top/Extras/P2/big Top/Extras/P3/big Top/Extras/P2/nothing Top/zz-last; do
    $R READ "$T/a.tar.bz2" "$T/a.map" "$f" > "$T/got" 2> "$T/e"
    [ $? -eq 0 ] && cmp -s "$T/got" "$T/src/$f"
    ok $? "$f comes out of the map byte for byte"
done
$R READ "$T/a.tar.bz2" "$T/a.map" Top/Extras/P1/big,Top/Extras/P3/big > "$T/got2"
cat "$T/src/Top/Extras/P1/big" "$T/src/Top/Extras/P3/big" | cmp -s - "$T/got2"
                                                        ok $? "two members far apart come out of one read"
# the last member: in the last block, where the stream's combined CRC is
tail=$(awk '/^b /{b=$2} END{print b}' "$T/a.map")
[ -n "$tail" ];                                         ok $? "the map names the last block"

echo "streams"
half=$(( $(wc -c < "$T/a.tar") / 2 ))
head -c "$half" "$T/a.tar" | bzip2 -9 > "$T/c.tar.bz2"
tail -c "+$((half + 1))" "$T/a.tar" | bzip2 -9 >> "$T/c.tar.bz2"
$R MAP "$T/c.tar.bz2" "$T/c.map";                       ok $? "two bzip2 streams back to back are mapped"
[ "$(awk '/^b /{print $5}' "$T/c.map" | sort -u | wc -l | tr -d ' ')" -eq 2 ]
                                                        ok $? "and the blocks of each stream know where their stream ends"
for f in Top/Extras/P1/big Top/Extras/P2/big Top/Extras/P3/big Top/zz-last; do
    $R READ "$T/c.tar.bz2" "$T/c.map" "$f" > "$T/got"
    [ $? -eq 0 ] && cmp -s "$T/got" "$T/src/$f"
    ok $? "$f comes out of the two-stream archive byte for byte"
done

echo "damage"
sed 's/^b \([0-9]*\) /b 999\1 /' "$T/a.map" > "$T/bad.map"
$R READ "$T/a.tar.bz2" "$T/bad.map" Top/Extras/P1/big > "$T/got" 2>/dev/null
[ $? -eq 3 ];                                           ok $? "a map whose block offsets are nonsense serves no read"
printf 'b 32 0 9 12\nf 0 5 644 Top/zz-last\n' > "$T/short.map"
$R READ "$T/a.tar.bz2" "$T/short.map" Top/zz-last > /dev/null 2>&1
[ $? -eq 3 ];                                           ok $? "so does one whose stream ends before its block"

# A negative control: the checks above can fail. Reading a member with the
# map of a different archive gives other bytes, and the comparison says so.
$R MAP "$T/c.tar.bz2" "$T/c.map" 2>/dev/null
$R READ "$T/a.tar.bz2" "$T/c.map" Top/Extras/P3/big > "$T/wrong" 2>/dev/null
if cmp -s "$T/wrong" "$T/src/Top/Extras/P3/big"; then control=1; else control=0; fi
[ "$control" -eq 0 ];                                   ok $? "negative control: the wrong map does not deliver the right bytes"

# ---- installing from the archive ---------------------------------------
cd "$T"
$PKG KEYGEN FILE key > /dev/null 2>&1
PKG_SIGNKEY=$PWD/key; export PKG_SIGNKEY
mkdir -p ch/archives
cp "$T/a.tar.bz2" ch/archives/a.tar.bz2
for n in 1 2 3; do
    $PKG PUBLISH "ch/archives/a.tar.bz2!/Top" FILES "Extras/P$n" CHANNEL ch NAME "p$n" \
         KIND application VERSION "1.$n" > "$T/pub$n" 2>&1
    ok $? "p$n is published out of the archive"
done

echo "install"
rm -f ch/archives/a.tar.bz2.pkgmap
$PKG INSTALL p1 ROOT root CHANNEL ch TRACE - > out1 2> tr1;  ok $? "the first install reads the archive whole"
has tr1 'wrote the block map';                          ok $? "and leaves the block map beside the archive"
[ -f ch/archives/a.tar.bz2.pkgmap ];                    ok $? "the map is a file of its own, next to the archive"
cmp -s root/Extras/P1/big "$T/src/Top/Extras/P1/big";   ok $? "the files it placed are the archive's, byte for byte"

$PKG INSTALL p2 ROOT root CHANNEL ch TRACE - > out2 2> tr2; ok $? "the second install uses the map"
has tr2 'has a block map; reading only the blocks';     ok $? "and says so"
! has tr2 'did not serve this read';                    ok $? "and needs no fall back"
cmp -s root/Extras/P2/big "$T/src/Top/Extras/P2/big" \
  && cmp -s root/Extras/P2/ReadMe "$T/src/Top/Extras/P2/ReadMe" \
  && [ ! -s root/Extras/P2/nothing ] && [ -f root/Extras/P2/nothing ]
                                                        ok $? "every file it placed is the archive's, the empty one included"
has out2 'reading the blocks it needs of'
                                                        ok $? "and it says where it read them"
$PKG INSTALL p3 ROOT root2 CHANNEL ch MACHINE > out3 2>&1
has out3 '^archive-from: map ' && has out3 '^cache: '
                                                        ok $? "MACHINE gets archive-from: and cache: records"

echo "fallback"
# A map whose bytes are damaged is thrown away, the archive read whole, and
# the install goes through all the same.
rm -rf root3
printf 'b 1 1 9 1\n' >> ch/archives/a.tar.bz2.pkgmap
sed -i.bak 's/^f \([0-9]*\)/f 9\1/' ch/archives/a.tar.bz2.pkgmap
$PKG INSTALL p1 ROOT root3 CHANNEL ch TRACE - > out4 2> tr4;  ok $? "an install whose map is corrupt goes through"
has tr4 'did not serve this read';                      ok $? "and says the map did not serve it"
has tr4 'wrote the block map';                          ok $? "and writes the map again"
cmp -s root3/Extras/P1/big "$T/src/Top/Extras/P1/big";  ok $? "with the archive's bytes"

# An archive that changed is a different archive: its old map is not read.
rm -rf root4
touch ch/archives/a.tar.bz2
$PKG INSTALL p1 ROOT root4 CHANNEL ch TRACE - > out5 2> tr5;  ok $? "an install after the archive changed goes through"
! has tr5 'has a block map';                            ok $? "and does not read the map made for the old one"
has tr5 'wrote the block map';                          ok $? "and writes one for the new one"

echo "unpacked"
mkdir -p un
(cd un && tar xjf "$T/a.tar.bz2")
rm -rf root5
$PKG INSTALL p1 ROOT root5 CHANNEL ch UNPACKED un > out6 2>&1; ok $? "UNPACKED reads the files from a drawer"
has out6 'reading the unpacked archive';             ok $? "and says which drawer"
cmp -s root5/Extras/P1/big "$T/src/Top/Extras/P1/big";  ok $? "byte for byte"
printf 'tampered' >> un/Top/Extras/P1/ReadMe
rm -rf root6
$PKG INSTALL p1 ROOT root6 CHANNEL ch UNPACKED un > out7 2>&1
[ $? -eq 12 ] && has out7 'Extras/P1/ReadMe' && has out7 'not the file'
                                                        ok $? "a file that differs is refused (12) by name"
[ ! -d root6/Extras ];                                  ok $? "and nothing was installed"
rm -rf un2; mkdir -p un2
$PKG INSTALL p1 ROOT root6 CHANNEL ch UNPACKED un2 > out8 2>&1
[ $? -eq 11 ] && has out8 'lacks Top/Extras/P1'
                                                        ok $? "an empty drawer is refused (11), naming what is missing"

echo "several names"
rm -rf root7
$PKG INSTALL p1 p2 p3 ROOT root7 CHANNEL ch > out9 2>&1
[ $? -eq 0 ] && has out9 'installed 3 packages'
                                                        ok $? "three names on one line install in one command"
cmp -s root7/Extras/P3/big "$T/src/Top/Extras/P3/big";  ok $? "and every one of them placed its files"
rm -rf root8
$PKG INSTALL p1 nosuch p3 ROOT root8 CHANNEL ch > outa 2>&1
rc=$?
[ "$rc" -eq 11 ] && has outa 'p1' && has outa 'nosuch' && has outa 'p3' \
  && has outa 'installed 2 of 3'
                                                        ok $? "one bad name is reported, the rest go ahead, and the code is its class"
cmp -s root8/Extras/P3/big "$T/src/Top/Extras/P3/big";  ok $? "the name after the bad one was installed"
$PKG INSTALL p1 p2 ROOT root9 CHANNEL ch VERSION 1.1 > outb 2>&1
[ $? -eq 20 ] && has outb 'one package'
                                                        ok $? "VERSION with several names is refused (20)"
$PKG INSTALL p1 p2 p3 ROOT rootA CHANNEL ch MACHINE > outc 2>&1
has outc '^package: p1 ' && has outc '^installed: 3$' && has outc '^summary: '
                                                        ok $? "MACHINE reports each name and the count"

echo "fastarchive: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
