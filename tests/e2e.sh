#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# End to end on the host: publish a drawer into a channel that is a directory,
# install it into a root, list, verify, damage, remove. Then the refusals.
#
# Independent oracles, so this does more than replay what the code intends:
# macOS `shasum -a 256` for every digest, `cmp` for installed bytes, and a
# channel entry forged by Python rather than by pkg for the traversal cases.

set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-e2e.XXXXXX")
trap 'rm -rf "$T"' EXIT

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

echo "publish_install_verify_remove"

D="$T/drawer"
mkdir -p "$D/C" "$D/Libs" "$D/S"
printf 'binary\000$VER: Hello 1.2 (18.9.2026)\000tail' > "$D/C/Hello"
printf 'library data\n' > "$D/Libs/data.txt"
printf 'echo started\n' > "$D/S/My Startup"
printf 'finder junk' > "$D/.DS_Store"
printf 'appledouble' > "$D/Libs/._data.txt"
CH="$T/channel"
R="$T/root"

$PKG MANIFEST "$D" > "$T/m1" 2>&1;                    ok $? "manifest succeeds"
has "$T/m1" '^Name: hello$';                          ok $? "name from \$VER:, lowercased"
has "$T/m1" '^Version: 1.2$';                         ok $? "version from \$VER:"
! has "$T/m1" 'DS_Store';                             ok $? ".DS_Store left out"
! has "$T/m1" '\._data';                              ok $? "AppleDouble left out"
has "$T/m1" ' S/My Startup$';                         ok $? "a path with a space"
grep '^File:' "$T/m1" | awk '{print $4" "$5}' > "$T/order"
sort "$T/order" | cmp -s - "$T/order";                ok $? "File lines sorted"

want=$(shasum -a 256 "$D/Libs/data.txt" | cut -d' ' -f1)
has "$T/m1" "^File: $want 13 Libs/data.txt$";         ok $? "digest agrees with shasum"

$PKG PUBLISH "$D" CHANNEL "$CH" > "$T/pub" 2>&1;      ok $? "publish succeeds"
has "$T/pub" 'published hello 1.2';                   ok $? "publish reports what it did"
has "$T/pub" 'skipped 2 host metadata';               ok $? "publish reports skipped files"
digest=$(awk '$1=="hello"{print $3}' "$CH/index")
[ "$(shasum -a 256 "$CH/objects/$digest.pkg" | cut -d' ' -f1)" = "$digest" ]
                                                      ok $? "payload name is its shasum"
cmp -s "$T/m1" "$CH/objects/$digest.manifest";        ok $? "MANIFEST equals what PUBLISH stored"

cp -R "$D" "$T/elsewhere"
$PKG MANIFEST "$T/elsewhere" > "$T/m2" 2>&1
cmp -s "$T/m1" "$T/m2";                               ok $? "same drawer elsewhere, same manifest"

$PKG PUBLISH "$D" CHANNEL "$CH" > "$T/pub2" 2>&1;     ok $? "republishing identical bytes is not an error"
has "$T/pub2" 'nothing to do';                        ok $? "and says so"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/inst" 2>&1
                                                      ok $? "install succeeds"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "C/Hello byte-identical"
cmp -s "$D/S/My Startup" "$R/S/My Startup";           ok $? "S/My Startup byte-identical"
[ ! -e "$R/.DS_Store" ];                              ok $? "host metadata not installed"
[ ! -e "$R/.pkg/staging/hello" ];                     ok $? "staging cleaned up"

$PKG LIST ROOT "$R" > "$T/list" 2>&1;                 ok $? "list succeeds"
has "$T/list" '^hello  *1.2  *application  *3 files'; ok $? "list shows the package"

$PKG VERIFY hello ROOT "$R" > "$T/ver" 2>&1;          ok $? "verify passes when intact"
has "$T/ver" 'all intact';                            ok $? "and says so"

printf 'edited by the user\n' > "$R/Libs/data.txt"
$PKG VERIFY hello ROOT "$R" > "$T/ver2" 2>&1
[ $? -eq 1 ];                                         ok $? "verify fails after an edit"
has "$T/ver2" 'changed  Libs/data.txt';               ok $? "and names the file"

$PKG REMOVE hello ROOT "$R" > "$T/rm" 2>&1;           ok $? "remove succeeds"
has "$T/rm" 'kept     Libs/data.txt';                 ok $? "the edited file is kept"
[ -f "$R/Libs/data.txt" ];                            ok $? "and is still there"
[ ! -e "$R/C" ];                                      ok $? "emptied directories are pruned"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "database entry gone"
rm -f "$R/Libs/data.txt"; rmdir "$R/Libs" 2>/dev/null

echo "refusals"

# A published version never changes.
printf 'different\n' > "$D/Libs/data.txt"
$PKG PUBLISH "$D" CHANNEL "$CH" > "$T/rep" 2>&1
[ $? -eq 1 ];                                         ok $? "republishing a version with new bytes refused"
has "$T/rep" 'never changes';                         ok $? "and says why"
printf 'library data\n' > "$D/Libs/data.txt"

# A tampered payload: one byte flipped in the channel.
cp "$CH/objects/$digest.pkg" "$T/good.pkg"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)
" "$CH/objects/$digest.pkg"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/tamper" 2>&1
[ $? -eq 1 ];                                         ok $? "tampered payload refused"
has "$T/tamper" "expected $digest";                   ok $? "the refusal names the expected digest"
[ ! -e "$R/C/Hello" ];                                ok $? "and nothing was installed"
cp "$T/good.pkg" "$CH/objects/$digest.pkg"

# Install twice.
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/twice" 2>&1
[ $? -eq 1 ];                                         ok $? "second install refused"
has "$T/twice" 'already installed';                   ok $? "and says why"
$PKG REMOVE hello ROOT "$R" > /dev/null 2>&1

# A file already in the root that the package would overwrite.
mkdir -p "$R/C"; printf 'mine' > "$R/C/Hello"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/clash" 2>&1
[ $? -eq 1 ];                                         ok $? "overwrite refused"
[ "$(cat "$R/C/Hello")" = "mine" ];                   ok $? "the existing file is untouched"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "and nothing is recorded"
rm -rf "$R"

# Unknown package, and a version not offered.
$PKG INSTALL nosuch ROOT "$R" CHANNEL "$CH" > "$T/none" 2>&1
[ $? -eq 1 ];                                         ok $? "unknown package refused"
$PKG INSTALL hello VERSION 9 ROOT "$R" CHANNEL "$CH" > "$T/v9" 2>&1
[ $? -eq 1 ];                                         ok $? "absent version refused"
has "$T/v9" 'versions offered: 1.2';                  ok $? "and the versions on offer are listed"

# Forged entries, written by Python, never by pkg.
forge() {  # name container_path manifest_path
    python3 - "$CH" "$1" "$2" "$3" <<'PY'
import sys, hashlib, struct, os
ch, name, cpath, mpath = sys.argv[1:]
data = b'evil\n'
body = struct.pack('>I', len(cpath)) + cpath.encode() + b'\0' + struct.pack('>I', len(data)) + data
pkg = b'PKG\x01' + struct.pack('>I', 8 + len(body)) + body
d = hashlib.sha256(pkg).hexdigest()
fd = hashlib.sha256(data).hexdigest()
man = ('Format: pkg-manifest 1\nName: %s\nVersion: 1\nArchitecture: generic\n'
       'Kind: data\nPayload: %s\nFile: %s %d %s\n') % (name, d, fd, len(data), mpath)
open(os.path.join(ch, 'objects', d + '.pkg'), 'wb').write(pkg)
open(os.path.join(ch, 'objects', d + '.manifest'), 'w').write(man)
open(os.path.join(ch, 'index'), 'a').write('%s 1 %s\n' % (name, d))
PY
}

forge evil-a "../evil" "../evil"
$PKG INSTALL evil-a ROOT "$R" CHANNEL "$CH" > "$T/ea" 2>&1
[ $? -eq 1 ];                                         ok $? "traversal in the manifest refused"
has "$T/ea" "'\.\.' component";                       ok $? "and the reason is named"

forge evil-b "../evil" "evil"
$PKG INSTALL evil-b ROOT "$R" CHANNEL "$CH" > "$T/eb" 2>&1
[ $? -eq 1 ];                                         ok $? "container disagreeing with its manifest refused"
[ ! -e "$T/evil" ] && [ ! -e "$R/evil" ];             ok $? "nothing written inside or outside the root"

forge evil-c ".pkg/db/hello" ".pkg/db/hello"
$PKG INSTALL evil-c ROOT "$R" CHANNEL "$CH" > "$T/ec" 2>&1
[ $? -eq 1 ];                                         ok $? "a payload writing into the database refused"

echo
echo "$checks checks, $fails failures"
[ "$fails" -eq 0 ]
