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
[ $? -eq 15 ];                                         ok $? "an existing key is never overwritten"
[ "$(awk '/^Public:/{print $2}' "$KEY")" = "$PUB" ];  ok $? "and the key is unchanged"

echo "publish_install_verify_remove"

D="$T/drawer"
mkdir -p "$D/C" "$D/Libs" "$D/S"
printf 'binary\000$VER: Hello 1.2 (18.9.2026)\000tail' > "$D/C/Hello"
printf 'library data\n' > "$D/Libs/data.txt"
printf 'echo started\n' > "$D/S/My Startup"
printf 'finder junk' > "$D/.DS_Store"
printf 'appledouble' > "$D/Libs/._data.txt"
mkdir -p "$D/.git/objects"; printf 'repo' > "$D/.git/objects/x"
printf 'workbench state' > "$D/.backdrop"
printf 'an icon' > "$D/C.info"
CH="$T/channel"
R="$T/root"

$PKG MANIFEST "$D" > "$T/m1" 2>&1;                    ok $? "manifest succeeds"
has "$T/m1" '^Name: hello$';                          ok $? "name from \$VER:, lowercased"
has "$T/m1" '^Version: 1.2$';                         ok $? "version from \$VER:"
! has "$T/m1" 'DS_Store';                             ok $? ".DS_Store left out"
! has "$T/m1" '\._data';                              ok $? "AppleDouble left out"
! has "$T/m1" '\.git' && ! has "$T/m1" 'backdrop';    ok $? "a hidden directory and the Workbench .backdrop left out"
has "$T/m1" ' C\.info$';                              ok $? "an Amiga icon, Name.info, is kept"
has "$T/m1" ' S/My Startup$';                         ok $? "a path with a space"
grep '^File:' "$T/m1" | awk '{print $4" "$5}' > "$T/order"
sort "$T/order" | cmp -s - "$T/order";                ok $? "File lines sorted"

want=$(shasum -a 256 "$D/Libs/data.txt" | cut -d' ' -f1)
has "$T/m1" "^File: $want 13 Libs/data.txt$";         ok $? "digest agrees with shasum"

$PKG PUBLISH "$D" CHANNEL "$CH" KIND application > "$T/pub" 2>&1;      ok $? "publish succeeds"
has "$T/pub" 'published hello 1.2';                   ok $? "publish reports what it did"
has "$T/pub" "signed by $(echo "$PUB" | cut -c1-16)"; ok $? "publish names the signing key"
has "$T/pub" 'left out \.DS_Store,' && has "$T/pub" 'left out Libs/\._data\.txt,'
                                                      ok $? "publish names each host file it left out"
digest=$(awk '$1=="hello"{print $4}' "$CH/index")
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

$PKG PUBLISH "$D" CHANNEL "$CH" KIND application > "$T/pub2" 2>&1;     ok $? "republishing identical bytes is not an error"
has "$T/pub2" 'nothing to do';                        ok $? "and says so"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/inst" 2>&1
                                                      ok $? "install succeeds"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "C/Hello byte-identical"
cmp -s "$D/S/My Startup" "$R/S/My Startup";           ok $? "S/My Startup byte-identical"
[ ! -e "$R/.DS_Store" ];                              ok $? "host metadata not installed"
[ ! -e "$R/.pkg/staging/hello" ];                     ok $? "staging cleaned up"
[ "$(head -c 64 "$R/.pkg/keys/hello")" = "$PUB" ];    ok $? "the signing key is pinned on first install"

$PKG LIST ROOT "$R" > "$T/list" 2>&1;                 ok $? "list succeeds"
has "$T/list" '^hello  *1.2  *application  *4 files'; ok $? "list shows the package"

$PKG VERIFY hello ROOT "$R" > "$T/ver" 2>&1;          ok $? "verify passes when intact"
has "$T/ver" 'all intact';                            ok $? "and says so"

printf 'edited by the user\n' > "$R/Libs/data.txt"
$PKG VERIFY hello ROOT "$R" > "$T/ver2" 2>&1
[ $? -eq 12 ];                                         ok $? "verify fails after an edit"
has "$T/ver2" 'changed  Libs/data.txt';               ok $? "and names the file"

$PKG REMOVE hello ROOT "$R" > "$T/rm" 2>&1;           ok $? "remove succeeds"
has "$T/rm" 'kept     Libs/data.txt';                 ok $? "the edited file is kept"
[ -f "$R/Libs/data.txt" ];                            ok $? "and is still there"
[ ! -e "$R/C" ];                                      ok $? "emptied directories are pruned"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "database entry gone"
rm -f "$R/Libs/data.txt"; rmdir "$R/Libs" 2>/dev/null

echo "cookies"

mkdir -p "$T/two/C"
printf 'x\000$VER: Guru 2.0 (31.5.2011)\000' > "$T/two/C/Guru"
printf 'y\000$VER: Function 2.0 (31.5.2011)\000' > "$T/two/C/Function"
$PKG MANIFEST "$T/two" > "$T/two.m" 2>&1
[ $? -eq 20 ] && has "$T/two.m" 'C/Function (function 2.0), C/Guru (guru 2.0)'
                                                      ok $? "two programs' cookies, no NAME: refused with 20, both listed"
$PKG MANIFEST "$T/two" NAME guru > "$T/two.m2" 2>&1
has "$T/two.m2" '^Name: guru$' && has "$T/two.m2" '^Version: 2.0$'
                                                      ok $? "NAME alone takes the version from that program's cookie"
mkdir -p "$T/one/L"
printf 'h\000$VER: afs.handler 41.7 (1.1.2026)\000' > "$T/one/L/afs-handler"
$PKG MANIFEST "$T/one" NAME afs-handler > "$T/one.m" 2>&1
has "$T/one.m" '^Name: afs-handler$' && has "$T/one.m" '^Version: 41.7$'
                                                      ok $? "NAME differing from the only cookie keeps that cookie's version"

# Architecture from the executables' own headers.
mkdir -p "$T/elf/C" "$T/mix/C"
python3 -c "
import sys
elf = bytearray(64); elf[0:4] = b'\x7fELF'; elf[4] = 2; elf[5] = 1; elf[18] = 183
open(sys.argv[1], 'wb').write(bytes(elf) + b'\0\$VER: Tool 1.0 (1.1.2026)\0')
open(sys.argv[2], 'wb').write(b'\x00\x00\x03\xf3' + b'\0' * 28)
" "$T/elf/C/Tool" "$T/mix/C/Old"
cp "$T/elf/C/Tool" "$T/mix/C/Tool"
$PKG MANIFEST "$T/elf" > "$T/elf.m" 2>&1
has "$T/elf.m" '^Architecture: aarch64$';            ok $? "an aarch64 ELF executable makes the package aarch64"
$PKG MANIFEST "$T/mix" NAME tool > "$T/mix.m" 2>&1
[ $? -eq 20 ] && has "$T/mix.m" 'C/Old (m68k)';       ok $? "aarch64 and 68k hunk executables in one drawer: refused with 20, both named"
$PKG MANIFEST "$T/elf" ARCH m68k > "$T/elf.m2" 2>&1
[ $? -eq 20 ] && has "$T/elf.m2" 'built for aarch64'; ok $? "ARCH contradicting the executables is refused"
$PKG MANIFEST "$D" > "$T/gen.m" 2>&1
has "$T/gen.m" '^Architecture: generic$';            ok $? "a drawer with no executable header stays generic"
$PKG HELP > "$T/help" 2>"$T/help.e"
[ $? -eq 0 ] && has "$T/help" 'DEPENDS' && [ ! -s "$T/help.e" ]
                                                      ok $? "HELP prints the usage on stdout and succeeds"

echo "refusals"

printf 'different\n' > "$D/Libs/data.txt"
$PKG PUBLISH "$D" CHANNEL "$CH" KIND application > "$T/rep" 2>&1
[ $? -eq 15 ];                                         ok $? "republishing a version with new bytes refused"
has "$T/rep" 'already published with a different payload' && has "$T/rep" 'was not raised'
                                                      ok $? "and says why: the version came from a \$VER not raised, or the old build"
printf 'library data\n' > "$D/Libs/data.txt"

env -u PKG_SIGNKEY $PKG PUBLISH "$D" CHANNEL "$T/ch-nokey" KIND application > "$T/nokey" 2>&1
[ $? -eq 14 ];                                         ok $? "publishing without a key refused, class key"
has "$T/nokey" 'same key';                            ok $? "and says to use the publisher's key"
[ ! -e "$T/ch-nokey/index" ];                         ok $? "and nothing was published"

cp "$CH/objects/$payload.pkg" "$T/good.pkg"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)
" "$CH/objects/$payload.pkg"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/tamper" 2>&1
[ $? -eq 12 ];                                         ok $? "tampered payload refused"
has "$T/tamper" "expected $payload";                  ok $? "the refusal names the expected digest"
[ ! -e "$R/C/Hello" ];                                ok $? "and nothing was installed"
cp "$T/good.pkg" "$CH/objects/$payload.pkg"

$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > /dev/null 2>&1
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/twice" 2>&1
[ $? -eq 0 ] && has "$T/twice" 'already installed';  ok $? "installing the installed version again succeeds and says so"
$PKG REMOVE hello ROOT "$R" > /dev/null 2>&1

mkdir -p "$R/C"; printf 'mine' > "$R/C/Hello"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/clash" 2>&1
[ $? -eq 15 ];                                         ok $? "overwrite refused"
[ "$(cat "$R/C/Hello")" = "mine" ];                   ok $? "the existing file is untouched"
[ ! -e "$R/.pkg/db/hello" ];                          ok $? "and nothing is recorded"
rm -rf "$R"

$PKG INSTALL nosuch ROOT "$R" CHANNEL "$CH" > "$T/none" 2>&1
[ $? -eq 11 ];                                         ok $? "unknown package refused"
$PKG INSTALL hello VERSION 9 ROOT "$R" CHANNEL "$CH" > "$T/v9" 2>&1
[ $? -eq 11 ];                                         ok $? "absent version refused"
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
open(os.path.join(ch, 'index'), 'a').write('%s 1 generic %s\n' % (name, md))
print(md)
PY
)
    $PKG SIGN "$CH/objects/$d.manifest" KEY "$KEY" OUT "$CH/objects/$d.sig" > /dev/null
}

forge evil-a "../evil" "../evil"
$PKG INSTALL evil-a ROOT "$R" CHANNEL "$CH" > "$T/ea" 2>&1
[ $? -eq 12 ];                                         ok $? "traversal in the manifest refused"
has "$T/ea" "'\.\.' component";                       ok $? "for the traversal, not the signature"

forge evil-b "../evil" "evil"
$PKG INSTALL evil-b ROOT "$R" CHANNEL "$CH" > "$T/eb" 2>&1
[ $? -eq 12 ];                                         ok $? "container disagreeing with its manifest refused"
has "$T/eb" 'where the manifest lists';               ok $? "for the disagreement, not the signature"
[ ! -e "$T/evil" ] && [ ! -e "$R/evil" ];             ok $? "nothing written inside or outside the root"

forge evil-c ".pkg/db/hello" ".pkg/db/hello"
$PKG INSTALL evil-c ROOT "$R" CHANNEL "$CH" > "$T/ec" 2>&1
[ $? -eq 12 ];                                         ok $? "a payload writing into the database refused"
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
[ "$(awk '$1=="same"{print $4}' "$CH/index" | sort -u | wc -l | tr -d ' ')" = 2 ]
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
[ $? -eq 13 ];                                         ok $? "an altered signature refused"
has "$T/sigt" 'does not verify';                      ok $? "and says why"
cp "$T/good.sig" "$CH/objects/$digest.sig"

rm "$CH/objects/$digest.sig"
$PKG INSTALL hello ROOT "$R" CHANNEL "$CH" > "$T/unsig" 2>&1
[ $? -eq 13 ];                                         ok $? "an unsigned package refused"
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
$PKG PUBLISH "$T/v13" CHANNEL "$CH" KIND application > /dev/null 2>&1; ok $? "publish 1.3"

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
[ $? -eq 18 ];                                         ok $? "EXACT to an older version refused without DOWNGRADE"
has "$T/dg" "requester's decision" && ! has "$T/dg" 'add DOWNGRADE'
                                                      ok $? "and leaves the choice to the requester, with no keyword to paste"
$PKG UPGRADE hello VERSION 1.2 DOWNGRADE ROOT "$R" CHANNEL "$CH" > "$T/dg2" 2>&1
                                                      ok $? "EXACT with DOWNGRADE succeeds"
has "$T/dg2" 'downgraded hello from 1.3 to 1.2';      ok $? "and calls it a downgrade"

printf 'my own edit\n' > "$R/C/Hello"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/conf" 2>&1
[ $? -eq 15 ];                                         ok $? "upgrade over an edited file both versions ship refused"
[ "$(cat "$R/C/Hello")" = "my own edit" ];            ok $? "the edit is untouched"
has "$T/conf" 'was edited since';                     ok $? "and the refusal names the file"
cp "$D/C/Hello" "$R/C/Hello"

echo "substituted_key"

cp -R "$T/v13" "$T/v14"
printf 'binary 3\000$VER: Hello 1.4 (20.9.2026)\000tail' > "$T/v14/C/Hello"
before=$(cat "$CH/index")
PKG_SIGNKEY="$EVIL" $PKG PUBLISH "$T/v14" CHANNEL "$CH" KIND application MACHINE > "$T/p14" 2>&1
[ $? -eq 14 ] && has "$T/p14" '^next: ask-requester$' && [ "$(cat "$CH/index")" = "$before" ]
                                                      ok $? "publishing hello with another key than its earlier versions: 14, ask-requester, nothing published"
PKG_SIGNKEY="$EVIL" $PKG PUBLISH "$T/v14" CHANNEL "$CH" KIND application ACCEPTKEY "$EVILPUB" > /dev/null 2>&1
                                                      ok $? "a second key can publish 1.4 when ACCEPTKEY names it"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" > "$T/sub" 2>&1
[ $? -eq 14 ];                                         ok $? "an upgrade signed by a substituted key refused"
has "$T/sub" "pinned $PUB";                           ok $? "the refusal prints the pinned key"
has "$T/sub" "signer $EVILPUB";                       ok $? "and the new signer"
cmp -s "$D/C/Hello" "$R/C/Hello";                     ok $? "and nothing was changed"

$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" ACCEPTKEY "$PUB" > "$T/sub2" 2>&1
[ $? -eq 14 ];                                         ok $? "ACCEPTKEY naming the wrong key does not open the door"
$PKG UPGRADE hello ROOT "$R" CHANNEL "$CH" ACCEPTKEY "$EVILPUB" > "$T/sub3" 2>&1
                                                      ok $? "ACCEPTKEY naming the new key in full accepts it"
has "$T/sub3" 'changed by explicit ACCEPTKEY';        ok $? "and says so"
[ "$(head -c 64 "$R/.pkg/keys/hello")" = "$EVILPUB" ]; ok $? "and the new key is now the pinned one"

echo "machine_contract"

# The contract an agent reads on every host: stdout holds only "key: value"
# lines, stderr stays empty, and a refusal's code field equals the exit status.
# The format check is shown able to fail on the human output of the same verb.
M="$T/mroot"
only_kv() { ! LC_ALL=C grep -a -v -E '^[a-z][a-z-]*: ' "$1" > /dev/null; }
mrun() {  # mrun <name> <args...>: stdout to $T/<name>.o, stderr to $T/<name>.e
    n=$1; shift
    $PKG "$@" > "$T/$n.o" 2> "$T/$n.e"
}

mrun mi INSTALL hello VERSION 1.2 ROOT "$M" CHANNEL "$CH" MACHINE
                                                      ok $? "MACHINE install exits 0"
only_kv "$T/mi.o" && [ ! -s "$T/mi.e" ];              ok $? "and prints only key: value lines, nothing on stderr"
has "$T/mi.o" '^result: installed$' && has "$T/mi.o" '^version: 1.2$' \
    && has "$T/mi.o" "^signer: $PUB\$";               ok $? "with the result, the version and the full signer key"
$PKG REMOVE hello ROOT "$M" > /dev/null 2>&1
$PKG INSTALL hello VERSION 1.2 ROOT "$M" CHANNEL "$CH" > "$T/human.o" 2>&1
! only_kv "$T/human.o";                               ok $? "control: the human output fails the same format check"

PKG_OUTPUT=machine $PKG LIST ROOT "$M" > "$T/ml.o" 2> "$T/ml.e"
                                                      ok $? "PKG_OUTPUT=machine list exits 0"
only_kv "$T/ml.o" && has "$T/ml.o" '^result: listed$' && has "$T/ml.o" '^package: hello 1.2 ' \
    && has "$T/ml.o" '^count: 1$'
                                                      ok $? "the environment variable selects the same contract"
mrun mv VERIFY hello ROOT "$M" MACHINE
has "$T/mv.o" '^result: intact$';                     ok $? "verify reports intact"
mrun mu UPGRADE hello VERSION 1.3 ROOT "$M" CHANNEL "$CH" MACHINE
has "$T/mu.o" '^result: upgraded$' && has "$T/mu.o" '^from: 1.2$' && has "$T/mu.o" '^version: 1.3$'
                                                      ok $? "upgrade reports from and to"
mrun mr ROLLBACK hello ROOT "$M" CHANNEL "$CH" MACHINE
has "$T/mr.o" '^result: rolled-back$' && has "$T/mr.o" '^version: 1.2$'
                                                      ok $? "rollback reports rolled-back"

refusal() {  # refusal <name> <code> <class> <what>
    rc=$3; name=$1
    only_kv "$T/$name.o" && [ ! -s "$T/$name.e" ] && has "$T/$name.o" '^result: refused$' \
        && has "$T/$name.o" "^class: $4\$" && has "$T/$name.o" "^code: $2\$" \
        && has "$T/$name.o" '^reason: .' && [ "$rc" -eq "$2" ]
    ok $? "$5 exits $2 and says class $4, code $2"
}
mrun x1 INSTALL hello ROOT "$M" CHANNEL "$CH" MACHINE;          refusal x1 15 $? conflict "a second install"
mrun x2 INSTALL nosuch ROOT "$M" CHANNEL "$CH" MACHINE;         refusal x2 11 $? not-found "an unknown package"
mrun x3 UPGRADE hello ROOT "$M" CHANNEL "$CH" MACHINE;          refusal x3 14 $? key "1.4 from another key"
has "$T/x3.o" "^reason: .*$EVILPUB";                  ok $? "the one-line reason still names the new signer"
mrun x4 FROB MACHINE;                                           refusal x4 20 $? usage "an unknown verb"
mrun x6 INSTALL a b MACHINE;                                    refusal x6 20 $? usage "a usage error found before MACHINE"
$PKG LIST ROOT "$M" NAME machine > "$T/nm.o" 2>&1
has "$T/nm.o" 'hello';                                ok $? "NAME machine is a value, and leaves the human output alone"
printf 'edited\n' > "$M/C/Hello"
mrun x5 VERIFY hello ROOT "$M" MACHINE
[ $? -eq 12 ] && has "$T/x5.o" '^result: damaged$' && has "$T/x5.o" '^changed: C/Hello$'
                                                      ok $? "a damaged install exits 12 and names the file"
mrun mx REMOVE hello ROOT "$M" MACHINE
has "$T/mx.o" '^result: removed$' && has "$T/mx.o" '^kept: C/Hello$'
                                                      ok $? "remove reports the edited file it kept"

echo "agent_safety"

# What an agent needs from the tool itself: a next step on every refusal, no
# ready-made command that overrides a safeguard, suggestions instead of
# guesses, dry runs that write nothing, and retries that are harmless.
A="$T/aroot"
$PKG INSTALL hello VERSION 1.2 ROOT "$A" CHANNEL "$CH" > /dev/null 2>&1
$PKG UPGRADE hello ROOT "$A" CHANNEL "$CH" MACHINE > "$T/k14" 2>&1
[ $? -eq 14 ] && has "$T/k14" '^next: ask-requester$' && has "$T/k14" "^signer: $EVILPUB\$" \
    && has "$T/k14" "^pinned: $PUB\$";                ok $? "a key refusal: next ask-requester, pinned and signer as their own fields"
! has "$T/k14" "ACCEPTKEY $EVILPUB";                  ok $? "and no ACCEPTKEY with the key filled in to paste"
$PKG UPGRADE hello ROOT "$A" CHANNEL "$CH" > "$T/k14h" 2>&1
has "$T/k14h" 'next: ask whoever requested this';                ok $? "the human refusal ends with the same next step"
$PKG INSTALL hello VERSION 1.2 ROOT "$A" CHANNEL "$CH" MACHINE > "$T/again" 2>&1
[ $? -eq 0 ] && has "$T/again" '^result: unchanged$'; ok $? "a retried INSTALL of the same version succeeds, unchanged"
$PKG INSTALL hello VERSION 1.3 ROOT "$A" CHANNEL "$CH" MACHINE > "$T/other" 2>&1
[ $? -eq 15 ] && has "$T/other" '^next: use-upgrade$'; ok $? "INSTALL of a newer version than installed: 15, next use-upgrade"
$PKG UPGRADE nosuch ROOT "$A" CHANNEL "$CH" MACHINE > "$T/ni" 2>&1
[ $? -eq 11 ] && has "$T/ni" '^next: use-install$';   ok $? "UPGRADE of what is not installed: 11, next use-install"
$PKG INSTALL hell ROOT "$A" CHANNEL "$CH" MACHINE > "$T/near" 2>&1
[ $? -eq 11 ] && has "$T/near" 'did you mean hello' && has "$T/near" '^next: check-name$'
                                                      ok $? "a near name is suggested, not guessed"
$PKG INSTALL hello --root "$A" MACHINE > "$T/dash" 2>&1
[ $? -eq 20 ] && has "$T/dash" 'perhaps ROOT' && has "$T/dash" '^next: fix-command$'
                                                      ok $? "--root is answered with the Pkg spelling, ROOT"
cp -R "$CH" "$T/chx"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)
" "$T/chx/objects/$payload.pkg"
$PKG INSTALL hello VERSION 1.2 ROOT "$T/xroot" CHANNEL "$T/chx" MACHINE > "$T/x12" 2>&1
[ $? -eq 12 ] && has "$T/x12" '^next: stop$';          ok $? "an integrity refusal says stop"

$PKG INSTALL hello VERSION 1.3 ROOT "$T/droot" CHANNEL "$CH" DRYRUN MACHINE > "$T/di" 2>&1
[ $? -eq 0 ] && has "$T/di" '^result: would-install$' && [ ! -e "$T/droot" ]
                                                      ok $? "INSTALL DRYRUN says would-install and creates nothing"
before=$(cat "$CH/index")
mkdir -p "$T/v15/C"; printf 'binary 5\000$VER: Hello 1.5 (1.10.2026)\000' > "$T/v15/C/Hello"
$PKG PUBLISH "$T/v15" CHANNEL "$CH" KIND application DRYRUN MACHINE > "$T/dp" 2>&1
[ $? -eq 0 ] && has "$T/dp" '^result: would-publish$' && has "$T/dp" '^depends: none$' \
    && has "$T/dp" '^version: 1.5$' && [ "$(cat "$CH/index")" = "$before" ]
                                                      ok $? "PUBLISH DRYRUN shows name, version, no dependencies, and leaves the channel as it was"
$PKG REMOVE hello ROOT "$A" DRYRUN MACHINE > "$T/dr" 2>&1
[ $? -eq 0 ] && has "$T/dr" '^result: would-remove$' && [ -f "$A/C/Hello" ] && [ -f "$A/.pkg/db/hello" ]
                                                      ok $? "REMOVE DRYRUN says would-remove and removes nothing"

echo "first_trust"
# A fresh root trusts no key for hello. The channel's first hello was signed
# by PUB; the highest, 1.4, by EVIL. Trusting EVIL is the person's decision.
$PKG INSTALL hello ROOT "$T/fresh" CHANNEL "$CH" MACHINE > "$T/ft" 2>&1
[ $? -eq 14 ] && has "$T/ft" '^next: ask-requester$' && has "$T/ft" "^first-signer: $PUB\$" \
    && [ ! -e "$T/fresh/C/Hello" ];                  ok $? "a first install signed by another key than the first version: 14, nothing placed"
$PKG INSTALL hello VERSION 1.2 ROOT "$T/fresh" CHANNEL "$CH" MACHINE > "$T/ft2" 2>&1
                                                      ok $? "the version signed by the first version's key installs"
$PKG INSTALL hello ROOT "$T/fresh2" CHANNEL "$CH" ACCEPTKEY "$EVILPUB" MACHINE > "$T/ft3" 2>&1
[ $? -eq 0 ] && has "$T/ft3" '^version: 1.4$';        ok $? "and the newer key once ACCEPTKEY names it"
$PKG SHOW hello CHANNEL "$CH" MACHINE > "$T/fts" 2>&1
has "$T/fts" '^warning: hello is signed by more than one key';
                                                      ok $? "SHOW warns that hello has more than one signer"
env -u PKG_SIGNKEY $PKG PUBLISH "$D" CHANNEL "$T/nokeych" KIND application MACHINE > "$T/nk" 2>&1
[ $? -eq 14 ] && has "$T/nk" '^next: ask-requester$' && has "$T/nk" 'same key';
                                                      ok $? "no signing key: the refusal says to find the publisher's key, not make one"
$PKG KEYINFO FILE "$KEY" MACHINE > "$T/ki" 2>&1
[ $? -eq 0 ] && has "$T/ki" "^public: $PUB\$" && ! has "$T/ki" 'Seed';
                                                      ok $? "KEYINFO names the public key a key file holds, and nothing else"

echo "trace"
TR="$T/troot"
$PKG INSTALL hello VERSION 1.2 ROOT "$TR" CHANNEL "$CH" MACHINE TRACE "$T/trace.log" > "$T/tro" 2>"$T/tre"
[ $? -eq 0 ] && ! LC_ALL=C grep -a -v -E '^[a-z][a-z-]*: ' "$T/tro" > /dev/null && [ ! -s "$T/tre" ]
                                                      ok $? "TRACE leaves stdout the pure MACHINE contract, and stderr empty"
has "$T/trace.log" 'picked hello 1.2: the version asked for' && has "$T/trace.log" 'signature verifies, signer' \
    && has "$T/trace.log" "move .* -> $TR/C/Hello"; ok $? "the trace tells the choice, the checks and each file moved"
PKG_TRACE="$T/env.log" $PKG INSTALL hello VERSION 1.2 ROOT "$T/xr2" CHANNEL "$T/chx" > /dev/null 2>&1
has "$T/env.log" 'refused, integrity (12), next stop';  ok $? "PKG_TRACE works too, and a refusal is traced with its class"
$PKG LIST ROOT "$TR" TRACE > "$T/tru" 2>&1
[ $? -eq 20 ];                                        ok $? "TRACE without a file is a usage error"

echo "guidance"
$PKG PUBLISH "$D" CHANNEL "$T/gch" MACHINE > "$T/g1" 2>&1
[ $? -eq 20 ] && has "$T/g1" 'no KIND given' && has "$T/g1" 'KIND image' && has "$T/g1" '^next: fix-command$' \
    && [ ! -e "$T/gch" ];                             ok $? "PUBLISH without KIND is refused with the kinds listed, and creates nothing"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND handler MACHINE > "$T/g2" 2>&1
[ $? -eq 20 ] && has "$T/g2" 'the kind for that is device'; ok $? "KIND handler is refused with the kind to use"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND application DRYRUN MACHINE > "$T/g3" 2>&1
has "$T/g3" '^hint: there is no channel at .* publishing creates it' && [ ! -e "$T/gch" ]
                                                      ok $? "a dry run into a missing channel says it would create it"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND application MACHINE > "$T/g4" 2>&1
has "$T/g4" '^hint: the channel .* did not exist and was created'; ok $? "and the publish says it did"
$PKG PUBLISH "$D" CHANNEL "$T/gch" KIND application MACHINE > "$T/g5" 2>&1
! has "$T/g5" '^hint: the channel';                   ok $? "no such hint once the channel exists"
mkdir -p "$T/gscript/S"; printf 'Echo hi\n' > "$T/gscript/S/Go"
$PKG PUBLISH "$T/gscript" CHANNEL "$T/gch" NAME go VERSION 1 KIND application DRYRUN MACHINE > "$T/g6" 2>&1
has "$T/g6" '^warning: no executable in the drawer'; ok $? "an application with no executable is warned about"
$PKG PUBLISH "$T/gscript" CHANNEL "$T/gch" NAME go VERSION 1 KIND data DRYRUN MACHINE > "$T/g7" 2>&1
! has "$T/g7" '^warning:';                            ok $? "data with no executable is not"
$PKG PUBLISH "$D" CHANNEL "$T/gch" NAME himg VERSION 1 KIND image DRYRUN MACHINE > "$T/g8" 2>&1
has "$T/g8" '^content: C/Hello [0-9]' && has "$T/g8" '^file: himg.hdf ';
                                                      ok $? "an image dry run lists the files that go into the image"
$PKG KEYGEN FILE "$T/gkey" MACHINE > "$T/g9" 2>&1
has "$T/g9" '^hint: every later version .* signed with it'; ok $? "KEYGEN says to keep the key"
$PKG PUBLISH "$D" CHANNEL "$T/gch" NAME himg VERSION 1 KIND image > /dev/null 2>&1
$PKG INSTALL himg ROOT "$T/groot" CHANNEL "$T/gch" MACHINE > "$T/g10" 2>&1
has "$T/g10" '^hint: to run it, mount the image: MOUNTLIST himg'; ok $? "installing an image says how to run it"
$PKG MOUNTLIST himg ROOT "$T/groot" MACHINE > "$T/g11" 2>&1
has "$T/g11" '^hint: Mount reads the entry from a file' && has "$T/g11" '^hint: no FFS handler';
                                                      ok $? "MOUNTLIST without OUT says to save it, and warns of the missing handler"

echo "wrong_program"
# A game drawer holding another program by mistake: its cookie names that program.
mkdir -p "$T/game/C" "$T/wch"
cp "$D/C/Hello" "$T/game/C/Asteroids"
$PKG PUBLISH "$T/game" CHANNEL "$T/wch" KIND image NAME asteroids VERSION 1.0 DRYRUN MACHINE > "$T/w1" 2>&1
has "$T/w1" '^warning: C/Asteroids carries the \$VER cookie of hello'
                                                      ok $? "an executable whose cookie names another program is warned about"
$PKG PUBLISH "$D" CHANNEL "$T/wch" KIND application MACHINE > /dev/null 2>&1
$PKG PUBLISH "$T/game" CHANNEL "$T/wch" KIND image VERSION 9 MACHINE > "$T/w2" 2>&1
[ $? -eq 20 ] && has "$T/w2" 'comes from the \$VER cookie in C/Asteroids' && has "$T/w2" 'add NAME' \
    && [ "$(grep -c . "$T/wch/index")" = 1 ]; ok $? "a cookie name that would replace a package of another kind is refused"
$PKG PUBLISH "$D" CHANNEL "$T/wch" KIND application NAME hello2 VERSION 1 DRYRUN MACHINE > "$T/w3" 2>&1
! has "$T/w3" '^warning: C/Hello carries';            ok $? "a cookie that matches its file name raises nothing"
$PKG SHOW nosuch CHANNEL "$T/wch" MACHINE > "$T/w4" 2>&1
[ $? -eq 0 ] && has "$T/w4" '^hint: no package is published as nosuch'; ok $? "SHOW of a name not published says so"

echo "next_version"
# A teammate's 2.1 that is the published 2.0 with a byte added, its $VER not raised.
NV="$T/nv"; mkdir -p "$NV/v2/C" "$NV/v21/C"
printf 'bin\000$VER: Tool 2.0 (1.1.2026)\000' > "$NV/v2/C/Tool"; printf 'data\n' > "$NV/v2/C/Tool.cfg"
cp -R "$NV/v2/C" "$NV/v21/"; printf 'x' >> "$NV/v21/C/Tool"
$PKG PUBLISH "$NV/v2" CHANNEL "$NV/ch" KIND image MACHINE > /dev/null 2>&1
grep -q '^Content: [0-9a-f]* [0-9]* C/Tool$' "$NV/ch/objects/"*.manifest
                                                      ok $? "an image's manifest lists the files inside it"
env -u PKG_SIGNKEY $PKG PUBLISH "$NV/v21" CHANNEL "$NV/ch" KIND image VERSION 2.1 DRYRUN MACHINE > "$T/n1" 2>&1
[ $? -eq 0 ] && has "$T/n1" '^result: would-publish$' && has "$T/n1" '^signer: none$' \
    && has "$T/n1" "^first-signer: $PUB\$" && has "$T/n1" '^hint: the real publish must be signed with the key that signed tool 2.0'
                                                      ok $? "a dry run needs no key, and names the key the real publish needs"
has "$T/n1" '^compared-with: tool 2.0$' && has "$T/n1" '^changed: C/Tool [0-9]* [0-9]*$' && has "$T/n1" '^same: C/Tool.cfg$'
                                                      ok $? "and compares the new version with the last, file by file"
has "$T/n1" '^warning: the \$VER cookie in C/Tool says 2.0, the version already published' \
    && ! has "$T/n1" '^version-from:' && has "$T/n1" '^name-from: C/Tool$'
                                                      ok $? "a \$VER not raised is named as such; the version is not claimed to come from it"
$PKG PUBLISH "$NV/v2" CHANNEL "$NV/ch" KIND image VERSION 2.2 DRYRUN MACHINE > "$T/n2" 2>&1
has "$T/n2" '^warning: every file is identical to tool 2.0'; ok $? "a new version with nothing changed is warned about"
$PKG INSTALL tool ROOT "$NV/r" CHANNEL "$NV/ch" > /dev/null 2>&1
$PKG PUBLISH "$NV/v21" CHANNEL "$NV/ch" KIND image VERSION 2.1 > /dev/null 2>&1
$PKG UPGRADE tool ROOT "$NV/r" CHANNEL "$NV/ch" MACHINE > "$T/n3" 2>&1
has "$T/n3" '^hint: the image tool.hdf is replaced: a machine that has it mounted must Eject it'
                                                      ok $? "UPGRADE of an image says to eject it and mount it again"

echo "moved_by_hand"
# The Amiga tradition: an installed drawer moved elsewhere by the person.
MV="$T/mv"; mkdir -p "$MV/d/Tool/C"
printf 'x\000$VER: tool 1.0 (1.1.2026)\000' > "$MV/d/Tool/C/Tool"; printf 'doc' > "$MV/d/Tool/ReadMe"
$PKG PUBLISH "$MV/d" CHANNEL "$MV/ch" KIND application > /dev/null 2>&1
$PKG INSTALL tool ROOT "$MV/r" CHANNEL "$MV/ch" > /dev/null 2>&1
mkdir -p "$MV/r/Work"; mv "$MV/r/Tool" "$MV/r/Work/"
$PKG VERIFY tool ROOT "$MV/r" MACHINE > "$T/mv1" 2>&1
[ $? -eq 0 ] && has "$T/mv1" '^result: moved$' && has "$T/mv1" '^moved: Tool/C/Tool Work/Tool/C/Tool$' \
    && ! has "$T/mv1" '^missing:';                   ok $? "VERIFY reports a hand-moved drawer as moved, not damaged"
$PKG LIST ROOT "$MV/r" MACHINE | grep -q '^package: tool 1.0 '; ok $? "and the package stays listed"
printf 'y' >> "$MV/r/Work/Tool/ReadMe"
$PKG VERIFY tool ROOT "$MV/r" MACHINE > "$T/mv2" 2>&1
[ $? -eq 12 ] && has "$T/mv2" '^moved: Tool/C/Tool' && has "$T/mv2" '^missing: Tool/ReadMe$'
                                                      ok $? "a moved file that was also changed is not taken for the package's"
$PKG REMOVE tool ROOT "$MV/r" > /dev/null 2>&1
[ -f "$MV/r/Work/Tool/C/Tool" ];                      ok $? "REMOVE leaves the moved files where the person put them"

echo
echo "$checks checks, $fails failures"
[ "$fails" -eq 0 ]
