#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Goal 2 on native AROS: pc-x86_64 in QEMU, nothing hosted, no host share.
#
# Guru, Function and identify.library are taken out of the nightly ISO itself
# and published on the host (identify as a library component, Guru 2.0 and
# 2.1 as images depending on it); the ISO is rebuilt without them, with the
# channel and Pkg on it and the sequence in S:User-Startup. AROS then, from
# its own startup: installs Guru, which brings identify in; runs Guru before
# the root's Libs is visible (it must fail); mounts the image through Pkg
# MOUNTLIST with the system's own FFS, runs Guru, upgrades, rolls back,
# verifies, refuses to remove identify while Guru needs it, removes Guru and
# the orphan. Every output leaves through the second serial port; the host
# reads it and decides.
#
# Needs: sh tools/build-aros-x86_64.sh; the nightly pc-x86_64 boot ISO
# (AROS_X86_64_ISO); qemu-system-x86_64; xorriso.

set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
host_pkg="$repo_root/build/pkg"
aros_pkg="$repo_root/build/aros-x86_64/Pkg"
iso=${AROS_X86_64_ISO:-$(ls "$HOME"/aros-native/AROS-*-pc-x86_64-boot-iso/aros-pc-x86_64.iso 2>/dev/null | tail -1)}
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-native.XXXXXX")
qemu_pid=

cleanup() {
    status=$?
    [ -z "$qemu_pid" ] || kill "$qemu_pid" 2>/dev/null
    if [ "$status" -ne 0 ] && [ "${PKG_KEEP_FAILURE:-0}" = 1 ]; then
        echo "native-x86_64: keeping $work" >&2
        return
    fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

for need in "$host_pkg" "$aros_pkg" "$iso"; do
    [ -e "$need" ] || { echo "native-x86_64: missing $need" >&2; exit 69; }
done
for tool in qemu-system-x86_64 xorriso bsdtar; do
    command -v $tool > /dev/null || { echo "native-x86_64: $tool not found" >&2; exit 69; }
done

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# ---- the ISO's own pieces, published on the host -----------------------

echo "native 1: the host publishes the ISO's own Guru and identify.library"
T="$work/iso"
mkdir -p "$T"
bsdtar -xf "$iso" -C "$T"
chmod -R u+w "$T"
mkdir -p "$work/id/Libs" "$work/g20/C" "$work/g21/C"
cp "$T/Libs/identify.library" "$work/id/Libs/"
cp "$T/C/Guru" "$work/g20/C/"
cp "$T/C/Guru" "$T/C/Function" "$work/g21/C/"
printf 'Guru decodes an Amiga alert code.\n' > "$work/g20/ReadMe"
printf 'Guru decodes an Amiga alert code. Function names a library function.\n' > "$work/g21/ReadMe"
rm "$T/Libs/identify.library" "$T/C/Guru" "$T/C/Function"
[ ! -e "$T/Libs/identify.library" ];                  ok $? "identify.library is taken out of the system: only Pkg can bring it"

idver=$(LC_ALL=C strings -a "$work/id/Libs/identify.library" | awk '/\$VER: identify.library/{print $3; exit}')
CH="$T/PkgTest/chan"
"$host_pkg" KEYGEN FILE "$work/dev.key" > /dev/null
export PKG_SIGNKEY="$work/dev.key"
"$host_pkg" PUBLISH "$work/id" CHANNEL "$CH" KIND library MACHINE > "$work/p-id"
                                                      ok $? "identify.library $idver published from its own \$VER"
idm=$(awk '$1=="identify.library"{print $4}' "$CH/index")
has "$CH/objects/$idm.manifest" '^Architecture: x86_64$' && has "$work/p-id" '^arch-from: Libs/identify.library$'
                                                      ok $? "and its architecture read from its ELF header: x86_64"
"$host_pkg" PUBLISH "$work/g20" CHANNEL "$CH" NAME guru VERSION 2.0 KIND image \
    DEPENDS "identify.library >= $idver" > /dev/null; ok $? "Guru 2.0 published as an image"
"$host_pkg" PUBLISH "$work/g21" CHANNEL "$CH" NAME guru VERSION 2.1 KIND image \
    DEPENDS "identify.library >= $idver" > /dev/null; ok $? "Guru 2.1, with Function, published"
# Pkg itself reaches this machine through its own bootstrap channel, made
# as a developer would make it: the aarch64 build is in it too, and comes
# first, so the script has to find the one that runs here.
sh "$repo_root/tools/make-aros-channel.sh" "$T/PkgTest/boot" > "$work/boot.log" 2>&1
                                                      ok $? "the bootstrap channel is made on the host"
[ -f "$T/PkgTest/boot/Bootstrap/aarch64/Pkg" ] && [ -f "$T/PkgTest/boot/Bootstrap/x86_64/Pkg" ]
                                                      ok $? "with the aarch64 and the x86_64 builds"

# ---- the sequence, as S:User-Startup ------------------------------------

P='RAM:sys/C/Pkg'
S='ROOT RAM:sys CHANNEL SYS:PkgTest/chan MACHINE'
steps=""
step() {  # step <name> <command...>
    n=$1; shift
    steps="$steps $n"
    printf '%s >RAM:out/%s.o\nEcho "$RC" >RAM:out/%s.rc\n' "$*" "$n" "$n"
}
mount_as() {  # mount_as <unit> <device>
    step "m$1" $P MOUNTLIST guru ROOT RAM:sys UNIT "$1" OUT "RAM:$2" MACHINE
    printf 'Copy RAM:%s RAM:out/%s.ml\nProtect RAM:sys/guru.hdf w SUB\nMakeLink RAM:fdsk/Unit%s RAM:sys/guru.hdf\n' "$2" "$2" "$1"
    step "mount$1" Mount "RAM:$2"
}
{
    echo 'FailAt 21'
    echo 'MakeDir RAM:out RAM:fdsk'
    echo 'Assign FDSK: RAM:fdsk'
    # The channel named by its root, as a person with a DEPOT: volume would:
    # DEPOT:/Bootstrap would be the parent, so Install-Pkg must not build it.
    echo 'Assign DEPOT: SYS:PkgTest/boot'
    echo 'Execute DEPOT:Install-Pkg DEPOT: RAM:sys'
    step s00 $P VERIFY pkg ROOT RAM:sys MACHINE
    step s01 $P INSTALL guru VERSION 2.0 $S
    step s02 $P LIST ROOT RAM:sys MACHINE
    mount_as 20 GURU0
    step r19 GURU0:C/Guru 04000001
    # Observed, not required: a library in a RAM: directory added to LIBS:
    # is not found here (a CD-booted native system); see the README's AROS
    # defects. The loader also looks in libs/ under the current directory, an
    # AmigaOS rule, and that is what the rest of the run relies on.
    echo 'Assign LIBS: RAM:sys/Libs ADD'
    step d05 Version identify.library
    echo 'CD RAM:sys'
    step r20 GURU0:C/Guru 04000001
    step l20 List GURU0: ALL
    echo 'Eject GURU0:'
    step s03 $P UPGRADE guru $S
    mount_as 21 GURU1
    step r21 GURU1:C/Guru 04000001
    step l21 List GURU1: ALL
    echo 'Eject GURU1:'
    step s04 $P ROLLBACK guru $S
    step s05 $P VERIFY guru ROOT RAM:sys MACHINE
    mount_as 22 GURU2
    step r22 GURU2:C/Guru 04000001
    step l22 List GURU2: ALL
    step s06 $P REMOVE identify.library ROOT RAM:sys MACHINE
    echo 'Eject GURU2:'
    step s07 $P REMOVE guru ROOT RAM:sys MACHINE
    step s08 $P REMOVE ORPHANS ROOT RAM:sys MACHINE
    step s09 $P LIST ROOT RAM:sys MACHINE
    # Everything out through the serial ports, marked for the host.
    for n in $steps; do
        for ext in o rc; do
            printf 'Echo "==BEGIN %s.%s==" >SER1:\nType RAM:out/%s.%s >SER1:\nEcho "==END==" >SER1:\n' \
                "$n" "$ext" "$n" "$ext"
        done
    done
    for ml in GURU0 GURU1 GURU2; do
        printf 'Echo "==BEGIN %s.ml==" >SER1:\nType RAM:out/%s.ml >SER1:\nEcho "==END==" >SER1:\n' "$ml" "$ml"
    done
    echo 'Echo "==PKGTEST-DONE==" >SER1:'
    echo 'Echo "==PKGTEST-DONE==" >DEBUG:'
} > "$T/S/User-Startup"
cp "$T/S/User-Startup" "$work/user-startup.txt"

sed -i '' 's/^set timeout=5/set timeout=0/; s|multiboot2 /boot/pc/bootstrap.xz ATA=32bit \$bootstrap_flags |multiboot2 /boot/pc/bootstrap.xz ATA=32bit debug=serial $bootstrap_flags |' \
    "$T/boot/grub/grub.cfg"
xorriso -as mkisofs -R -J -V AROS -o "$work/test.iso" -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info "$T" > "$work/xorriso.log" 2>&1
                                                      ok $? "the test ISO is built"
rm -rf "$T"

# ---- native AROS in QEMU -------------------------------------------------

echo "native 2-4: pc-x86_64 AROS in QEMU, one boot, S:User-Startup"
qemu-system-x86_64 -m 1024 -cdrom "$work/test.iso" -boot d -display none -no-reboot \
    -serial file:"$work/com1.log" -serial file:"$work/com2.log" > "$work/qemu.log" 2>&1 &
qemu_pid=$!
w=0
while [ "$w" -lt 900 ] && ! LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/com2.log" "$work/com1.log" 2>/dev/null; do
    sleep 5; w=$((w + 5))
done
kill "$qemu_pid" 2>/dev/null; qemu_pid=
LC_ALL=C grep -a -q 'PKGTEST-DONE' "$work/com2.log";   ok $? "the startup ran to its end and wrote to the second serial port ($w s)"

O="$work/out"
mkdir -p "$O"
LC_ALL=C tr -d '\r' < "$work/com2.log" | awk -v dir="$O" '
    /^==BEGIN / { name = $2; sub(/==$/, "", name); file = dir "/" name; printf "" > file; next }
    /^==END==/  { file = ""; next }
    file != ""  { print >> file }'
code() { tr -d ' \r\n' < "$O/$1.rc" 2>/dev/null; }
exits() { [ "$(code "$1")" = "$2" ]; ok $? "$3: \$RC $2 (got $(code "$1"))"; }

exits s00 0 "Pkg installed itself from its channel, with one Execute line"
has "$O/s00.o" '^result: intact$';                     ok $? "and verifies intact: the x86_64 build, signed, found after the aarch64 one did not run"
exits s01 0 "Guru 2.0 installed on native AROS"
has "$O/s01.o" "^dependency: identify.library $idver\$";  ok $? "and identify.library came with it"
has "$O/s02.o" '^package: guru 2.0 image 1 explicit$' && has "$O/s02.o" '^package: pkg [0-9][0-9.+]* application 1 explicit$'
                                                      ok $? "the database lists Guru, and Pkg itself"
has "$O/r19.o" 'Could not open version .* of library "identify.library"'
                                                      ok $? "control: before the root's Libs is visible, Guru from the image cannot open identify.library"
cat > "$work/guru.expected" <<'EOF'
Alert Code: 04000001
Type:       Recoverable
Subsystem:  intuition.library
General:    General fault
Specified:  Recovery form of AN_GadgetType
EOF
guru_ok() { sed 's/^ *//; s/ *$//; /^$/d' "$O/$1.o" 2>/dev/null | cmp -s - "$work/guru.expected"; }
! has "$O/GURU0.ml" 'FileSystem';                      ok $? "MOUNTLIST names no handler: the system's own FFS mounts the image"
exits mount20 0 "Mount of the 2.0 image"
code d05 > /dev/null
echo "  observed: LIBS: with RAM:sys/Libs added finds identify.library: $( [ "$(code d05)" = 0 ] && echo yes || echo no, an AROS defect )"
guru_ok r20;                                           ok $? "Guru 2.0 runs from the mounted image on native AROS"
exits s03 0 "upgrade to 2.1"
guru_ok r21;                                           ok $? "Guru 2.1 runs from its image"
has "$O/l21.o" 'Function';                             ok $? "the 2.1 volume holds Function"
exits s04 0 "rollback"
exits s05 0 "verify after mounting"
has "$O/s05.o" '^result: intact$';                     ok $? "the image is intact after being mounted"
guru_ok r22;                                           ok $? "Guru runs from the rolled-back image"
! has "$O/l22.o" 'Function';                           ok $? "and that volume has no Function"
exits s06 16 "removing identify while Guru needs it"
exits s07 0 "Guru removed"
has "$O/s07.o" "^orphan: identify.library $idver\$";   ok $? "leaving identify an orphan"
exits s08 0 "orphans removed"
has "$O/s09.o" '^count: 1$' && has "$O/s09.o" '^package: pkg [0-9][0-9.+]* ';   ok $? "at the end the root holds Pkg alone"

echo
echo "native-x86_64: $checks checks, $fails failures"
if [ "$fails" -ne 0 ]; then
    for f in "$O"/*; do echo "--- $(basename "$f")"; LC_ALL=C cat "$f"; done
    echo "--- last of com1"; LC_ALL=C tail -c 1500 "$work/com1.log"
fi
[ "$fails" -eq 0 ]
