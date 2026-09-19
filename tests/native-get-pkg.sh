#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Getting started on a clean native AROS: the nightly pc-x86_64 ISO with the
# contrib's wget and nothing of Pkg on it, in QEMU with AROSTCP started. The
# two lines the Downloads page gives are run as they are, against the portal
# (PORTAL, plain http), and Pkg must end up installed, signed and current.
#
# Needs: the nightly boot ISO (AROS_X86_64_ISO) and the contrib's WGet drawer
# (AROS_WGET, Extras/Networking/Apps/WGet of the pc-x86_64 contrib archive; it
# runs only once ENV:wgetcfg exists, which the test creates as a user must);
# qemu-system-x86_64; xorriso; bsdtar.

set -u
iso=${AROS_X86_64_ISO:-$(ls "$HOME"/aros-native/AROS-*-pc-x86_64-boot-iso/aros-pc-x86_64.iso 2>/dev/null | tail -1)}
wget=${AROS_WGET:-$(ls -d "$HOME"/aros-native/nightly/*/wget-x86_64/Extras/Networking/Apps/WGet 2>/dev/null | tail -1)}
portal=${PORTAL:-http://aros-pkg.azurewebsites.net}
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-getpkg.XXXXXX")
qemu_pid=
cleanup() {
    [ -z "$qemu_pid" ] || kill "$qemu_pid" 2>/dev/null
    [ "${KEEP:-0}" = 1 ] && { echo "kept $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
for need in "$iso" "$wget/wget"; do
    [ -e "$need" ] || { echo "native-get-pkg: missing $need" >&2; exit 69; }
done
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

T="$work/iso"
mkdir -p "$T"
bsdtar -xf "$iso" -C "$T"
chmod -R u+w "$T"
mkdir -p "$T/Extras/Networking/Apps"
cp -R "$wget" "$T/Extras/Networking/Apps/WGet"
! find "$T" -iname 'Pkg' -type f | grep -q .;          ok $? "nothing of Pkg is on the machine"
steps=""
step() { n=$1; shift; steps="$steps $n"; printf '%s >RAM:out/%s.o\nEcho "$RC" >RAM:out/%s.rc\n' "$*" "$n" "$n"; }
{
    echo 'FailAt 99'
    for d in RAM:out RAM:sys RAM:netdb ENV:AROSTCP; do echo "MakeDir $d"; done
    echo 'Copy SYS:System/Network/AROSTCP/db RAM:netdb ALL QUIET'
    echo 'Echo "eth0 DEV=DEVS:networks/pcnet32.device UNIT=0 IP=10.0.2.15 NETMASK=255.255.255.0 UP" >RAM:netdb/interfaces'
    echo 'Echo "DEFAULT GATEWAY 10.0.2.2" >RAM:netdb/static-routes'
    echo 'Echo "HOST 10.0.2.15 arosbox.arosnet arosbox" >RAM:netdb/netdb-myhost'
    echo 'Echo "NAMESERVER 10.0.2.3" >>RAM:netdb/netdb-myhost'
    echo 'Echo "RAM:netdb" NOLINE >ENV:AROSTCP/Config'
    echo 'Run >NIL: QUIET SYS:System/Network/AROSTCP/C/AROSTCP'
    echo 'WaitForPort AROSTCP'
    echo 'Wait 8'
    echo 'Path SYS:Extras/Networking/Apps/WGet ADD'
    echo 'Echo "" >ENV:wgetcfg'
    step g0 wget -o RAM:out/wget.log -O RAM:probe "$portal/health"
    echo 'Copy RAM:out/wget.log RAM:out/w0.o'
    echo 'Echo "-" >RAM:out/w0.rc'
    steps="$steps w0"
    # the two lines of the Downloads page, with a root that can be written (the CD cannot)
    step g1 wget -q -O RAM:Get-Pkg "$portal/Get-Pkg"
    echo 'Execute RAM:Get-Pkg RAM:sys/'
    echo 'Echo "$RC" >RAM:out/g2.rc'
    echo 'Echo "-" >RAM:out/g2.o'
    steps="$steps g2"
    step g3 RAM:sys/C/Pkg VERIFY pkg ROOT RAM:sys MACHINE
    step g4 RAM:sys/C/Pkg STATUS ROOT RAM:sys CHANNEL "$portal/pkg"
    for n in $steps; do
        for ext in o rc; do
            printf 'Echo "==BEGIN %s.%s==" >SER1:\nType RAM:out/%s.%s >SER1:\nEcho "==END==" >SER1:\n' "$n" "$ext" "$n" "$ext"
        done
    done
    echo 'Echo "==PKGTEST-DONE==" >SER1:'
} > "$T/S/User-Startup"
sed -i '' 's/^set timeout=5/set timeout=0/' "$T/boot/grub/grub.cfg"
xorriso -as mkisofs -R -J -V AROS -o "$work/test.iso" -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info "$T" > "$work/xorriso.log" 2>&1
ok $? "the test ISO is built"
rm -rf "$T"

echo "native-get-pkg: a clean pc-x86_64 AROS in QEMU"
qemu-system-x86_64 -m 1024 -cdrom "$work/test.iso" -boot d -display none -no-reboot -nic user,model=pcnet \
    -serial file:"$work/com1.log" -serial file:"$work/com2.log" > "$work/qemu.log" 2>&1 &
qemu_pid=$!
w=0
while [ "$w" -lt 900 ] && ! LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/com2.log" 2>/dev/null; do sleep 5; w=$((w + 5)); done
kill "$qemu_pid" 2>/dev/null; qemu_pid=
LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/com2.log";   ok $? "the startup ran to its end ($w s)"
O="$work/out"; mkdir -p "$O"
LC_ALL=C tr -d '\r' < "$work/com2.log" | awk -v dir="$O" '
    /^==BEGIN / { name = $2; sub(/==$/, "", name); file = dir "/" name; printf "" > file; next }
    /^==END==/  { file = ""; next }
    file != ""  { print >> file }'
code() { tr -d ' \r\n' < "$O/$1.rc" 2>/dev/null; }
[ "$(code g1)" = 0 ];                                  ok $? "line 1: wget fetches Get-Pkg (\$RC $(code g1))"
[ "$(code g2)" = 0 ];                                  ok $? "line 2: Execute RAM:Get-Pkg installs Pkg (\$RC $(code g2))"
[ "$(code g3)" = 0 ] && has "$O/g3.o" '^result: intact$'
ok $? "the installed Pkg runs and is intact against its signed manifest"
[ "$(code g4)" = 0 ] && has "$O/g4.o" 'all up to date'
ok $? "STATUS over http: up to date with the portal"

echo
echo "native-get-pkg: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
