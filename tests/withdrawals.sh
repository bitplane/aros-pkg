#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The list of withdrawn versions the portal serves beside the index. A reader
# that has it asks for a signed withdrawal only where there is one, instead of
# asking once per entry; what the list says is still proven by the signed file.
#
# Needs: make; the portal built (dotnet build portal/src/Portal -c Release).

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
dll="$repo_root/portal/src/Portal/bin/Release/net10.0/Portal.dll"
export PATH="$HOME/.dotnet:$PATH"
command -v dotnet > /dev/null && [ -f "$dll" ] || { echo "withdrawals: the portal is not built; skipped" >&2; exit 0; }
T=$(mktemp -d); port=5083; site="http://127.0.0.1:$port"
trap 'kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
unset PKG_PUSHKEY PKG_SIGNKEY; export PKG_CACHE="$T/cache"

"$P" KEYGEN FILE "$T/pub.key" > /dev/null
pub=$("$P" KEYINFO FILE "$T/pub.key" MACHINE | awk '/^public:/{print $2}')
mkdir -p "$T/d/C"
for v in 1.0 1.1; do
    printf 'x\000$VER: tool %s (19.9.2026)\000' "$v" > "$T/d/C/tool"
    "$P" PUBLISH "$T/d" CHANNEL "$T/ch" KIND data ARCH generic SIGN "$T/pub.key" > /dev/null 2>&1
done
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" \
  Portal__SignedKeys="tester:$pub:tools,quiet:files" ASPNETCORE_URLS="$site" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done

"$P" PUSH CHANNEL "$T/ch" TO "$site/tools" SIGN "$T/pub.key" > /dev/null 2>&1
                                                        ok $? "two versions are published"

curl -fs "$site/tools/withdrawals" > "$T/w0" 2>/dev/null
[ $? -eq 0 ] && [ "$(head -1 "$T/w0")" = "Format: pkg-withdrawals 1" ]
                                                        ok $? "the channel serves a list, named by its format"
[ "$(sed 1d "$T/w0" | wc -l | tr -d ' ')" = "0" ];      ok $? "with nothing withdrawn, the list is empty: no file to ask for"

# The publisher withdraws 1.0 in his own channel and pushes it.
"$P" WITHDRAW tool VERSION 1.0 CHANNEL "$T/ch" SIGN "$T/pub.key" > /dev/null 2>&1
                                                        ok $? "the publisher withdraws 1.0"
"$P" PUSH CHANNEL "$T/ch" TO "$site/tools" SIGN "$T/pub.key" > "$T/p1" 2>&1
[ $? -eq 0 ] && grep -q 'withdrawn' "$T/p1";            ok $? "the push carries the withdrawal"

w=$(awk '$1=="tool" && $2=="1.0"{print $4}' "$T/ch/index")
o=$(awk '$1=="tool" && $2=="1.1"{print $4}' "$T/ch/index")
curl -fs "$site/tools/withdrawals" > "$T/w1"
[ "$(sed 1d "$T/w1")" = "$w" ];                         ok $? "the list names the withdrawn version, and only it"
curl -fs "$site/tools/objects/$w.withdrawn" > /dev/null;      ok $? "what it names has its signed withdrawal"
curl -fs "$site/tools/objects/$w.withdrawn.sig" > /dev/null;  ok $? "and that withdrawal's signature"
curl -s -o /dev/null -w '%{http_code}' "$site/tools/objects/$o.withdrawn" | grep -q 404
                                                        ok $? "the version it does not name has none (404)"

# The list is the server's own account of what it holds. A publisher cannot
# write one, so a copied channel cannot be made to hide a withdrawal by push.
code=$(curl -s -o "$T/r" -w '%{http_code}' -X PUT --data-binary 'Format: pkg-withdrawals 1' "$site/tools/_push/files/withdrawals")
[ "$code" != "200" ] && [ "$code" != "204" ];           ok $? "a push may not write the list itself ($code)"

# It is a hint, never authority: the signed file still decides.
"$P" SHOW tool CHANNEL "$site/tools" MACHINE 2>&1 | grep -q '^entry: tool 1.0 data generic withdrawn'
                                                        ok $? "pkg reads 1.0 as withdrawn from the signed file"
out=$("$P" INSTALL tool VERSION 1.0 CHANNEL "$site/tools" ROOT "$T/root" 2>&1); rc=$?
[ $rc -eq 18 ] && echo "$out" | grep -qi 'withdrawn';   ok $? "and refuses to install it (18)"

echo "withdrawals: $((checks - fails))/$checks checks passed"
[ "$fails" -eq 0 ]
