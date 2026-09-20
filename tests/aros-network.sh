#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Pkg reads channels over http and https on hosted AROS (Macaros), where
# bsdsocket.library forwards to the Mac's own sockets: from channels this
# script serves, and from the portal by name. The https part checks the
# certificate as well as the transfer: a server this Pkg has no authority
# for, one made out to another name and one out of date are each refused
# with their own reason. tests/native-network.sh is the same on native
# x86_64 AROS with AROSTCP.
#
# Needs: make; tools/build-aros.sh; python3; an openssl that takes -not_after;
# hosted AROS with bsdsocket.library (make workbench-libs-bsdsocket-unix in
# the AROS build; make bsdsock-dylib and aros-ctl deploy in Macaros).
# PORTAL= empty skips the portal part.

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
portal=${PORTAL-https://aros-pkg.azurewebsites.net/pkg}
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-arosnet.XXXXXX")
share="$work/share"
aros_started=0; srv_pid=; tls_pids=
cleanup() {
    [ "$aros_started" = 0 ] || "$control" stop > /dev/null 2>&1 || true
    [ -z "$srv_pid" ] || kill "$srv_pid" 2>/dev/null
    for p in $tls_pids; do kill "$p" 2>/dev/null; done
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
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
port=$(free_port)
python3 -m http.server --bind 127.0.0.1 --directory "$work/www" "$port" > "$work/srv.log" 2>&1 &
srv_pid=$!

# The same channel over https, four times: once with a certificate Pkg can
# check, and three times with one it must refuse.
sh "$repo_root/tests/tls-certs.sh" "$work/tls" > /dev/null || exit 69
mkdir -p "$share/tls"
cp "$work/tls/ca.pem" "$share/tls/ca.pem"
tls_ports=
for name in good other wrongname expired; do
    p=$(free_port)
    python3 "$repo_root/tests/https_server.py" "$work/tls/$name.pem" "$work/www" "$p" > "$work/$name.log" 2>&1 &
    tls_pids="$tls_pids $!"
    tls_ports="$tls_ports $p"
done
# shellcheck disable=SC2086 -- four ports, in the order of the loop above.
set -- $tls_ports
good_port=$1; other_port=$2; wrongname_port=$3; expired_port=$4
# a fifth, with the same good certificate, that holds the connection open:
# what a keep-alive client asks of a server that can do it
ka_port=$(free_port)
PKG_TEST_KEEPALIVE=1 python3 "$repo_root/tests/https_server.py" "$work/tls/good.pem" "$work/www" "$ka_port" \
    > "$work/keepalive.log" 2>&1 &
tls_pids="$tls_pids $!"
w=0
while [ "$w" -lt 50 ]; do
    [ "$(cat "$work"/good.log "$work"/other.log "$work"/wrongname.log "$work"/expired.log "$work"/keepalive.log 2>/dev/null | grep -c '^listening ')" = 5 ] && break
    sleep 1; w=$((w + 1))
done
[ "$w" -lt 50 ] || { echo "aros-network: the https servers did not start" >&2; exit 69; }

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
C:Echo \"\$RC\" >MacRW:out/n04.rc
C:SetEnv PKG_CAFILE MacRW:tls/ca.pem
C:MakeDir RAM:sys2
$P SHOW CHANNEL https://127.0.0.1:$good_port/ch >MacRW:out/n05.o
C:Echo \"\$RC\" >MacRW:out/n05.rc
$P INSTALL nethello ROOT RAM:sys2 CHANNEL https://127.0.0.1:$good_port/ch >MacRW:out/n06.o
C:Echo \"\$RC\" >MacRW:out/n06.rc
$P VERIFY nethello ROOT RAM:sys2 MACHINE >MacRW:out/n07.o
C:Echo \"\$RC\" >MacRW:out/n07.rc
$P SHOW CHANNEL https://127.0.0.1:$other_port/ch >MacRW:out/n08.o
C:Echo \"\$RC\" >MacRW:out/n08.rc
$P SHOW CHANNEL https://127.0.0.1:$wrongname_port/ch >MacRW:out/n09.o
C:Echo \"\$RC\" >MacRW:out/n09.rc
$P SHOW CHANNEL https://127.0.0.1:$expired_port/ch >MacRW:out/n0a.o
C:Echo \"\$RC\" >MacRW:out/n0a.rc
$P SHOW CHANNEL https://127.0.0.1:$ka_port/ch TRACE MacRW:out/keep.trace >MacRW:out/n0b.o
C:Echo \"\$RC\" >MacRW:out/n0b.rc
C:SetEnv PKG_NO_KEEPALIVE 1
$P SHOW CHANNEL https://127.0.0.1:$ka_port/ch TRACE MacRW:out/nokeep.trace >MacRW:out/n0c.o
C:Echo \"\$RC\" >MacRW:out/n0c.rc
C:UnSetEnv PKG_NO_KEEPALIVE
C:UnSetEnv PKG_CAFILE"
[ -z "$portal" ] || script="$script
$P SHOW CHANNEL http://${portal#https://} >MacRW:out/n10.o
C:Echo \"\$RC\" >MacRW:out/n10.rc
$P INSTALL pkg ROOT RAM:sys CHANNEL http://${portal#https://} >MacRW:out/n11.o
C:Echo \"\$RC\" >MacRW:out/n11.rc
RAM:sys/C/Pkg VERIFY pkg ROOT RAM:sys MACHINE >MacRW:out/n12.o
C:Echo \"\$RC\" >MacRW:out/n12.rc
$P SHOW CHANNEL $portal >MacRW:out/n13.o
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
exits n05 0 "SHOW reads the same channel over https"
has "$O/n05.o" '^nethello  *1.0 .* ok ';              ok $? "and checks its entry: ok"
exits n06 0 "INSTALL over https"
exits n07 0 "VERIFY of what came over TLS"
has "$O/n07.o" '^result: intact$';                    ok $? "intact against its signed manifest"
[ "$(code n08)" != 0 ];                               ok $? "a certificate from an authority Pkg was not given is refused (\$RC $(code n08))"
has "$O/n08.o" 'authority this Pkg does not know';    ok $? "and says the authority is unknown, and what PKG_CAFILE is for"
[ "$(code n09)" != 0 ];                               ok $? "a certificate made out to another name is refused (\$RC $(code n09))"
has "$O/n09.o" 'made out to another name';            ok $? "and says the name is wrong"
[ "$(code n0a)" != 0 ];                               ok $? "a certificate out of date is refused (\$RC $(code n0a))"
has "$O/n0a.o" 'certificate that expired';            ok $? "and says it expired"
has "$O/n0a.o" 'clock is wrong';                      ok $? "and that the clock may be the reason, with the date the machine believes"
# The connection, held open: the same channel read from a server that can
# keep one, and read again with the reuse turned off.
conns() { grep -c 'net: connect ' "$O/$1.trace" 2>/dev/null || true; }
gets() { grep -c 'answered ' "$O/$1.trace" 2>/dev/null || true; }
exits n0b 0 "SHOW reads a channel from a server that holds the connection open"
[ "$(gets keep)" -gt 2 ] && [ "$(conns keep)" = 1 ]
ok $? "and asks for $(gets keep) files over one connection (connections opened: $(conns keep))"
exits n0c 0 "the same read with PKG_NO_KEEPALIVE=1"
[ "$(conns nokeep)" = "$(gets nokeep)" ] && [ "$(conns nokeep)" -gt 1 ]
ok $? "opens a connection per file: the check above fails without the reuse ($(conns nokeep) connections for $(gets nokeep) files)"
if [ -n "$portal" ]; then
    exits n10 0 "SHOW reads the portal's channel over http"
    has "$O/n10.o" '^pkg  *[0-9.]*  *application  *aarch64  *ok ';  ok $? "its aarch64 entry checks: ok"
    exits n11 0 "INSTALL pkg from the portal"
    exits n12 0 "the installed Pkg runs and verifies itself"
    has "$O/n12.o" '^result: intact$';                 ok $? "intact"
    exits n13 0 "SHOW reads the portal over https, with the authorities built in"
    has "$O/n13.o" '^pkg  *[0-9.]*  *application  *aarch64  *ok ';  ok $? "and every entry checks: ok"
fi

echo
echo "aros-network: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
