#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Contrib from the nightly, on native AROS: pc-x86_64 in QEMU with a
# persistent virtual disk. The channel is the one tools/contrib/
# publish-nightly.sh made from the nightly contrib archive: signed manifests
# whose files stay in the archive. AROS partitions and formats the disk the
# first time (C:Partition, System/Format), installs Lua, Regina and AmiChess
# from the channel onto it, runs Lua and Regina, and follows the SYS/Packages
# registration the way the Startup-Sequence does; after a reboot it checks
# that everything is still there and intact, and runs them again.
#
# The archive in the test channel carries only the tested packages' files,
# byte for byte those of the nightly: in QEMU on this Mac the x86_64 CPU is
# emulated, and unpacking the whole 2.6 GB would take most of an hour per
# package. The manifests are the real ones; every file is checked against
# them as usual.
#
# Needs: sh tools/build-aros-x86_64.sh; the nightly boot ISO; the channel in
# $PKG_CONTRIB_WORK (default ~/aros-native/contrib-test), published with its
# key; qemu-system-x86_64, qemu-img, xorriso, bsdtar. The disk image stays in
# that directory between runs, so later runs skip partitioning.

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_pkg="$repo_root/build/aros-x86_64/Pkg"
W=${PKG_CONTRIB_WORK:-$HOME/aros-native/contrib-test}
iso=${AROS_X86_64_ISO:-$(ls "$HOME"/aros-native/AROS-*-pc-x86_64-boot-iso/aros-pc-x86_64.iso 2>/dev/null | tail -1)}
disk="$W/hd.img"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-contrib.XXXXXX")
qemu_pid=
cleanup() {
    [ -z "$qemu_pid" ] || kill "$qemu_pid" 2>/dev/null
    [ "${PKG_KEEP:-0}" = 1 ] && { echo "native-contrib: keeping $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

for need in "$aros_pkg" "$iso" "$W/channel/index"; do
    [ -e "$need" ] || { echo "native-contrib: missing $need" >&2; exit 69; }
done
for tool in qemu-system-x86_64 qemu-img xorriso bsdtar; do
    command -v $tool > /dev/null || { echo "native-contrib: $tool not found" >&2; exit 69; }
done
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

archive=$(ls "$W/channel/archives/"*.tar.bz2 | head -1)
aname=$(basename "$archive")
top=${aname%.tar.bz2}
pkgs="lua regina amichess"
table="$repo_root/tools/contrib/split-20260918.txt"

echo "native-contrib 1: the test channel and ISO"
T="$work/iso"
mkdir -p "$T"
bsdtar -xf "$iso" -C "$T" && chmod -R u+w "$T"
CH="$T/PkgTest/chan"
mkdir -p "$CH/archives" "$CH/objects" "$T/PkgTest/C"
cp "$W/channel/index" "$CH/"
cp "$W/channel/objects/"* "$CH/objects/"
cp "$aros_pkg" "$T/PkgTest/C/Pkg"
# The reduced archive: the tested packages' paths, taken from the nightly.
paths=""
for p in $pkgs; do
    paths="$paths $(awk -v n="$p" '$1==n{gsub(",", " ", $3); print $3}' "$table")"
done
members=""
for p in $paths; do members="$members $top/$p"; done
# one read of the archive for all of them
( cd "$work" && mkdir -p x && bsdtar -xjf "$archive" -C x $members
  cd x && COPYFILE_DISABLE=1 tar -cjf "$CH/archives/$aname" "$top" )
[ -s "$CH/archives/$aname" ];                         ok $? "the reduced archive carries the tested packages' files, as in the nightly"

run=$(date +%s)
cat > "$T/S/User-Startup" <<EOF
FailAt 21
Echo "==BOOT==" >SER1:
Assign >NIL: EXISTS DH0:
If WARN
    Echo "==STEP partition" >SER1:
    C:Partition DEVICE ata.device UNIT 0 SYSTYPE FFSIntl SYSNAME DH0 WIPE FORCE QUIET >SER1:
    Echo "==BOOT-END==" >SER1:
    C:Reboot
EndIf
If NOT EXISTS DH0:pkgtest-formatted
    Echo "==STEP format" >SER1:
    Echo "" >RAM:cr
    SYS:System/Format <RAM:cr DRIVE DH0: NAME Work FFS INTL QUICK NOICONS >SER1:
    Echo formatted >DH0:pkgtest-formatted
EndIf
Assign Extras: DH0:root/Extras
Echo "say 'regina says' 40+2" >RAM:t.rexx
; the root is not SYS:, so its Libs is added to LIBS: as SYS:Libs would be
Assign LIBS: DH0:root/Libs ADD
If NOT EXISTS DH0:pkgtest-run-$run
    Delete DH0:root ALL QUIET FORCE >NIL:
    Delete DH0:pkgtest-run-#? QUIET >NIL:
    MakeDir DH0:root
EOF
step() {  # step <name> <command...>
    printf 'Echo "==BEGIN %s==" >SER1:\n%s >SER1:\nEcho "==RC $RC" >SER1:\nEcho "==END==" >SER1:\n' "$1" "$2" \
        >> "$T/S/User-Startup"
}
P='SYS:PkgTest/C/Pkg'
S='ROOT DH0:root CHANNEL SYS:PkgTest/chan MACHINE'
for p in $pkgs; do step "i-$p" "$P INSTALL $p $S"; done
step list1 "$P LIST ROOT DH0:root MACHINE"
step status1 "$P STATUS $S"
cat >> "$T/S/User-Startup" <<'EOF'
    Assign LIBS: DH0:root/Libs ADD
EOF
step lua1 "DH0:root/Extras/Developer/Lua/Lua -v"
# The loader does not search a LIBS: directory added on this CD-booted
# system (an AROS defect, in the README), so the root is the current
# directory: the loader also looks in its libs/.
printf 'CD DH0:root\n' >> "$T/S/User-Startup"
step rexx1 "DH0:root/Extras/Regina/regina RAM:t.rexx"
printf 'CD SYS:\n' >> "$T/S/User-Startup"
cat >> "$T/S/User-Startup" <<EOF
    Echo x >DH0:pkgtest-run-$run
    Echo "==BOOT-END==" >SER1:
    C:Reboot
EndIf
Echo "==STEP after-reboot" >SER1:
EOF
step list2 "$P LIST ROOT DH0:root MACHINE"
for p in $pkgs; do step "v-$p" "$P VERIFY $p ROOT DH0:root MACHINE"; done
cat >> "$T/S/User-Startup" <<'EOF'
MakeDir ENV:SYS ENV:SYS/Packages >NIL:
Copy DH0:root/Prefs/Env-Archive/SYS/Packages/Lua ENV:SYS/Packages/ QUIET
List "ENV:SYS/Packages" NOHEAD FILES TO "T:P" LFORMAT="If EXISTS ${SYS/Packages/%N}*NCD ${SYS/Packages/%N}*NIf EXISTS S/%N-Startup*NExecute S/%N-Startup*NElse*NIf EXISTS S/Package-Startup*NExecute S/Package-Startup*NEndIf*NEndIf*NEndIf*N"
Execute "T:P"
CD SYS:
EOF
step assign "Assign LUA: EXISTS"
step lua2 "LUA:Lua -v"
printf 'CD DH0:root\n' >> "$T/S/User-Startup"
step rexx2 "DH0:root/Extras/Regina/regina RAM:t.rexx"
printf 'CD SYS:\n' >> "$T/S/User-Startup"
step rm "$P REMOVE amichess ROOT DH0:root MACHINE"
step list3 "$P LIST ROOT DH0:root MACHINE"
printf 'Echo "==PKGTEST-DONE==" >SER1:\n' >> "$T/S/User-Startup"
sed -i '' 's/^set timeout=5/set timeout=0/' "$T/boot/grub/grub.cfg"
xorriso -as mkisofs -R -J -V AROS -o "$work/test.iso" -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info "$T" > "$work/xorriso.log" 2>&1
                                                      ok $? "the test ISO is built"
rm -rf "$T" "$work/x"
[ -f "$disk" ] || qemu-img create -f raw "$disk" 2G > /dev/null

echo "native-contrib 2: boots, until the run is done"
: > "$work/all.log"
boots=0
while [ $boots -lt 5 ] && ! LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/all.log"; do
    boots=$((boots + 1))
    : > "$work/com2.log"
    qemu-system-x86_64 -m 1024 -drive file="$disk",format=raw,if=ide,index=0 -cdrom "$work/test.iso" \
        -boot d -display none -no-reboot -serial file:"$work/com1.log" -serial file:"$work/com2.log" \
        > "$work/qemu.log" 2>&1 &
    qemu_pid=$!
    w=0
    while [ $w -lt 2400 ] && kill -0 "$qemu_pid" 2>/dev/null \
          && ! LC_ALL=C grep -a -q 'PKGTEST-DONE\|BOOT-END' "$work/com2.log"; do
        sleep 5; w=$((w + 5))
    done
    sleep 2
    kill "$qemu_pid" 2>/dev/null; wait "$qemu_pid" 2>/dev/null; qemu_pid=
    LC_ALL=C tr -d '\r' < "$work/com2.log" >> "$work/all.log"
    echo "  boot $boots: $(LC_ALL=C grep -a -o '==STEP [a-z-]*' "$work/com2.log" | tr '\n' ' ')($w s)"
done
LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/all.log";   ok $? "the run reached its end in $boots boots"

O="$work/out"
mkdir -p "$O"
LC_ALL=C awk -v dir="$O" '
    /^==BEGIN / { name = $2; sub(/==$/, "", name); file = dir "/" name; printf "" > file; next }
    /^==RC /    { if (file != "") print $2 > (file ".rc"); next }
    /^==END==/  { file = ""; next }
    file != ""  { print >> file }' "$work/all.log"
rc() { tr -d ' \n' < "$O/$1.rc" 2>/dev/null; }

echo "native-contrib 3: what AROS did"
for p in $pkgs; do
    [ "$(rc "i-$p")" = 0 ] && has "$O/i-$p" '^result: installed$'; ok $? "$p installed from the nightly's archive on native AROS"
done
has "$O/list1" '^package: lua ' && has "$O/list1" '^package: regina ' && has "$O/list1" '^package: amichess '
                                                      ok $? "LIST shows the three"
has "$O/status1" '^summary: everything is up to date';  ok $? "STATUS says everything is up to date"
has "$O/lua1" '^Lua 5\.';                             ok $? "Lua runs: $(head -1 "$O/lua1" 2>/dev/null)"
has "$O/rexx1" 'regina says 42';                      ok $? "Regina runs a REXX script"
has "$work/all.log" '==STEP after-reboot';            ok $? "AROS rebooted from the disk's state"
has "$O/list2" '^package: lua ' && has "$O/list2" '^package: amichess ';  ok $? "after the reboot the packages are still listed"
for p in $pkgs; do
    has "$O/v-$p" '^result: intact$';                 ok $? "$p verifies intact after the reboot"
done
[ "$(rc assign)" = 0 ];                               ok $? "the SYS/Packages registration ran Lua's Package-Startup: LUA: exists"
has "$O/lua2" '^Lua 5\.';                             ok $? "and LUA:Lua runs"
has "$O/rexx2" 'regina says 42';                      ok $? "Regina runs again after the reboot"
[ "$(rc rm)" = 0 ] && ! has "$O/list3" '^package: amichess ';  ok $? "REMOVE takes AmiChess out"

echo
echo "native-contrib: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
