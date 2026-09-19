#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# A portal that no longer takes this Pkg says so, and Pkg shows its words:
# on a read and on a push, while the channel Pkg comes from stays readable.
# Needs: make; the portal built.
set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}; dll="$repo_root/portal/src/Portal/bin/Release/net10.0/Portal.dll"
export PATH="$HOME/.dotnet:$PATH"
command -v dotnet > /dev/null && [ -f "$dll" ] || { echo "min-version: the portal is not built; skipped" >&2; exit 0; }
T=$(mktemp -d); site="http://127.0.0.1:5083"; export PKG_CACHE="$T/cache"; unset PKG_PUSHKEY PKG_SIGNKEY
trap 'kill $srv 2>/dev/null; rm -rf "$T" 2>/dev/null' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
"$P" KEYGEN FILE "$T/k" > /dev/null; pub=$("$P" KEYINFO FILE "$T/k" MACHINE | awk '/^public:/{print $2}')
mkdir -p "$T/d/C"; printf 'x\000$VER: Thing 1.0 (19.9.2026)\000' > "$T/d/C/Thing"
for ch in pkg other; do "$P" PUBLISH "$T/d" CHANNEL "$T/data/channels/$ch" KIND data ARCH generic SIGN "$T/k" > /dev/null 2>&1; done
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" Portal__Pinned=pkg/pkg \
  Portal__SignedKeys="t:$pub:*:files" Portal__Policy__MinPkg=99.0 Portal__Policy__MinPkgReads=true \
  ASPNETCORE_URLS="$site" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done
out=$("$P" SHOW CHANNEL "$site/other" 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q 'works with Pkg 99.0 or later' && echo "$out" | grep -q 'UPGRADE pkg'
ok $? "a read is refused with the portal's words: the version wanted and how to update"
"$P" SHOW CHANNEL "$site/pkg" 2>&1 | grep -q '^thing .* ok ';    ok $? "the channel Pkg comes from stays readable"
out=$("$P" PUSH CHANNEL "$T/data/channels/other" TO "$site/other" SIGN "$T/k" 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q 'works with Pkg 99.0 or later';  ok $? "a push is refused the same way"
curl -fs -A 'Mozilla/5.0' "$site/other/index" | grep -q '^thing ';  ok $? "a browser still reads the channel"
echo; echo "min-version: $checks checks, $fails failures"; [ "$fails" -eq 0 ]
