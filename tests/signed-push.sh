#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# A signed push, against the real portal on this machine over plain http: no
# portal key anywhere, each request signed with the publisher's key. What must
# pass passes, and what an attacker on the wire could try is refused: another
# key, a channel the key may not push to, a forged signature, the same request
# sent twice, a body changed after signing, and a portal key sent in clear.
#
# Needs: make; the portal built (dotnet build portal/src/Portal -c Release).

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
dll="$repo_root/portal/src/Portal/bin/Release/net10.0/Portal.dll"
export PATH="$HOME/.dotnet:$PATH"
command -v dotnet > /dev/null && [ -f "$dll" ] || { echo "signed-push: the portal is not built; skipped" >&2; exit 0; }
T=$(mktemp -d); port=5079; site="http://127.0.0.1:$port"
trap 'kill $srv 2>/dev/null; rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
unset PKG_PUSHKEY PKG_SIGNKEY; export PKG_CACHE="$T/cache"

"$P" KEYGEN FILE "$T/pub.key" > /dev/null; "$P" KEYGEN FILE "$T/link.key" > /dev/null; "$P" KEYGEN FILE "$T/other.key" > /dev/null
pub=$("$P" KEYINFO FILE "$T/pub.key" MACHINE | awk '/^public:/{print $2}')
link=$("$P" KEYINFO FILE "$T/link.key" MACHINE | awk '/^public:/{print $2}')
mkdir -p "$T/d/C"; printf 'x\000$VER: Sgn 1.0 (19.9.2026)\000' > "$T/d/C/Sgn"
"$P" PUBLISH "$T/d" CHANNEL "$T/ch" KIND data ARCH generic SIGN "$T/pub.key" > /dev/null 2>&1
"$P" PUBLISH "$T/d" CHANNEL "$T/chl" KIND data ARCH generic SIGN "$T/link.key" > /dev/null 2>&1
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" \
  Portal__SignedKeys="tester:$pub:sgn:files;linker:$link:lnk" ASPNETCORE_URLS="$site" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done

out=$("$P" PUSH CHANNEL "$T/ch" TO "$site/sgn" SIGN "$T/pub.key" 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$out" | grep -q 'published sgn 1.0';      ok $? "a signed push over plain http publishes, with no portal key"
"$P" SHOW CHANNEL "$site/sgn" 2>&1 | grep -q '^sgn .* ok ';      ok $? "and the channel reads back: ok"
out=$("$P" PUSH CHANNEL "$T/ch" TO "$site/sgn" SIGN "$T/other.key" 2>&1); rc=$?
[ $rc -eq 14 ] && echo "$out" | grep -q 'does not know that signing key'; ok $? "a key the portal does not know gets no session (14)"
out=$("$P" PUSH CHANNEL "$T/ch" TO "$site/elsewhere" SIGN "$T/pub.key" 2>&1); rc=$?
[ $rc -eq 14 ] && echo "$out" | grep -q 'may not push to elsewhere';  ok $? "a channel the key may not push to is refused (14)"
out=$("$P" PUSH CHANNEL "$T/chl" TO "$site/lnk" SIGN "$T/link.key" 2>&1)
echo "$out" | grep -q -i 'Binaries\|files right\|link';           ok $? "a signed key without the files right is link-only, like a portal key"

# requests made by hand: SIGN writes the same Ed25519 signature PUSH sends
session() { curl -s -X POST --data "key: $pub" "$site/sgn/_push/session" | awk '/^session:/{print $2}'; }
signed() {  # signed <session> <seq> <body signed> <body sent>
    d=$(printf '%s' "$3" | shasum -a 256 | cut -d' ' -f1)
    printf 'pkg-push-1\nPOST\n/sgn/_push/plan\n%s\n%s\n%s\n-\n' "$1" "$2" "$d" > "$T/req"
    "$P" SIGN "$T/req" KEY "$T/pub.key" OUT "$T/req.sig" > /dev/null
    sig=$(awk '/^Signature:/{print $2}' "$T/req.sig"); rm -f "$T/req.sig"
    curl -s -X POST -H "Authorization: Pkg-Signature key=$pub,session=$1,seq=$2,sha256=$d,sig=$sig" --data-binary "$4" "$site/sgn/_push/plan"
}
s=$(session)
signed "$s" 1 "" "" | grep -q '^result: plan\|^need\|^summary';  ok $? "a request signed by hand is taken"
signed "$s" 1 "" "" | grep -q 'already sent once';               ok $? "the same request sent again is refused: no replay"
signed "$s" 2 "" "objects/x 0 1" | grep -q 'body does not check';  ok $? "a body changed after signing is refused"
z=$(printf '0%.0s' $(seq 1 128))
e=$(printf "" | shasum -a 256 | cut -d" " -f1)
curl -s -X POST -H "Authorization: Pkg-Signature key=$pub,session=$s,seq=9,sha256=$e,sig=$z" --data "" "$site/sgn/_push/plan" | grep -q 'does not check'
ok $? "a forged signature is refused"
signed "$s" 3 "" "" | grep -q '^result: plan\|^need\|^summary';  ok $? "and none of those used up the session: the next good request passes"
signed "$(printf '0%.0s' $(seq 1 32))" 1 "" "" | grep -q 'unknown or over';  ok $? "a session the portal did not give is refused"
curl -s -X POST -H "Authorization: Bearer whatever" --data "" "$site/sgn/_push/plan" | grep -q 'never travel in clear'
ok $? "a portal key over plain http is still refused"

echo
echo "signed-push: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
