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
# partitioning; PKG_FRESH=1 starts from an empty disk. PKG_DISPLAY=cocoa
# shows the AROS screen in a window on a Mac while it runs.

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
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

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

# The system partition as InstallAROS makes it, in a logical partition of an
# MBR disk: SFS, its other choice, since GRUB aborts reading an FFS one
# ("alloc magic is broken") and FFS holds no name over 30 characters, which
# the ISO has; PKG_SYSFS=FFSIntl for InstallAROS's default.
# PKG_SYSSIZE in MB.
sysfs=${PKG_SYSFS:-SFS}
syssize=${PKG_SYSSIZE:-2048}
scheme=${PKG_SCHEME:-mbr}     # rdb would put the RDB where Install-grub2 writes GRUB
case $sysfs in FFSIntl) fmtflags="FFS INTL" ;; *) fmtflags="" ;; esac

# What InstallAROS copies, less Developer (an option there too).
pkgs="aros-boot aros-base aros-prefs aros-fonts aros-locale aros-tools aros-demos aros-extras"
drawers="boot efi C L Libs Devs S Classes System Rexxc Storage WBStartup Prefs Fonts Locale Tools Utilities Demos Extras"

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
# Everything runs in a Shell window on the Workbench screen, where it can be
# watched and recorded: User-Startup only opens that window, and the boot
# goes on to Wanderer. Each step shows its command and Pkg's own output,
# which also goes to the second serial port, where this script reads it.
S="$T/S/pkg-steps"
step() {  # step <name> <command shown> <command run>: Pkg, live, a copy by LOG
    cat >> "$S" <<EOF
Echo "*N*E[1m1> $2*E[0m"
Delete T:o QUIET >NIL:
$3 LOG T:o
Echo "==RC \$RC" >T:rc
Echo "==BEGIN $1==" >SER1:
Type T:o >SER1:
Type T:rc >SER1:
Echo "==END==" >SER1:
EOF
}
cmd() {  # cmd <name> <command shown> <command run>: another program, shown when done
    cat >> "$S" <<EOF
Echo "*N*E[1m1> $2*E[0m"
$3 >T:o
Echo "==RC \$RC" >T:rc
Type T:o
Echo "==BEGIN $1==" >SER1:
Type T:o >SER1:
Type T:rc >SER1:
Echo "==END==" >SER1:
EOF
}
say() {  # say <line>: a comment shown in the window
    printf 'Echo "*N*E[32m; %s*E[0m"\n' "$1" >> "$S"
}
cat > "$T/S/User-Startup" <<'EOF'
Echo "==BOOT==" >SER1:
Run >NIL: C:NewShell "CON:0/16/800/584/Pkg on AROS/AUTO/WAIT" FROM S:pkg-demo
EOF
cat > "$T/S/pkg-demo" <<'EOF'
Wait 12
Execute S:pkg-steps
EOF
cat > "$S" <<EOF
FailAt 21
SetEnv PKG_PROGRESS 1
If EXISTS SYS:pkgtest-installed
    Skip disk
EndIf
Assign >NIL: EXISTS DH0:
If WARN
    Echo "==STEP partition" >SER1:
    Echo "*N*E[32m; An empty disk: partitioned as InstallAROS partitions it*E[0m"
    C:Partition DEVICE ata.device UNIT 0 SCHEME $scheme SYSSIZE $syssize SYSTYPE $sysfs SYSNAME DH0 MAXWORK WORKTYPE SFS WORKNAME DH1 WIPE FORCE QUIET
    Echo "==BOOT-END==" >SER1:
    Wait 3
    C:Reboot
EndIf
If EXISTS DH0:pkgtest-installed
    Echo "==DISK-BOOT-FAILED==" >SER1:
    Echo "==BOOT-END==" >SER1:
    Quit
EndIf
Echo "==STEP install" >SER1:
Echo "*N*E[32m; Formatting System and Work*E[0m"
Echo "" >RAM:cr
SYS:System/Format <RAM:cr DRIVE DH0: NAME System $fmtflags QUICK NOICONS >NIL:
SYS:System/Format <RAM:cr DRIVE DH1: NAME Work QUICK NOICONS >NIL:
Echo "*N*E[32m; Copying AROS onto System, as InstallAROS copies it*E[0m"
EOF
for d in $drawers; do
    printf 'Echo "  %s"\nCopy SYS:%s DH0:%s ALL CLONE QUIET\n' "$d" "$d" "$d" >> "$S"
done
printf 'Copy SYS:#?.info DH0: CLONE QUIET\n' >> "$S"
printf 'Copy SYS:AROS.boot DH0: CLONE QUIET\n' >> "$S"   # the boot signature dos.library looks for
printf 'Copy CD0:PkgTest/eltorito.img DH0:boot/grub/i386-pc/eltorito.img CLONE QUIET\n' >> "$S"
say "Pkg takes over the files it finds there, package by package"
# PKG_TRACE_AROS=1: Pkg's trace of each adoption goes to the second serial
# port, ahead of the step's own output, to see where a slow one spends it.
tr=; [ "${PKG_TRACE_AROS:-0}" = 1 ] && tr=" TRACE SER1:"
# PKG_ADOPT=0: no Pkg at all before the reboot, as a control of the disk boot.
if [ "${PKG_ADOPT:-1}" != 0 ]; then
    for p in $pkgs; do step "adopt-$p" "pkg INSTALL $p ROOT DH0:" "$P INSTALL $p ROOT DH0: $C$tr"; done
fi
say "The boot loader, as InstallAROS installs it"
cmd grub "Install-grub2 DEVICE ata.device UNIT 0 GRUB DH0:boot/grub" 'C:Install-grub2 DEVICE ata.device UNIT 0 GRUB DH0:boot/grub'
cat >> "$S" <<'EOF'
Copy CD0:PkgTest/C/Pkg DH0:C/Pkg CLONE QUIET
Echo x >DH0:pkgtest-installed
Echo "*N*E[32m; Installed. Rebooting from the disk*E[0m"
; the file systems write out what they hold before the reboot
C:Lock DH0: ON >NIL:
Wait 5
Echo "==INSTALLED==" >SER1:
Echo "==BOOT-END==" >SER1:
C:Reboot
Lab disk
Echo "==STEP disk" >SER1:
Echo "*N*E[32m; Booted from the disk. What Pkg knows of this system*E[0m"
EOF
step list "pkg LIST ROOT SYS:" "C:Pkg LIST ROOT SYS:"
step verify1 "pkg VERIFY ALL ROOT SYS:" "C:Pkg VERIFY ALL ROOT SYS:"
say "Accidents: C:Dir deleted, Clock overwritten; and Shell-Startup edited on purpose"
cat >> "$S" <<'EOF'
Echo "1> Delete SYS:C/Dir"
Delete SYS:C/Dir QUIET
Echo "1> Echo >SYS:Utilities/Clock ..."
Echo "overwritten by accident" >SYS:Utilities/Clock
Echo "1> Echo >>SYS:S/Shell-Startup ..."
Echo "; my own line" >>SYS:S/Shell-Startup
EOF
step verify2 "pkg VERIFY ALL ROOT SYS:" "C:Pkg VERIFY ALL ROOT SYS:"
step repair "pkg REPAIR ALL ROOT SYS: CHANNEL <the CD>" "C:Pkg REPAIR ALL ROOT SYS: $C"
step verify3 "pkg VERIFY ALL ROOT SYS:" "C:Pkg VERIFY ALL ROOT SYS:"
cmd dir "Dir SYS:Utilities" "SYS:C/Dir SYS:Utilities"
cmd shellstartup "Search SYS:S/Shell-Startup my-own-line" "Search SYS:S/Shell-Startup \"my own line\""
cat >> "$S" <<'EOF'
Echo "*N*E[32m; Done*E[0m"
Echo "==PKGTEST-DONE==" >SER1:
EOF
xorriso -as mkisofs -R -J -V AROS -o "$work/test.iso" -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info "$T" > "$work/xorriso.log" 2>&1
                                                      ok $? "the test CD is built"
# After the install the CD only carries the channel: without AROS.boot it is
# no boot volume, so AROS starts from the disk, and REPAIR still reads it.
rm -f "$T/AROS.boot"
xorriso -as mkisofs -R -J -V AROS -o "$work/channel.iso" "$T" > "$work/xorriso2.log" 2>&1
                                                      ok $? "the channel CD is built"
rm -rf "$T"
[ "${PKG_FRESH:-0}" = 1 ] && rm -f "$disk"
[ -f "$disk" ] || qemu-img create -f raw "$disk" 4G > /dev/null

echo "native-system 2: boots, until the run is done"
: > "$work/all.log"
boots=0
from=d
start=$(date +%s)
# What the screen shows, boot after boot, for tools/qemu-video.sh; the last
# frames of a boot that hangs show where. PKG_FRAMES=0 records nothing.
frames="$W/frames"
rm -rf "$frames"
while [ $boots -lt 6 ] && ! LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/all.log"; do
    boots=$((boots + 1))
    : > "$work/com2.log"
    cd_iso="$work/test.iso"; [ "$from" = c ] && cd_iso="$work/channel.iso"
    qemu-system-x86_64 -m 2048 -drive file="$disk",format=raw,if=ide,index=0 -cdrom "$cd_iso" \
        -boot $from -display "${PKG_DISPLAY:-none}" -name "Pkg on AROS - boot $boots" -no-reboot -serial file:"$work/com1.log" -serial file:"$work/com2.log" \
        -monitor unix:"$work/mon",server,nowait > "$work/qemu.log" 2>&1 &
    qemu_pid=$!
    cap_pid=
    if [ "${PKG_FRAMES:-1}" != 0 ]; then
        python3 "$repo_root/tools/qemu-frames.py" "$work/mon" "$frames" "${PKG_FRAME_INTERVAL:-1}" &
        cap_pid=$!
    fi
    w=0
    while [ $w -lt 5400 ] && kill -0 "$qemu_pid" 2>/dev/null \
          && ! LC_ALL=C grep -a -q 'PKGTEST-DONE\|BOOT-END' "$work/com2.log"; do
        sleep 5; w=$((w + 5))
        # nothing on either serial port after five minutes: it never started
        [ $w -ge 300 ] && [ ! -s "$work/com1.log" ] && [ ! -s "$work/com2.log" ] && break
    done
    if ! LC_ALL=C grep -a -q 'PKGTEST-DONE\|BOOT-END' "$work/com2.log"; then
        echo "  boot $boots stopped without finishing: its last screens are the last frames in $frames"
    fi
    sleep 2
    kill "$qemu_pid" 2>/dev/null; wait "$qemu_pid" 2>/dev/null; qemu_pid=
    [ -z "$cap_pid" ] || wait "$cap_pid" 2>/dev/null
    LC_ALL=C tr -d '\r' < "$work/com2.log" >> "$work/all.log"
    LC_ALL=C grep -a -q '==INSTALLED==' "$work/com2.log" && from=c
    if LC_ALL=C grep -a -q '==DISK-BOOT-FAILED==' "$work/com2.log"; then
        echo "  boot $boots: the installed disk did not boot; AROS came up from the CD"
        break
    fi
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
    [ "$(rc "adopt-$p")" = 0 ] && has "$O/adopt-$p" "^installed $p " && has "$O/adopt-$p" '^  adopted  '
                                                      ok $? "$p adopts the files copied as InstallAROS copies them ($(sed -n 's/^  adopted  \([0-9]*\).*/\1/p' "$O/adopt-$p" 2>/dev/null))"
done
[ "$(rc grub)" = 0 ];                                 ok $? "GRUB installed on the disk as InstallAROS installs it"
has "$work/all.log" '==STEP disk' && ! has "$work/all.log" '==DISK-BOOT-FAILED=='
                                                      ok $? "AROS booted from the disk"
has "$O/list" 'aros-base';                            ok $? "the booted system lists its packages"
[ "$(rc verify1)" = 0 ] && has "$O/verify1" 'all intact'
                                                      ok $? "VERIFY ALL finds the booted system intact"
# Each package is a row of the table, its files under it: the file line
# follows its package's row before any other package's row.
under() { awk -v pkg="$2" -v line="$3" 'index($0, pkg " ") == 1 { in_pkg = 1; next }
    /^[^ ]/ { in_pkg = 0 } in_pkg && index($0, line) == 1 { found = 1 } END { exit !found }' "$1"; }
under "$O/verify2" aros-base '  missing  C/Dir' && under "$O/verify2" aros-tools '  changed  Utilities/Clock' \
  && under "$O/verify2" aros-base '  edited   S/Shell-Startup' && has "$O/verify2" 'missing' \
  && has "$O/verify2" 'packages damaged' && [ "$(rc verify2)" != 0 ]
                                                      ok $? "after the accidents VERIFY ALL lists each file under its package"
[ "$(rc repair)" = 0 ] && has "$O/repair" '^  restored C/Dir' && has "$O/repair" '^  aside    Utilities/Clock -> '
                                                      ok $? "REPAIR ALL puts both back from the channel, keeping the overwritten bytes"
[ "$(rc verify3)" = 0 ] && has "$O/verify3" 'all intact'
                                                      ok $? "and the system verifies intact again"
[ "$(rc dir)" = 0 ] && has "$O/dir" 'Clock';          ok $? "the restored Dir runs"
[ "$(rc shellstartup)" = 0 ];                         ok $? "the edit of S/Shell-Startup is still there"
for f in verify1 verify3; do
    [ -f "$O/$f" ] && echo "  $f: $(tail -1 "$O/$f")"
done
[ -s "$frames/frames.txt" ] && echo "  the screen was recorded: sh tools/qemu-video.sh $frames <out.mp4>"

echo
echo "native-system: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
