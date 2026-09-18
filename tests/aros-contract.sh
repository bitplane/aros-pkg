#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The command-line contract is the same on every host. One sequence of Pkg
# commands, the successes and one refusal of each class, runs twice against the
# same signed channel: on macOS with the host build, and on hosted AROS with
# the AROS build, driven by the AmigaDOS startup and nothing else. No ARexx.
#
# Checked on the host, from files AROS wrote into the share:
#   - each step's exit code, as AmigaDOS saw it in $RC, is the class code the
#     table fixes, and the macOS exit status is the same number;
#   - each MACHINE step's output is identical line for line on both hosts, once
#     the root and channel paths are written as {R} and {CH};
#   - the comparison can fail: one altered line in a copy of an AROS output is
#     reported as a difference.
#
#   sh tests/aros-contract.sh

set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
aros_pkg="$repo_root/build/aros/Pkg"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-contract.XXXXXX")
share="$work/share"
aros_started=0

cleanup() {
    status=$?
    [ "$aros_started" = 0 ] || "$control" stop >/dev/null 2>&1 || true
    if [ "$status" -ne 0 ] && [ "${PKG_KEEP_FAILURE:-0}" = 1 ]; then
        echo "aros-contract: keeping $work" >&2
        return
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

for need in "$host_pkg" "$aros_pkg" "$control"; do
    [ -x "$need" ] || { echo "aros-contract: missing $need (make; sh tools/build-aros.sh)" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "aros-contract: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}

# ---- the channel, published on the host --------------------------------

mkdir -p "$share/bin" "$share/out" "$work/host"
sh "$repo_root/tests/contract-channel.sh" "$host_pkg" "$work" "$share" \
    || { echo "aros-contract: cannot publish the channel" >&2; exit 1; }
cp "$aros_pkg" "$share/bin/Pkg"

# ---- the sequence ------------------------------------------------------

steps=$(grep -v '^#' "$repo_root/tests/contract-steps.txt")

expand() {  # expand <R> <R2> <R3> <CH> <T> <U> <args...>
    r=$1 r2=$2 r3=$3 ch=$4 t=$5 u=$6
    shift 6
    printf '%s\n' "$*" | sed -e "s|{R}|$r|g" -e "s|{R2}|$r2|g" -e "s|{R3}|$r3|g" \
        -e "s|{CH}|$ch|g" -e "s|{T}|$t|g" -e "s|{U}|$u|g"
}

# macOS: run the sequence now. The words carry no spaces, so the unquoted
# expansion splits them as intended.
H="$work/host"
printf '%s\n' "$steps" | while read -r name code args; do
    if [ "$name" = edit ]; then printf 'edited\n' > "$H/root/C/Hello"; continue; fi
    # shellcheck disable=SC2046
    "$host_pkg" $(expand "$H/root" "$H/root2" "$H/root3" "$share/channel" "$share/tampered" \
        "$share/unsigned" $args) > "$H/$name.o" 2> "$H/$name.e"
    echo $? > "$H/$name.rc"
done

# AROS: the same sequence as an AmigaDOS startup.
startup='C:FailAt 21'
nl='
'
while read -r name code args; do
    if [ "$name" = edit ]; then
        startup="$startup${nl}C:Echo edited >RAM:root/C/Hello"
        continue
    fi
    cmd=$(expand RAM:root RAM:root2 RAM:root3 MacRW:channel MacRW:tampered MacRW:unsigned $args)
    startup="$startup${nl}MacRW:bin/Pkg $cmd >MacRW:out/$name.o${nl}C:Echo \"\$RC\" >MacRW:out/$name.rc"
done <<EOF
$steps
EOF
startup="$startup${nl}C:Echo done >MacRW:done"

echo "aros-contract: booting hosted AROS"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$startup" "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 120 ]; do sleep 1; w=$((w + 1)); done
"$control" stop > /dev/null 2>&1 || true
aros_started=0
[ -f "$share/done" ];                                 ok $? "the AROS startup ran to its end"

# ---- comparison ----------------------------------------------------------

norm_host() {
    sed -e "s|$H/root3|{R3}|g" -e "s|$H/root2|{R2}|g" -e "s|$H/root|{R}|g" \
        -e "s|$share/channel|{CH}|g" -e "s|$share/tampered|{T}|g" -e "s|$share/unsigned|{U}|g" "$1"
}
norm_aros() {
    sed -e 's|RAM:root3|{R3}|g' -e 's|RAM:root2|{R2}|g' -e 's|RAM:root|{R}|g' \
        -e 's|MacRW:channel|{CH}|g' -e 's|MacRW:tampered|{T}|g' -e 's|MacRW:unsigned|{U}|g' "$1"
}

while read -r name code args; do
    [ "$name" = edit ] && continue
    hrc=$(cat "$H/$name.rc" 2>/dev/null)
    arc=$(cat "$share/out/$name.rc" 2>/dev/null | tr -d ' \r')
    [ "$hrc" = "$code" ];                             ok $? "$name: macOS exits $code (got $hrc)"
    [ "$arc" = "$code" ];                             ok $? "$name: AmigaDOS \$RC is $code (got $arc)"
    case $name in h_*) continue ;; esac
    grep -q '^result: ' "$H/$name.o";                 ok $? "$name: macOS gives a result line"
    [ ! -s "$H/$name.e" ];                            ok $? "$name: macOS writes nothing on stderr"
    norm_host "$H/$name.o" > "$work/$name.h"
    norm_aros "$share/out/$name.o" > "$work/$name.a" 2>/dev/null
    cmp -s "$work/$name.h" "$work/$name.a";           ok $? "$name: the output is identical on both hosts"
    if ! cmp -s "$work/$name.h" "$work/$name.a"; then diff "$work/$name.h" "$work/$name.a" | sed 's/^/      /'; fi
done <<EOF
$steps
EOF

grep -q "^package: hello 1.2 " "$share/out/list.o";   ok $? "the AROS list output is the package, read on the host"
grep -q '^changed: C/Hello$' "$share/out/dmg.o";      ok $? "AROS names the edited file"
grep -q '^dependency: hlib 1.0$' "$share/out/app.o" && grep -q '^orphan: hlib 1.0$' "$share/out/arm.o"
                                                      ok $? "AROS brings hlib in with happ, and reports it orphaned after"

# The comparison must be able to fail.
cp "$work/i12.a" "$work/control.a"
sed 's/^version: 1.2$/version: 1.3/' "$work/control.a" > "$work/control.b"
! cmp -s "$work/i12.h" "$work/control.b";             ok $? "control: one altered line is reported as a difference"

echo
echo "aros-contract: $checks checks, $fails failures"
if [ "$fails" -ne 0 ]; then
    for f in "$share"/out/*; do echo "--- AROS $(basename "$f")"; cat "$f"; done
fi
[ "$fails" -eq 0 ]
