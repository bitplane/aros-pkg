#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# PUSH from AROS itself: hosted AROS publishes a drawer and pushes it, over
# plain http with signed requests, to the real portal running on this machine;
# the host then reads the portal's channel. No portal key exists anywhere.
#
# Needs: make; tools/build-aros.sh; the portal built; hosted AROS with
# bsdsocket.library (see tests/aros-network.sh).

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
P="$repo_root/build/pkg"
dll="$repo_root/portal/src/Portal/bin/Release/net10.0/Portal.dll"
export PATH="$HOME/.dotnet:$PATH"
for need in "$P" "$repo_root/build/aros/Pkg" "$control" "$dll"; do
    [ -e "$need" ] || { echo "aros-push: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || { echo "aros-push: a hosted AROS instance is running; refusing to disturb it" >&2; exit 75; }
T=$(mktemp -d); share="$T/share"; port=5081; site="http://127.0.0.1:$port"; srv=; started=0
trap '[ $started = 0 ] || "$control" stop > /dev/null 2>&1; kill $srv 2>/dev/null; [ "${KEEP:-0}" = 1 ] && echo "kept $T" >&2 || rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }

mkdir -p "$share/out" "$share/bin" "$share/src/Tool/C"
cp "$repo_root/build/aros/Pkg" "$share/bin/Pkg"; cp "$repo_root/build/aros/Pkg" "$share/src/Tool/C/Tool"
"$P" KEYGEN FILE "$share/my.key" > /dev/null
pub=$("$P" KEYINFO FILE "$share/my.key" MACHINE | awk '/^public:/{print $2}')
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" \
  Portal__SignedKeys="arospublisher:$pub:fromaros:files" ASPNETCORE_URLS="$site" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done

script="FailAt 99
MacRW:bin/Pkg PUBLISH MacRW:src/Tool CHANNEL MacRW:channel NAME tool KIND application SIGN MacRW:my.key >MacRW:out/publish.o
C:Echo \"\$RC\" >MacRW:out/publish.rc
MacRW:bin/Pkg PUSH CHANNEL MacRW:channel TO $site/fromaros SIGN MacRW:my.key >MacRW:out/push.o
C:Echo \"\$RC\" >MacRW:out/push.rc
MacRW:bin/Pkg PUSH CHANNEL MacRW:channel TO https://127.0.0.1/fromaros SIGN MacRW:my.key >MacRW:out/https.o
C:Echo \"\$RC\" >MacRW:out/https.rc
C:Echo done >MacRW:done"
echo "aros-push: hosted AROS"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$script" "$control" run > /dev/null 2>&1; started=1
w=0; while [ ! -f "$share/done" ] && [ "$w" -lt 240 ]; do sleep 1; w=$((w + 1)); done
crash=$("$control" crash 2> /dev/null | grep -c -i -E 'stack limits|ALERT')
"$control" stop > /dev/null 2>&1; started=0
O="$share/out"; code() { tr -d ' \r\n' < "$O/$1.rc" 2>/dev/null; }
[ -f "$share/done" ] && [ "$crash" = 0 ];                        ok $? "the AmigaDOS script ran to its end, no alert"
[ "$(code publish)" = 0 ];                                       ok $? "AROS publishes tool"
[ "$(code push)" = 0 ] && grep -q 'published tool' "$O/push.o";  ok $? "AROS pushes it over http, signed (\$RC $(code push))"
PKG_CACHE="$T/cache" "$P" SHOW CHANNEL "$site/fromaros" 2>&1 | grep -q "^tool .* ok  *${pub:0:16}"
ok $? "the portal's channel holds it, checked, signed by AROS's key"
[ "$(code https)" = 17 ] && grep -q 'no TLS' "$O/https.o";       ok $? "to an https address AROS refuses, and says to use http"
echo; echo "aros-push: $checks checks, $fails failures"; [ "$fails" -eq 0 ]
