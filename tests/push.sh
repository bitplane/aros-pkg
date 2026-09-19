#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# PUSH against tests/push_server.py, a stand-in for the portal's push API:
# a local channel goes up (plan, only the files needed, large ones in parts,
# commit checked with Pkg itself), is read back by URL and installed; a
# second push sends nothing; a wrong key and a tampered manifest are
# refused; an upload cut in the middle resumes where it stopped.

set -u
PKG=${PKG:-./build/pkg}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
here=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-push.XXXXXX")
server=
stop() { [ -z "$server" ] || { kill "$server" 2>/dev/null; wait "$server" 2>/dev/null; }; server=; }
trap 'stop; rm -rf "$T"' EXIT
cd "$T" || exit 1
export COPYFILE_DISABLE=1 PKG_CACHE="$T/cache" PKG_PUSHKEY=testkey
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }
start() {
    rm -f port
    python3 "$here/push_server.py" "$T/portal" "$PKG" "$T/port" "$T/log" &
    server=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -s port ] && break; sleep 0.3; done
    U="http://127.0.0.1:$(cat port)/pkg"
}
mkdir -p portal
start

$PKG KEYGEN FILE key > /dev/null
export PKG_SIGNKEY="$T/key"
mkdir -p d/C
printf 'x\000$VER: hello 1.0 (1.1.2026)\000' > d/C/Hello
$PKG PUBLISH d CHANNEL local KIND application > /dev/null
# an archive source, large enough to go in parts
mkdir -p arc/Top/Extras/Big
dd if=/dev/urandom of=arc/Top/Extras/Big/Data bs=1024 count=300 2>/dev/null
printf 'x\000$VER: big 1.0 (1.1.2026)\000' > arc/Top/Extras/Big/Big
(cd arc && tar -cf ../big.tar Top)
mkdir -p local/archives && cp big.tar local/archives/
$PKG PUBLISH "local/archives/big.tar!/Top" FILES Extras/Big CHANNEL local NAME big BUILD 20260919 KIND application > /dev/null

echo "push"
: > log
PKG_PUSH_PART_BYTES=100000 $PKG PUSH CHANNEL local TO "$U" MACHINE > o1 2>&1
code=$?
[ $code -eq 0 ] && has o1 '^result: published$' && has o1 '^published: hello 1.0 ' && has o1 '^published: big '
                                                      ok $? "a local channel is pushed and published (exit $code)"
[ "$(grep -c 'Content-Range: bytes' log)" -ge 3 ];           ok $? "the archive went up in parts"
! grep -q 'pkgidx' log;                               ok $? "the publisher-side archive index is never sent"
has o1 '^summary: ';                                  ok $? "the answer carries a sentence"
$PKG INSTALL hello ROOT r CHANNEL "$U" MACHINE > o2 2>&1
[ $? -eq 0 ] && [ -f r/C/Hello ];                     ok $? "what was pushed installs from the portal by URL"
$PKG INSTALL big ROOT r CHANNEL "$U" MACHINE > o3 2>&1
[ $? -eq 0 ] && cmp -s r/Extras/Big/Data arc/Top/Extras/Big/Data;  ok $? "and so does the archive package"
: > log
$PKG PUSH CHANNEL local TO "$U" MACHINE > o4 2>&1
[ $? -eq 0 ] && has o4 '^result: unchanged$' && ! grep -q '^PUT' log && has o4 '^uploaded: 0$'
                                                      ok $? "a second push sends no file and changes nothing"

echo "upstream"
# An archive its packages download from where its makers publish it stays off the portal.
mkdir -p uparc/Top/Extras/Up && printf 'x\000$VER: up 1.0 (1.1.2026)\000' > uparc/Top/Extras/Up/Up
(cd uparc && tar -cf ../local/archives/up.tar Top)
$PKG PUBLISH "local/archives/up.tar!/Top" FILES Extras/Up CHANNEL local NAME up BUILD 20260919 KIND application UPSTREAM "https://downloads.example.org/up.tar" > /dev/null
: > log
$PKG PUSH CHANNEL local TO "$U" MACHINE > o4b 2>&1
[ $? -eq 0 ] && has o4b '^published: up ' && ! grep -q 'archives/up.tar' log && [ ! -e portal/pkg/archives/up.tar ]
                                                      ok $? "PUSH publishes the package and leaves its upstream archive out"

echo "bootstrap"
mkdir -p local/Bootstrap/x86_64 local/Bootstrap/macos-arm64 local/Bootstrap/windows-x86_64 local/Bootstrap/bad
printf 'aros' > local/Bootstrap/x86_64/Pkg; printf 'mac' > local/Bootstrap/macos-arm64/pkg
printf 'win' > local/Bootstrap/windows-x86_64/pkg.exe; printf 'no' > local/Bootstrap/bad/other
: > log
$PKG PUSH CHANNEL local TO "$U" MACHINE > o4c 2>&1
[ $? -eq 0 ] && grep -q 'files/Bootstrap/x86_64/Pkg' log && grep -q 'files/Bootstrap/macos-arm64/pkg' log \
  && grep -q 'files/Bootstrap/windows-x86_64/pkg.exe' log && ! grep -q 'Bootstrap/bad' log
                                                      ok $? "PUSH sends the AROS and the host builds of Pkg, and nothing else under Bootstrap"

echo "refusals"
PKG_PUSHKEY=wrong $PKG PUSH CHANNEL local TO "$U" MACHINE > o5 2>&1
[ $? -eq 14 ] && has o5 'refused the key';            ok $? "a wrong key is refused with 14"
env -u PKG_PUSHKEY $PKG PUSH CHANNEL local TO "$U" MACHINE > o6 2>&1
[ $? -eq 14 ] && has o6 'PKG_PUSHKEY';               ok $? "no key: refused, naming where the key goes"
$PKG PUSH CHANNEL local TO "http://example.com/pkg" MACHINE > o7 2>&1
[ $? -eq 20 ] && has o7 'only over https';            ok $? "a key never goes over plain http to another machine"
mkdir -p d2/C; printf 'x\000$VER: hello 1.1 (1.1.2026)\000' > d2/C/Hello
$PKG PUBLISH d2 CHANNEL local KIND application > /dev/null
m=$(ls -t local/objects/*.manifest | head -1)
printf 'Kind: data\n' >> "$m"                         # changed after signing
mv "$m" "local/objects/$(shasum -a 256 "$m" | cut -d' ' -f1).manifest" 2>/dev/null
d=$(awk '$1=="hello" && $2=="1.1"{print $4}' local/index)
nd=$(ls -t local/objects/*.manifest | head -1 | xargs basename | cut -d. -f1)
mv "local/objects/$d.sig" "local/objects/$nd.sig"
sed -i '' "s|$d|$nd|" local/index
$PKG PUSH CHANNEL local TO "$U" MACHINE > o8 2>&1
[ $? -eq 12 ] && has o8 '^result: refused$' && ! grep -q ' 1.1 ' portal/pkg/index
                                                      ok $? "a manifest changed after signing is refused by the portal's check, nothing published"

echo "resume"
stop
rm -rf portal local/objects/$nd.* && sed -i '' "/ $nd\$/d" local/index
export PKG_TEST_FAIL_PART=2
start
: > log
PKG_PUSH_PART_BYTES=100000 $PKG PUSH CHANNEL local TO "$U" MACHINE > o9 2>&1
[ $? -eq 17 ];                                        ok $? "an upload cut in the middle is refused with 17"
first=$(grep -c 'Content-Range: bytes' log)
: > log
PKG_PUSH_PART_BYTES=100000 $PKG PUSH CHANNEL local TO "$U" MACHINE > o10 2>&1
[ $? -eq 0 ] && has o10 '^result: published$';       ok $? "pushing again finishes it"
grep -q 'Content-Range: bytes 0-' log && [ "$(grep -c 'Content-Range: bytes' log)" -le 4 ]
                                                      ok $? "and resumes where the portal says it got to ($first parts, then $(grep -c 'Content-Range: bytes' log))"

echo
echo "push: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
