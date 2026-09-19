#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# PUSH from AROS itself: hosted AROS publishes a drawer and pushes it three
# ways to servers on this machine, and the host then reads what arrived.
#
#   over plain http, signed with the publisher's key, to the real portal
#   over https, signed the same way, to the same portal
#   over https with a portal key in PKG_PUSHKEY, to tests/push_server.py
#
# The certificates are the throwaway ones of tests/tls-certs.sh, and AROS is
# given their authority in PKG_CAFILE.
#
# Needs: make; tools/build-aros.sh; python3; an openssl that takes -not_after;
# the portal built; hosted AROS with bsdsocket.library (see
# tests/aros-network.sh).

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
T=$(mktemp -d); share="$T/share"; port=5081; sport=5443
site="http://127.0.0.1:$port"; ssite="https://127.0.0.1:$sport"; srv=; stub=; started=0
trap '[ $started = 0 ] || "$control" stop > /dev/null 2>&1; kill $srv $stub 2>/dev/null; [ "${KEEP:-0}" = 1 ] && echo "kept $T" >&2 || rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }

mkdir -p "$share/out" "$share/bin" "$share/src/Tool/C" "$share/tls"
cp "$repo_root/build/aros/Pkg" "$share/bin/Pkg"; cp "$repo_root/build/aros/Pkg" "$share/src/Tool/C/Tool"
"$P" KEYGEN FILE "$share/my.key" > /dev/null
pub=$("$P" KEYINFO FILE "$share/my.key" MACHINE | awk '/^public:/{print $2}')
sh "$repo_root/tests/tls-certs.sh" "$T/tls" > /dev/null || exit 69
cp "$T/tls/ca.pem" "$share/tls/ca.pem"
# the portal, on http and on https with the throwaway certificate
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" \
  Portal__SignedKeys="arospublisher:$pub:fromaros,fromaros-tls:files" \
  Kestrel__Certificates__Default__Path="$T/tls/good.pfx" Kestrel__Certificates__Default__Password=pkg \
  ASPNETCORE_URLS="$site;$ssite" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done
# the push API stand-in, over https, for the push that carries a portal key
mkdir -p "$T/stub/fromaros"
PKG_TEST_TLS_CERT="$T/tls/good.pem" python3 "$repo_root/tests/push_server.py" "$T/stub" "$P" "$T/stub.port" \
    > "$T/stub.log" 2>&1 &
stub=$!
disown $stub 2> /dev/null || true              # its kill at the end is not news
for i in $(seq 1 40); do [ -s "$T/stub.port" ] && break; sleep 1; done
[ -s "$T/stub.port" ] || { echo "aros-push: the https push server did not start" >&2; exit 69; }
stubsite="https://127.0.0.1:$(cat "$T/stub.port")"

script="FailAt 99
MacRW:bin/Pkg PUBLISH MacRW:src/Tool CHANNEL MacRW:channel NAME tool KIND application SIGN MacRW:my.key >MacRW:out/publish.o
C:Echo \"\$RC\" >MacRW:out/publish.rc
MacRW:bin/Pkg PUSH CHANNEL MacRW:channel TO $site/fromaros SIGN MacRW:my.key >MacRW:out/push.o
C:Echo \"\$RC\" >MacRW:out/push.rc
C:SetEnv PKG_CAFILE MacRW:tls/ca.pem
MacRW:bin/Pkg PUSH CHANNEL MacRW:channel TO $ssite/fromaros-tls SIGN MacRW:my.key >MacRW:out/https.o
C:Echo \"\$RC\" >MacRW:out/https.rc
C:SetEnv PKG_PUSHKEY testkey
MacRW:bin/Pkg PUSH CHANNEL MacRW:channel TO $stubsite/fromaros >MacRW:out/key.o
C:Echo \"\$RC\" >MacRW:out/key.rc
C:UnSetEnv PKG_PUSHKEY
C:UnSetEnv PKG_CAFILE
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
[ "$(code https)" = 0 ] && grep -q 'published tool' "$O/https.o"; ok $? "AROS pushes a second channel over https, signed (\$RC $(code https))"
PKG_CACHE="$T/cache2" "$P" SHOW CHANNEL "$site/fromaros-tls" 2>&1 | grep -q "^tool .* ok  *${pub:0:16}"
ok $? "the portal's second channel holds what TLS carried, checked"
[ "$(code key)" = 0 ] && grep -q 'published tool' "$O/key.o";    ok $? "AROS pushes over https with a portal key in PKG_PUSHKEY (\$RC $(code key))"
[ -f "$T/stub/fromaros/index" ];                                 ok $? "and the push server holds the channel it sent"
echo; echo "aros-push: $checks checks, $fails failures"; [ "$fails" -eq 0 ]
