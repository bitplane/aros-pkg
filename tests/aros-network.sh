#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Pkg reads channels over http on hosted AROS (Macaros), where
# bsdsocket.library forwards to the Mac's own sockets: from a channel this
# script serves, from the portal by name, and https refused with the reason.
# tests/native-network.sh is the same on native x86_64 AROS with AROSTCP.
#
# Needs: make; tools/build-aros.sh; hosted AROS with bsdsocket.library
# (make workbench-libs-bsdsocket-unix in the AROS build; make bsdsock-dylib and
# aros-ctl deploy in Macaros). PORTAL= empty skips the portal part.

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
portal=${PORTAL-http://aros-pkg.azurewebsites.net/pkg}
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-arosnet.XXXXXX")
share="$work/share"
aros_started=0; srv_pid=
cleanup() {
    [ "$aros_started" = 0 ] || "$control" stop > /dev/null 2>&1 || true
    [ -z "$srv_pid" ] || kill "$srv_pid" 2>/dev/null
    [ "${KEEP:-0}" = 1 ] && { echo "kept $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
for need in "$host_pkg" "$repo_root/build/aros/Pkg" "$control"; do
    [ -e "$need" ] || { echo "aros-network: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "aros-network: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

mkdir -p "$share/out" "$share/bin" "$work/drawer/C" "$work/www"
cp "$repo_root/build/aros/Pkg" "$share/bin/Pkg"
cp "$repo_root/build/aros/Pkg" "$work/drawer/C/NetHello"
"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
PKG_SIGNKEY="$work/dev.key" "$host_pkg" PUBLISH "$work/drawer" CHANNEL "$work/www/ch" NAME nethello VERSION 1.0 \
    KIND application > /dev/null 2>&1
ok $? "the host publishes nethello 1.0"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 -m http.server --bind 127.0.0.1 --directory "$work/www" "$port" > "$work/srv.log" 2>&1 &
srv_pid=$!

P='MacRW:bin/Pkg'
script="FailAt 99
C:MakeDir RAM:sys
$P SHOW CHANNEL http://127.0.0.1:$port/ch >MacRW:out/n01.o
C:Echo \"\$RC\" >MacRW:out/n01.rc
$P INSTALL nethello ROOT RAM:sys CHANNEL http://127.0.0.1:$port/ch >MacRW:out/n02.o
C:Echo \"\$RC\" >MacRW:out/n02.rc
$P VERIFY nethello ROOT RAM:sys MACHINE >MacRW:out/n03.o
C:Echo \"\$RC\" >MacRW:out/n03.rc
$P SHOW CHANNEL http://127.0.0.1:$port/nowhere >MacRW:out/n04.o
C:Echo \"\$RC\" >MacRW:out/n04.rc"
[ -z "$portal" ] || script="$script
$P SHOW CHANNEL $portal >MacRW:out/n10.o
C:Echo \"\$RC\" >MacRW:out/n10.rc
$P INSTALL pkg ROOT RAM:sys CHANNEL $portal >MacRW:out/n11.o
C:Echo \"\$RC\" >MacRW:out/n11.rc
RAM:sys/C/Pkg VERIFY pkg ROOT RAM:sys MACHINE >MacRW:out/n12.o
C:Echo \"\$RC\" >MacRW:out/n12.rc
$P SHOW CHANNEL https://${portal#http://} >MacRW:out/n13.o
C:Echo \"\$RC\" >MacRW:out/n13.rc"
script="$script
C:Echo done >MacRW:done"

echo "aros-network: hosted AROS"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$script" "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 240 ]; do sleep 1; w=$((w + 1)); done
crash=$("$control" crash 2> /dev/null | grep -c -i -E 'stack limits|ALERT')
"$control" stop > /dev/null 2>&1 || true
aros_started=0
O="$share/out"
code() { tr -d ' \r\n' < "$O/$1.rc" 2>/dev/null; }
exits() { [ "$(code "$1")" = "$2" ]; ok $? "$3: \$RC $2 (got $(code "$1"))"; }

[ -f "$share/done" ];                                  ok $? "the AmigaDOS script ran to its end"
[ "$crash" = 0 ];                                      ok $? "no alert in AROS's log"
exits n01 0 "SHOW reads the host's channel over http"
has "$O/n01.o" '^nethello  *1.0 .* ok ';               ok $? "and checks its entry: ok"
exits n02 0 "INSTALL over http"
grep -q "GET /ch/objects/.*\.pkg" "$work/srv.log";     ok $? "the host's server saw AROS fetch the payload"
exits n03 0 "VERIFY of what came over the network"
has "$O/n03.o" '^result: intact$';                     ok $? "intact against its signed manifest"
exits n04 11 "a web address with no channel is refused as not found"
if [ -n "$portal" ]; then
    exits n10 0 "SHOW reads the portal's channel by name"
    has "$O/n10.o" '^pkg  *[0-9.]*  *application  *aarch64  *ok ';  ok $? "its aarch64 entry checks: ok"
    exits n11 0 "INSTALL pkg from the portal"
    exits n12 0 "the installed Pkg runs and verifies itself"
    has "$O/n12.o" '^result: intact$';                 ok $? "intact"
    exits n13 17 "https is refused on AROS"
    has "$O/n13.o" 'AROS has no TLS';                  ok $? "with the reason, and what to use instead"
fi

echo
echo "aros-network: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
