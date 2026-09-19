#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Publishing on AROS itself, from a Shell with the default stack (40 KB):
# KEYGEN makes a key from the system's own randomness (entropy.resource,
# through getentropy), with the output redirected and nobody at the keyboard,
# MANIFEST and PUBLISH run without a Stack command, and the host build reads
# what AROS published as "ok". Until this test, every AROS test published on
# the host.
#
# The typed-keys fallback of pkg_fs_random_typed is not exercised here: it
# only runs on an AROS with no entropy.resource, which this one has.
#
# Needs: make; tools/build-aros.sh; hosted AROS (MACAROS_ROOT).

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="${MACAROS_ROOT:-$repo_root/../Macaros}/graft/aros-ctl"
host_pkg="$repo_root/build/pkg"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-arospub.XXXXXX")
share="$work/share"
aros_started=0
cleanup() {
    [ "$aros_started" = 0 ] || "$control" stop > /dev/null 2>&1 || true
    [ "${KEEP:-0}" = 1 ] && { echo "kept $work" >&2; return; }
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
for need in "$host_pkg" "$repo_root/build/aros/Pkg" "$control"; do
    [ -e "$need" ] || { echo "aros-publish: missing $need" >&2; exit 69; }
done
"$control" status | grep -q '^state=stopped$' || {
    echo "aros-publish: a hosted AROS instance is running; refusing to disturb it" >&2
    exit 75
}
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

mkdir -p "$share/out" "$share/src/Tool/C" "$share/bin"
cp "$repo_root/build/aros/Pkg" "$share/bin/Pkg"
# the package is Pkg itself: a real AROS program of 800 KB with a $VER: string
cp "$repo_root/build/aros/Pkg" "$share/src/Tool/C/Tool"

script="FailAt 99
C:Stack >MacRW:out/stack.o
MacRW:bin/Pkg KEYGEN FILE MacRW:redirected.key >MacRW:out/keygen.o
C:Echo \"\$RC\" >MacRW:out/keygen.rc
MacRW:bin/Pkg MANIFEST MacRW:src/Tool NAME tool KIND application >MacRW:out/manifest.o
C:Echo \"\$RC\" >MacRW:out/manifest.rc
MacRW:bin/Pkg PUBLISH MacRW:src/Tool CHANNEL MacRW:channel NAME tool KIND application SIGN MacRW:redirected.key >MacRW:out/publish.o
C:Echo \"\$RC\" >MacRW:out/publish.rc
C:MakeDir RAM:root
MacRW:bin/Pkg INSTALL tool ROOT RAM:root CHANNEL MacRW:channel >MacRW:out/install.o
C:Echo \"\$RC\" >MacRW:out/install.rc
C:Echo done >MacRW:done"

echo "aros-publish: hosted AROS"
AROS_CTL_HOST_FOLDER="$share" AROS_CTL_STARTUP_EXTRA="$script" "$control" run > /dev/null 2>&1
aros_started=1
w=0
while [ ! -f "$share/done" ] && [ "$w" -lt 180 ]; do sleep 1; w=$((w + 1)); done
crash=$("$control" crash 2> /dev/null | grep -c -i -E 'stack limits|ALERT')
"$control" stop > /dev/null 2>&1 || true
aros_started=0
O="$share/out"
code() { tr -d ' \r' < "$O/$1.rc" 2>/dev/null; }

[ -f "$share/done" ];                                          ok $? "the AmigaDOS script ran to its end"
has "$O/stack.o" '40960';                                      ok $? "the Shell's stack is the default, 40960 bytes"
[ "$crash" = 0 ];                                              ok $? "no alert and no stack overrun in AROS's log"
[ "$(code keygen)" = 0 ] && has "$share/redirected.key" '^Seed: [0-9a-f]\{64\}$'
ok $? "KEYGEN makes a key from the system's randomness, with the output redirected"
[ "$(code manifest)" = 0 ] && has "$O/manifest.o" '^Architecture: aarch64$'
ok $? "MANIFEST runs within the default stack"
[ "$(code publish)" = 0 ] && has "$O/publish.o" '^published tool '
ok $? "PUBLISH runs within the default stack, signed by that key"
[ "$(code install)" = 0 ] && has "$O/install.o" '^installed tool '
ok $? "AROS installs what AROS published"
"$host_pkg" SHOW CHANNEL "$share/channel" > "$work/show" 2>&1 && has "$work/show" '^tool .* ok '
ok $? "the host build reads the channel AROS wrote: ok"
pub=$(awk '/^Public:/{print substr($2, 1, 16)}' "$share/redirected.key" 2> /dev/null)
[ -n "$pub" ] && has "$work/show" "$pub";                      ok $? "and its signer is the key AROS made"

echo
echo "aros-publish: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
