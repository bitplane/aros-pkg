#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# AROS itself as packages, on native AROS: pc-x86_64 in QEMU. Booted from a
# CD made of the nightly boot ISO plus the system channel
# (tools/aros/publish-system.sh), AROS partitions and formats an empty disk,
# copies the system onto it the way InstallAROS does, lets Pkg adopt the
# copied files package by package, and installs GRUB as InstallAROS does.
# Then it boots from the disk and, on that system: VERIFY ALL; a program
# deleted and a library overwritten by accident, a configuration file edited
# on purpose; VERIFY ALL names each; REPAIR ALL puts the two back from the
# channel, keeps the overwritten bytes as .pkgold and the edit as it is; the
# restored program runs.
#
# Needs: sh tools/build-aros-x86_64.sh; the nightly boot ISO; the system
# channel in $PKG_SYSTEM_WORK/ch (default ~/aros-native/system-test),
# published from that ISO's tree; qemu-system-x86_64, qemu-img, xorriso,
# bsdtar. The disk image stays in that directory, so later runs skip
# partitioning; PKG_FRESH=1 starts from an empty disk.

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_pkg="$repo_root/build/aros-x86_64/Pkg"
W=${PKG_SYSTEM_WORK:-$HOME/aros-native/system-test}
iso=${AROS_X86_64_ISO:-$(ls "$HOME"/aros-native/AROS-*-pc-x86_64-boot-iso/aros-pc-x86_64.iso 2>/dev/null | tail -1)}
disk="$W/sys.img"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-system.XXXXXX")
qemu_pid=
cleanup() {
    [ -z "$qemu_pid" ] || kill "$qemu_pid" 2>/dev/null
    [ "${PKG_KEEP:-0}" = 1 ] && { echo "native-system: keeping $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

for need in "$aros_pkg" "$iso" "$W/ch/index"; do
    [ -e "$need" ] || { echo "native-system: missing $need" >&2; exit 69; }
done
for tool in qemu-system-x86_64 qemu-img xorriso bsdtar; do
    command -v $tool > /dev/null || { echo "native-system: $tool not found" >&2; exit 69; }
done
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# The system partition as InstallAROS makes it: its SFS choice by default
# here, since FFS takes no name over 30 characters and the ISO has some;
# PKG_SYSFS=FFSIntl for its other choice. PKG_SYSSIZE in MB.
sysfs=${PKG_SYSFS:-SFS}
syssize=${PKG_SYSSIZE:-2048}
case $sysfs in FFSIntl) fmtflags="FFS INTL" ;; *) fmtflags="" ;; esac

# What InstallAROS copies, less Developer (an option there too).
pkgs="aros-boot aros-base aros-prefs aros-fonts aros-locale aros-tools aros-demos aros-extras"
drawers="boot C L Libs Devs S Classes System Rexxc Storage WBStartup Prefs Fonts Locale Tools Utilities Demos Extras"

echo "native-system 1: the CD"
T="$work/iso"
mkdir -p "$T"
bsdtar -xf "$iso" -C "$T" && chmod -R u+w "$T"
CH="$T/PkgTest/chan"
mkdir -p "$CH/objects" "$T/PkgTest/C"
: > "$CH/index"
for p in $pkgs; do
    line=$(awk -v n="$p" '$1==n' "$W/ch/index" | tail -1)
    [ -n "$line" ] || { echo "native-system: $p is not in $W/ch" >&2; exit 69; }
    echo "$line" >> "$CH/index"
    d=$(echo "$line" | awk '{print $4}')
    cp "$W/ch/objects/$d.manifest" "$W/ch/objects/$d.sig" "$CH/objects/"
    pl=$(sed -n 's/^Payload: //p' "$W/ch/objects/$d.manifest")
    cp "$W/ch/objects/$pl.pkg" "$CH/objects/"
done
cp "$aros_pkg" "$T/PkgTest/C/Pkg"
# xorriso patches the boot image's table as it builds this CD, so its copy is
# not the released ISO's; InstallAROS copies the released one.
cp "$T/boot/grub/i386-pc/eltorito.img" "$T/PkgTest/eltorito.img"
[ -s "$CH/index" ];                                   ok $? "the CD carries the channel of the system packages"

P='CD0:PkgTest/C/Pkg'
C='CHANNEL CD0:PkgTest/chan'
step() {  # step <name> <command...>
    printf 'Echo "==BEGIN %s==" >SER1:\n%s >SER1:\nEcho "==RC $RC" >SER1:\nEcho "==END==" >SER1:\n' "$1" "$2" \
        >> "$T/S/User-Startup"
}
cat > "$T/S/User-Startup" <<EOF
FailAt 21
Echo "==BOOT==" >SER1:
If EXISTS SYS:pkgtest-installed
    Skip disk
EndIf
Assign >NIL: EXISTS DH0:
If WARN
    Echo "==STEP partition" >SER1:
    C:Partition DEVICE ata.device UNIT 0 SYSSIZE $syssize SYSTYPE $sysfs SYSNAME DH0 MAXWORK WORKTYPE SFS WORKNAME DH1 WIPE FORCE QUIET >SER1:
    Echo "==BOOT-END==" >SER1:
    C:Reboot
EndIf
Echo "==STEP install" >SER1:
Echo "" >RAM:cr
SYS:System/Format <RAM:cr DRIVE DH0: NAME System $fmtflags QUICK NOICONS >SER1:
SYS:System/Format <RAM:cr DRIVE DH1: NAME Work QUICK NOICONS >SER1:
EOF
for d in $drawers; do
    printf 'Copy SYS:%s DH0:%s ALL CLONE QUIET\n' "$d" "$d" >> "$T/S/User-Startup"
done
printf 'Copy SYS:#?.info DH0: CLONE QUIET\n' >> "$T/S/User-Startup"
printf 'Copy CD0:PkgTest/eltorito.img DH0:boot/grub/i386-pc/eltorito.img CLONE QUIET\n' >> "$T/S/User-Startup"
for p in $pkgs; do step "adopt-$p" "$P INSTALL $p ROOT DH0: $C MACHINE"; done
step grub 'C:Install-grub2 DEVICE ata.device UNIT 0 GRUB DH0:boot/grub'
cat >> "$T/S/User-Startup" <<'EOF'
Copy CD0:PkgTest/C/Pkg DH0:C/Pkg CLONE QUIET
Echo x >DH0:pkgtest-installed
Echo "==INSTALLED==" >SER1:
Echo "==BOOT-END==" >SER1:
C:Reboot
Lab disk
Echo "==STEP disk" >SER1:
EOF
step list "C:Pkg LIST ROOT SYS: MACHINE"
step verify1 "C:Pkg VERIFY ALL ROOT SYS: MACHINE"
cat >> "$T/S/User-Startup" <<'EOF'
Delete SYS:C/Dir QUIET
Echo "overwritten by accident" >SYS:Utilities/Clock
Echo "; my own line" >>SYS:S/Shell-Startup
EOF
step verify2 "C:Pkg VERIFY ALL ROOT SYS: MACHINE"
step repair "C:Pkg REPAIR ALL ROOT SYS: $C MACHINE"
step verify3 "C:Pkg VERIFY ALL ROOT SYS: MACHINE"
step dir "SYS:C/Dir SYS:Utilities"
step shellstartup "Search SYS:S/Shell-Startup \"my own line\""
printf 'Echo "==PKGTEST-DONE==" >SER1:\n' >> "$T/S/User-Startup"
xorriso -as mkisofs -R -J -V AROS -o "$work/test.iso" -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info "$T" > "$work/xorriso.log" 2>&1
                                                      ok $? "the test CD is built"
rm -rf "$T"
[ "${PKG_FRESH:-0}" = 1 ] && rm -f "$disk"
[ -f "$disk" ] || qemu-img create -f raw "$disk" 4G > /dev/null

echo "native-system 2: boots, until the run is done"
: > "$work/all.log"
boots=0
from=d
start=$(date +%s)
while [ $boots -lt 6 ] && ! LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/all.log"; do
    boots=$((boots + 1))
    : > "$work/com2.log"
    qemu-system-x86_64 -m 2048 -drive file="$disk",format=raw,if=ide,index=0 -cdrom "$work/test.iso" \
        -boot $from -display none -no-reboot -serial file:"$work/com1.log" -serial file:"$work/com2.log" \
        -monitor unix:"$work/mon",server,nowait > "$work/qemu.log" 2>&1 &
    qemu_pid=$!
    w=0
    while [ $w -lt 5400 ] && kill -0 "$qemu_pid" 2>/dev/null \
          && ! LC_ALL=C grep -a -q 'PKGTEST-DONE\|BOOT-END' "$work/com2.log"; do
        sleep 5; w=$((w + 5))
        # nothing on either serial port after five minutes: it never started
        [ $w -ge 300 ] && [ ! -s "$work/com1.log" ] && [ ! -s "$work/com2.log" ] && break
    done
    if ! LC_ALL=C grep -a -q 'PKGTEST-DONE\|BOOT-END' "$work/com2.log"; then
        echo "screendump $W/screen-boot$boots.ppm" | nc -U -w 2 "$work/mon" > /dev/null 2>&1
        echo "  boot $boots stopped without finishing: its screen is in $W/screen-boot$boots.ppm"
    fi
    sleep 2
    kill "$qemu_pid" 2>/dev/null; wait "$qemu_pid" 2>/dev/null; qemu_pid=
    LC_ALL=C tr -d '\r' < "$work/com2.log" >> "$work/all.log"
    LC_ALL=C grep -a -q '==INSTALLED==' "$work/com2.log" && from=c
    echo "  boot $boots from $from: $(LC_ALL=C grep -a -o '==STEP [a-z-]*' "$work/com2.log" | tr '\n' ' ')($w s)"
done
LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/all.log";   ok $? "the run reached its end in $boots boots, $(( $(date +%s) - start )) s"

O="$work/out"
mkdir -p "$O"
LC_ALL=C awk -v dir="$O" '
    /^==BEGIN / { name = $2; sub(/==$/, "", name); file = dir "/" name; printf "" > file; next }
    /^==RC /    { if (file != "") print $2 > (file ".rc"); next }
    /^==END==/  { file = ""; next }
    file != ""  { print >> file }' "$work/all.log"
rc() { tr -d ' \n' < "$O/$1.rc" 2>/dev/null; }
cp -R "$O" "$W/last-run" 2>/dev/null

echo "native-system 3: what AROS did"
for p in $pkgs; do
    [ "$(rc "adopt-$p")" = 0 ] && has "$O/adopt-$p" '^result: installed$' && has "$O/adopt-$p" '^adopted: '
                                                      ok $? "$p adopts the files copied as InstallAROS copies them ($(sed -n 's/^adopted: //p' "$O/adopt-$p" 2>/dev/null))"
done
[ "$(rc grub)" = 0 ];                                 ok $? "GRUB installed on the disk as InstallAROS installs it"
has "$work/all.log" '==STEP disk';                    ok $? "AROS booted from the disk"
has "$O/list" '^package: aros-base ';                 ok $? "the booted system lists its packages"
[ "$(rc verify1)" = 0 ] && has "$O/verify1" '^result: intact$'
                                                      ok $? "VERIFY ALL finds the booted system intact"
has "$O/verify2" '^missing: aros-base C/Dir$' && has "$O/verify2" '^changed: aros-tools Utilities/Clock$' \
  && has "$O/verify2" '^edited: aros-base S/Shell-Startup$' && [ "$(rc verify2)" != 0 ]
                                                      ok $? "after the accidents VERIFY ALL names each file with its package"
[ "$(rc repair)" = 0 ] && has "$O/repair" '^restored: C/Dir$' && has "$O/repair" '^set-aside: Utilities/Clock '
                                                      ok $? "REPAIR ALL puts both back from the channel, keeping the overwritten bytes"
[ "$(rc verify3)" = 0 ] && has "$O/verify3" '^result: intact$'
                                                      ok $? "and the system verifies intact again"
[ "$(rc dir)" = 0 ] && has "$O/dir" 'Clock';          ok $? "the restored Dir runs"
[ "$(rc shellstartup)" = 0 ];                         ok $? "the edit of S/Shell-Startup is still there"
for f in verify1 verify3; do
    [ -f "$O/$f" ] && echo "  $f: $(sed -n 's/^summary: //p' "$O/$f")"
done

echo
echo "native-system: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
