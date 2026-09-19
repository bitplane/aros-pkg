#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Pkg reads channels over http on native AROS: pc-x86_64 in QEMU, AROSTCP on
# an emulated pcnet32 card, nothing hosted. The nightly ISO is remastered
# with Pkg on it; S:User-Startup configures and starts the network, then
# installs from a channel this script serves on the host (by address, no
# name server needed) and from the portal (by name, redirects and all), and
# checks that https is refused with the reason.
#
# Needs: sh tools/build-aros-x86_64.sh; the nightly pc-x86_64 boot ISO
# (AROS_X86_64_ISO); qemu-system-x86_64; xorriso; bsdtar; python3.
# PORTAL=<http url of a pkg channel> to change or, empty, skip the portal part.

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
host_pkg="$repo_root/build/pkg"
aros_pkg="$repo_root/build/aros-x86_64/Pkg"
iso=${AROS_X86_64_ISO:-$(ls "$HOME"/aros-native/AROS-*-pc-x86_64-boot-iso/aros-pc-x86_64.iso 2>/dev/null | tail -1)}
portal=${PORTAL-http://aros-pkg.azurewebsites.net/pkg}
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-natnet.XXXXXX")
qemu_pid=; srv_pid=
cleanup() {
    [ -z "$qemu_pid" ] || kill "$qemu_pid" 2>/dev/null
    [ -z "$srv_pid" ] || kill "$srv_pid" 2>/dev/null
    [ "${KEEP:-0}" = 1 ] && { echo "kept $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
for need in "$host_pkg" "$aros_pkg" "$iso"; do
    [ -e "$need" ] || { echo "native-network: missing $need" >&2; exit 69; }
done
for tool in qemu-system-x86_64 xorriso bsdtar python3; do
    command -v "$tool" > /dev/null || { echo "native-network: missing $tool" >&2; exit 69; }
done
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# ---- a channel on the host, served over http ------------------------------
echo "native-network 1: a channel served by the host"
mkdir -p "$work/drawer/C" "$work/www"
cp "$aros_pkg" "$work/drawer/C/NetHello"          # a real x86_64 AROS program
"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
PKG_SIGNKEY="$work/dev.key" "$host_pkg" PUBLISH "$work/drawer" CHANNEL "$work/www/ch" NAME nethello VERSION 1.0 \
    KIND application > /dev/null 2>&1
ok $? "the host publishes nethello 1.0"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 -m http.server --bind 127.0.0.1 --directory "$work/www" "$port" > "$work/srv.log" 2>&1 &
srv_pid=$!

# ---- the ISO, with Pkg, the network's configuration and the sequence ------
T="$work/iso"
mkdir -p "$T"
bsdtar -xf "$iso" -C "$T"
chmod -R u+w "$T"
mkdir -p "$T/PkgTest"
cp "$aros_pkg" "$T/PkgTest/Pkg"
P='SYS:PkgTest/Pkg'
steps=""
step() { n=$1; shift; steps="$steps $n"; printf '%s >RAM:out/%s.o\nEcho "$RC" >RAM:out/%s.rc\n' "$*" "$n" "$n"; }
{
    echo 'FailAt 99'
    echo 'MakeDir RAM:out RAM:sys RAM:netdb'
    # QEMU's user network: the guest is 10.0.2.15, the host 10.0.2.2, names at 10.0.2.3
    echo 'Copy SYS:System/Network/AROSTCP/db RAM:netdb ALL QUIET'
    echo 'Echo "eth0 DEV=DEVS:networks/pcnet32.device UNIT=0 IP=10.0.2.15 NETMASK=255.255.255.0 UP" >RAM:netdb/interfaces'
    echo 'Echo "DEFAULT GATEWAY 10.0.2.2" >RAM:netdb/static-routes'
    echo 'Echo "HOST 10.0.2.15 arosbox.arosnet arosbox" >RAM:netdb/netdb-myhost'
    echo 'Echo "NAMESERVER 10.0.2.3" >>RAM:netdb/netdb-myhost'
    step n00 $P SHOW CHANNEL "http://10.0.2.2:$port/ch"      # before the network is started
    echo 'MakeDir ENV:AROSTCP'
    echo 'Echo "RAM:netdb" NOLINE >ENV:AROSTCP/Config'
    step d00 GetEnv AROSTCP/Config
    echo 'Path SYS:System/Network/AROSTCP/C ADD'
    echo 'Run >NIL: QUIET SYS:System/Network/AROSTCP/C/AROSTCP'
    echo 'WaitForPort AROSTCP'
    echo 'Wait 8'
    step d01 netstat -i
    step d02 netstat -rn
    step n01 $P SHOW CHANNEL "http://10.0.2.2:$port/ch"
    step n02 $P INSTALL nethello ROOT RAM:sys CHANNEL "http://10.0.2.2:$port/ch"
    step n03 $P VERIFY nethello ROOT RAM:sys MACHINE
    step n04 $P SHOW CHANNEL "http://10.0.2.2:$port/nowhere"
    if [ -n "$portal" ]; then
        step n10 $P SHOW CHANNEL "$portal"
        step n11 $P INSTALL pkg ROOT RAM:sys CHANNEL "$portal"
        step n12 RAM:sys/C/Pkg VERIFY pkg ROOT RAM:sys MACHINE
        step n13 $P SHOW CHANNEL "https://${portal#http://}"
    fi
    for n in $steps; do
        for ext in o rc; do
            printf 'Echo "==BEGIN %s.%s==" >SER1:\nType RAM:out/%s.%s >SER1:\nEcho "==END==" >SER1:\n' "$n" "$ext" "$n" "$ext"
        done
    done
    echo 'Echo "==PKGTEST-DONE==" >SER1:'
} > "$T/S/User-Startup"
cp "$T/S/User-Startup" "$work/user-startup.txt"
sed -i '' 's/^set timeout=5/set timeout=0/; s|multiboot2 /boot/pc/bootstrap.xz ATA=32bit \$bootstrap_flags |multiboot2 /boot/pc/bootstrap.xz ATA=32bit debug=serial $bootstrap_flags |' \
    "$T/boot/grub/grub.cfg"
xorriso -as mkisofs -R -J -V AROS -o "$work/test.iso" -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info "$T" > "$work/xorriso.log" 2>&1
ok $? "the test ISO is built"
rm -rf "$T"

# ---- native AROS in QEMU, with a network card -------------------------------
echo "native-network 2: pc-x86_64 AROS in QEMU, pcnet32, AROSTCP"
qemu-system-x86_64 -m 1024 -cdrom "$work/test.iso" -boot d -display none -no-reboot \
    -nic user,model=pcnet \
    -serial file:"$work/com1.log" -serial file:"$work/com2.log" > "$work/qemu.log" 2>&1 &
qemu_pid=$!
w=0
while [ "$w" -lt 900 ] && ! LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/com2.log" 2>/dev/null; do
    sleep 5; w=$((w + 5))
done
kill "$qemu_pid" 2>/dev/null; qemu_pid=
LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/com2.log";   ok $? "the startup ran to its end ($w s)"

O="$work/out"
mkdir -p "$O"
LC_ALL=C tr -d '\r' < "$work/com2.log" | awk -v dir="$O" '
    /^==BEGIN / { name = $2; sub(/==$/, "", name); file = dir "/" name; printf "" > file; next }
    /^==END==/  { file = ""; next }
    file != ""  { print >> file }'
code() { tr -d ' \r\n' < "$O/$1.rc" 2>/dev/null; }
exits() { [ "$(code "$1")" = "$2" ]; ok $? "$3: \$RC $2 (got $(code "$1"))"; }

exits n00 17 "before the network is started, a web channel is refused"
has "$O/n00.o" 'network is not started';               ok $? "and the refusal says the network is not started"
exits n01 0 "SHOW reads the host's channel over http"
has "$O/n01.o" '^nethello  *1.0 .* ok ';               ok $? "and checks its entry: ok"
exits n02 0 "INSTALL over http"
grep -q "GET /ch/objects/.*\.pkg" "$work/srv.log";     ok $? "the host's server saw AROS fetch the payload"
exits n03 0 "VERIFY of what came over the network"
has "$O/n03.o" '^result: intact$';                     ok $? "intact against its signed manifest"
exits n04 11 "a web address with no channel is refused as not found"
if [ -n "$portal" ]; then
    exits n10 0 "SHOW reads the portal's channel by name"
    has "$O/n10.o" '^pkg  *[0-9.]*  *application  *x86_64  *ok ';  ok $? "its x86_64 entry checks: ok"
    exits n11 0 "INSTALL pkg from the portal"
    exits n12 0 "the installed Pkg runs and verifies itself"
    has "$O/n12.o" '^result: intact$';                 ok $? "intact"
    exits n13 17 "https is refused on AROS"
    has "$O/n13.o" 'AROS has no TLS';                  ok $? "with the reason, and what to use instead"
fi

echo
echo "native-network: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
