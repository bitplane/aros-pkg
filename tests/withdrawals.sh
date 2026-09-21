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
# A portal built before the sources it is tested against fails checks that
# have nothing wrong with them: say so rather than let it look like a defect.
# Sources only: obj/ and bin/ hold files a build writes itself (a deploy's
# publish step rewrites Portal.AssemblyInfo.cs), which would make a current
# binary look stale.
newer=$(find "$repo_root/portal/src/Portal" \( -name obj -o -name bin \) -prune -o \
        \( -name '*.cs' -o -name '*.cshtml' \) -newer "$dll" -print -quit)
[ -z "$newer" ] || { echo "withdrawals: $dll is older than $newer; rebuild it (dotnet build portal/src/Portal -c Release)" >&2; exit 69; }
T=$(mktemp -d); port=5083; site="http://127.0.0.1:$port"
trap 'kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
unset PKG_PUSHKEY PKG_SIGNKEY; export PKG_CACHE="$T/cache"

"$P" KEYGEN FILE "$T/pub.key" > /dev/null
pub=$("$P" KEYINFO FILE "$T/pub.key" MACHINE | awk '/^public:/{print $2}')
mkdir -p "$T/d/C"
for v in 1.0 1.1 1.2 1.3; do
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

# ---- the reader's side: the list is a hint, and its absence is not an answer.
# A copy of the channel served as a plain directory over http, so the requests
# pkg makes can be counted in its own trace.
cp -R "$T/ch" "$T/copy"
python3 -m http.server --bind 127.0.0.1 --directory "$T/copy" 5084 > /dev/null 2>&1 &
web=$!
for i in $(seq 1 40); do curl -fs http://127.0.0.1:5084/index > /dev/null && break; sleep 1; done
copy=http://127.0.0.1:5084

probes() {  # how many .withdrawn files a read of the whole channel asked for
    rm -rf "$T/cache"
    "$P" SHOW CHANNEL "$1" METADATA TRACE - > "$T/tr" 2>&1
    grep -c '\.withdrawn ' "$T/tr"
}
withdrawn_seen() {
    rm -rf "$T/cache"
    "$P" SHOW tool CHANNEL "$1" MACHINE 2>&1 | grep -q '^entry: tool 1.0 data generic withdrawn'
}

[ ! -f "$T/copy/withdrawals" ];                         ok $? "the copy serves no list, as a drawer or an older portal would"
withdrawn_seen "$copy";                                 ok $? "without a list, the withdrawal is still found: absence is not an answer"
noList=$(probes "$copy")
withList=$(probes "$site/tools")
[ "$withList" -lt "$noList" ];                          ok $? "with a list, fewer files are asked for ($withList against $noList)"
withdrawn_seen "$site/tools";                           ok $? "and the withdrawal the list names is still read from its signed file"

# A list is read again on the next run, as the index is: it changes whenever
# a publisher withdraws something, and a copy kept from an earlier run would
# answer "nothing withdrawn" for a version that has since been withdrawn.
printf 'Format: pkg-withdrawals 1\n' > "$T/copy/withdrawals"
rm -rf "$T/cache"
"$P" SHOW CHANNEL "$copy" > /dev/null 2>&1                     # reads the empty list and keeps it
wd=$(awk '$1=="tool" && $2=="1.0"{print $4}' "$T/copy/index")
printf 'Format: pkg-withdrawals 1\n%s\n' "$wd" > "$T/copy/withdrawals"
"$P" SHOW tool CHANNEL "$copy" MACHINE 2>&1 | grep -q '^entry: tool 1.0 data generic withdrawn'
ok $? "a list read in an earlier run is read again, so a new withdrawal is seen"

printf 'Format: pkg-withdrawals 2\n' > "$T/copy/withdrawals"
withdrawn_seen "$copy";                                 ok $? "a list in a format this pkg does not read is ignored, not believed"
[ "$(probes "$copy")" -eq "$noList" ];                  ok $? "and the versions are asked about one by one, as before"

# A list that leaves a withdrawal out hides it. That is the same power as
# deleting the signed file from that copy, which whoever serves a channel
# always had, so it adds none; it is checked here so it stays deliberate.
printf 'Format: pkg-withdrawals 1\n' > "$T/copy/withdrawals"
withdrawn_seen "$copy"
[ $? -ne 0 ];                                           ok $? "a channel that leaves a withdrawal out of its list hides it, as deleting the file would"
kill $web 2>/dev/null; wait $web 2>/dev/null

echo "withdrawals: $((checks - fails))/$checks checks passed"
[ "$fails" -eq 0 ]
