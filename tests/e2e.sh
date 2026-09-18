#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# End to end on the host. M1: publish a drawer into a channel that is a
# directory, install it into a root, list, verify, damage, remove, and the
# refusals. M2: signatures, key pinning, upgrade, rollback, downgrade.
#
# Independent oracles, so this does more than replay what the code intends:
# `shasum -a 256` for digests, `cmp` for installed bytes, `stat` for the key
# file's mode, and channel entries forged by Python rather than by pkg for the
# traversal cases. The forged entries are validly signed, so that what refuses
# them is the path check under test and not a missing signature.

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

KEY="$T/dev.key"
EVIL="$T/other.key"
$PKG KEYGEN FILE "$KEY" > "$T/kg" 2>&1 || { echo "cannot create the key"; exit 1; }
$PKG KEYGEN FILE "$EVIL" > /dev/null 2>&1
PUB=$(awk '/^Public:/{print $2}' "$KEY")
EVILPUB=$(awk '/^Public:/{print $2}' "$EVIL")
export PKG_SIGNKEY="$KEY"

echo "keys"
[ "$(stat -f %Lp "$KEY" 2>/dev/null || stat -c %a "$KEY")" = "600" ]
                                                      ok $? "key file readable by the owner alone"
has "$T/kg" "public key $PUB";                        ok $? "keygen prints the public key"
$PKG KEYGEN FILE "$KEY" > "$T/kg2" 2>&1
[ $? -eq 1 ];                                         ok $? "an existing key is never overwritten"
[ "$(awk '/^Public:/{print $2}' "$KEY")" = "$PUB" ];  ok $? "and the key is unchanged"

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
has "$T/pub" "signed by $(echo "$PUB" | cut -c1-16)"; ok $? "publish names the signing key"
has "$T/pub" 'skipped 2 host metadata';               ok $? "publish reports skipped files"
digest=$(awk '$1=="hello"{print $3}' "$CH/index")
payload=$(awk '/^Payload:/{print $2}' "$CH/objects/$digest.manifest")
[ "$(shasum -a 256 "$CH/objects/$digest.manifest" | cut -d' ' -f1)" = "$digest" ]
                                                      ok $? "the index names the manifest by its shasum"
[ "$(shasum -a 256 "$CH/objects/$payload.pkg" | cut -d' ' -f1)" = "$payload" ]
                                                      ok $? "the manifest names the payload by its shasum"
cmp -s "$T/m1" "$CH/objects/$digest.manifest";        ok $? "MANIFEST equals what PUBLISH stored"
has "$CH/objects/$digest.sig" "^Signer: $PUB$";       ok $? "signature file names its signer"

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
[ "$(head -c 64 "$R/.pkg/keys/hello")" = "$PUB" ];    ok $? "the signing key is pinned on first install"

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

printf 'different\n' > "$D/Libs/data.txt"
$PKG PUBLISH "$D" CHANNEL "$CH" > "$T/rep" 2>&1
[ $? -eq 1 ];                                         ok $? "republishing a version with new bytes refused"
has "$T/rep" 'never changes';                         ok $? "and says why"
printf 'library data\n' > "$D/Libs/data.txt"

env -u PKG_SIGNKEY $PKG PUBLISH "$D" CHANNEL "$T/ch-nokey" > "$T/nokey" 2>&1
[ $? -eq 1 ];                                         ok $? "publishing without a key refused"
has "$T/nokey" 'Every package is signed';             ok $? "and says why"
[ ! -e "$T/ch-nokey/index" ];                         ok $? "and nothing was published"

cp "$CH/objects/$payload.pkg" "$T/good.pkg"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)
" "$CH/objects/$payload.pkg"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/tamper" 2>&1
[ $? -eq 1 ];                                         ok $? "tampered payload refused"
has "$T/tamper" "expected $payload";                  ok $? "the refusal names the expected digest"
[ ! -e "$R/C/Hello" ];                                ok $? "and nothing was installed"
cp "$T/good.pkg" "$CH/objects/$payload.pkg"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/twice" 2>&1
[ $? -eq 1 ];                                         ok $? "second install refused"
has "$T/twice" 'already installed';                   ok $? "and points to UPGRADE"
$PKG REMOVE hello ROOT "$R" > /dev/null 2>&1

mkdir -p "$R/C"; printf 'mine' > "$R/C/Hello"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/clash" 2>&1
[ $? -eq 1 ];                                         ok $? "overwrite refused"
[ "$(cat "$R/C/Hello")" = "mine" ];                   ok $? "the existing file is untouched"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "and nothing is recorded"
rm -rf "$R"

$PKG INSTALL nosuch ROOT "$R" CHANNEL "$CH" > "$T/none" 2>&1
[ $? -eq 1 ];                                         ok $? "unknown package refused"
$PKG INSTALL hello VERSION 9 ROOT "$R" CHANNEL "$CH" > "$T/v9" 2>&1
[ $? -eq 1 ];                                         ok $? "absent version refused"
has "$T/v9" 'versions offered: 1.2';                  ok $? "and the versions on offer are listed"

# Forged entries, written by Python and signed with the development key, so
# the signature is valid and the path check is what must refuse them.
forge() {  # name container_path manifest_path
    d=$(python3 - "$CH" "$1" "$2" "$3" <<'PY'
import sys, hashlib, struct, os
ch, name, cpath, mpath = sys.argv[1:]
data = b'evil\n'
body = struct.pack('>I', len(cpath)) + cpath.encode() + b'\0' + struct.pack('>I', len(data)) + data
pkg = b'PKG\x01' + struct.pack('>I', 8 + len(body)) + body
d = hashlib.sha256(pkg).hexdigest()
fd = hashlib.sha256(data).hexdigest()
man = ('Format: pkg-manifest 1\nName: %s\nVersion: 1\nArchitecture: generic\n'
       'Kind: data\nPayload: %s\nFile: %s %d %s\n') % (name, d, fd, len(data), mpath)
md = hashlib.sha256(man.encode()).hexdigest()
open(os.path.join(ch, 'objects', d + '.pkg'), 'wb').write(pkg)
open(os.path.join(ch, 'objects', md + '.manifest'), 'w').write(man)
open(os.path.join(ch, 'index'), 'a').write('%s 1 %s\n' % (name, md))
print(md)
PY
)
    $PKG SIGN "$CH/objects/$d.manifest" KEY "$KEY" OUT "$CH/objects/$d.sig" > /dev/null
}

forge evil-a "../evil" "../evil"
$PKG INSTALL evil-a ROOT "$R" CHANNEL "$CH" > "$T/ea" 2>&1
[ $? -eq 1 ];                                         ok $? "traversal in the manifest refused"
has "$T/ea" "'\.\.' component";                       ok $? "for the traversal, not the signature"

forge evil-b "../evil" "evil"
$PKG INSTALL evil-b ROOT "$R" CHANNEL "$CH" > "$T/eb" 2>&1
[ $? -eq 1 ];                                         ok $? "container disagreeing with its manifest refused"
has "$T/eb" 'where the manifest lists';               ok $? "for the disagreement, not the signature"
[ ! -e "$T/evil" ] && [ ! -e "$R/evil" ];             ok $? "nothing written inside or outside the root"

forge evil-c ".pkg/db/hello" ".pkg/db/hello"
$PKG INSTALL evil-c ROOT "$R" CHANNEL "$CH" > "$T/ec" 2>&1
[ $? -eq 1 ];                                         ok $? "a payload writing into the database refused"
has "$T/ec" 'inside .pkg';                            ok $? "for the database path, not the signature"

echo "identical_payloads"

# Two versions whose bytes are identical keep separate manifests and signatures
# over one shared payload. The first layout keyed manifests by the payload, so
# the second publish overwrote the first's manifest; this is the case that
# found it.
mkdir -p "$T/same/Libs"; printf 'same bytes\n' > "$T/same/Libs/x.txt"
$PKG PUBLISH "$T/same" CHANNEL "$CH" NAME same VERSION 1 KIND data > /dev/null 2>&1
                                                      ok $? "publish same 1"
$PKG PUBLISH "$T/same" CHANNEL "$CH" NAME same VERSION 2 KIND data > /dev/null 2>&1
                                                      ok $? "publish same 2, identical bytes"
[ "$(awk '$1=="same"{print $3}' "$CH/index" | sort -u | wc -l | tr -d ' ')" = 2 ]
                                                      ok $? "two index entries, two distinct manifests"
$PKG INSTALL same VERSION 1 ROOT "$T/r-same" CHANNEL "$CH" > "$T/same1" 2>&1
                                                      ok $? "version 1 installs"
has "$T/same1" 'installed same 1';                    ok $? "and is version 1"
$PKG UPGRADE same VERSION 2 ROOT "$T/r-same" CHANNEL "$CH" > "$T/same2" 2>&1
                                                      ok $? "version 2 installs over it"
has "$T/same2" 'upgraded same from 1 to 2';           ok $? "and is version 2"

echo "signatures"

cp "$CH/objects/$digest.sig" "$T/good.sig"
python3 -c "
import sys
p=sys.argv[1]; s=open(p).read(); i=s.index('Signature: ')+11
c='0' if s[i]!='0' else '1'; open(p,'w').write(s[:i]+c+s[i+1:])
" "$CH/objects/$digest.sig"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/sigt" 2>&1
[ $? -eq 1 ];                                         ok $? "an altered signature refused"
has "$T/sigt" 'does not verify';                      ok $? "and says why"
cp "$T/good.sig" "$CH/objects/$digest.sig"

rm "$CH/objects/$digest.sig"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/unsig" 2>&1
[ $? -eq 1 ];                                         ok $? "an unsigned package refused"
has "$T/unsig" 'is not signed';                       ok $? "and says why"
cp "$T/good.sig" "$CH/objects/$digest.sig"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "and nothing was installed by either"

echo "upgrade_rollback"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
                                                      ok $? "install 1.2"
cp -R "$D" "$T/v13"
printf 'binary 2\000$VER: Hello 1.3 (19.9.2026)\000tail' > "$T/v13/C/Hello"
rm "$T/v13/S/My Startup"; rmdir "$T/v13/S"
printf 'new in 1.3\n' > "$T/v13/Libs/extra.txt"
$PKG PUBLISH "$T/v13" CHANNEL "$CH" > /dev/null 2>&1; ok $? "publish 1.3"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/up" 2>&1
                                                      ok $? "upgrade to 1.3"
has "$T/up" 'upgraded hello from 1.2 to 1.3';         ok $? "upgrade reports both versions"
cmp -s "$T/v13/C/Hello" "$R/C/Hello";                 ok $? "C/Hello is the 1.3 bytes"
[ -f "$R/Libs/extra.txt" ];                           ok $? "a file new in 1.3 appears"
[ ! -e "$R/S/My Startup" ] && [ ! -e "$R/S" ];        ok $? "a file dropped in 1.3 is removed, with its directory"
$PKG VERIFY hello ROOT "$R" > /dev/null 2>&1;         ok $? "verify passes on 1.3"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/up2" 2>&1
                                                      ok $? "upgrading when current is not an error"
has "$T/up2" 'already at 1.3';                        ok $? "and says so"

$PKG ROLLBACK hello ROOT "$R" CHANNEL "$CH" > "$T/rb" 2>&1
                                                      ok $? "rollback succeeds"
has "$T/rb" 'rolled back hello from 1.3 to 1.2';      ok $? "rollback reports both versions"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "C/Hello is the 1.2 bytes again"
cmp -s "$D/S/My Startup" "$R/S/My Startup";           ok $? "the file 1.3 dropped is back"
[ ! -e "$R/Libs/extra.txt" ];                         ok $? "the file 1.3 added is gone"
$PKG VERIFY hello ROOT "$R" > /dev/null 2>&1;         ok $? "verify passes on 1.2"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG UPGRADE hello VERSION 1.2 ROOT "$R" CHANNEL "$CH" > "$T/dg" 2>&1
[ $? -eq 1 ];                                         ok $? "EXACT to an older version refused without DOWNGRADE"
has "$T/dg" 'add DOWNGRADE';                          ok $? "and the way through is named"
$PKG UPGRADE hello VERSION 1.2 DOWNGRADE ROOT "$R" CHANNEL "$CH" > "$T/dg2" 2>&1
                                                      ok $? "EXACT with DOWNGRADE succeeds"
has "$T/dg2" 'downgraded hello from 1.3 to 1.2';      ok $? "and calls it a downgrade"

printf 'my own edit\n' > "$R/C/Hello"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/conf" 2>&1
[ $? -eq 1 ];                                         ok $? "upgrade over an edited file both versions ship refused"
[ "$(cat "$R/C/Hello")" = "my own edit" ];            ok $? "the edit is untouched"
has "$T/conf" 'was edited since';                     ok $? "and the refusal names the file"
cp "$D/C/Hello" "$R/C/Hello"

echo "substituted_key"

cp -R "$T/v13" "$T/v14"
printf 'binary 3\000$VER: Hello 1.4 (20.9.2026)\000tail' > "$T/v14/C/Hello"
PKG_SIGNKEY="$EVIL" $PKG PUBLISH "$T/v14" CHANNEL "$CH" > /dev/null 2>&1
                                                      ok $? "a second key can publish 1.4 into the channel"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/sub" 2>&1
[ $? -eq 1 ];                                         ok $? "an upgrade signed by a substituted key refused"
has "$T/sub" "pinned $PUB";                           ok $? "the refusal prints the pinned key"
has "$T/sub" "signer $EVILPUB";                       ok $? "and the new signer"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "and nothing was changed"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" ACCEPTKEY "$PUB" > "$T/sub2" 2>&1
[ $? -eq 1 ];                                         ok $? "ACCEPTKEY naming the wrong key does not open the door"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" ACCEPTKEY "$EVILPUB" > "$T/sub3" 2>&1
                                                      ok $? "ACCEPTKEY naming the new key in full accepts it"
has "$T/sub3" 'changed by explicit ACCEPTKEY';        ok $? "and says so"
[ "$(head -c 64 "$R/.pkg/keys/hello")" = "$EVILPUB" ]; ok $? "and the new key is now the pinned one"

echo
echo "$checks checks, $fails failures"
[ "$fails" -eq 0 ]
