#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# A channel that puts Pkg itself on AROS machines, made on the development
# host. For every AROS build of Pkg present (build/aros/Pkg for aarch64,
# build/aros-x86_64/Pkg for x86_64, build/aros-m68k/Pkg for m68k), it
# publishes the signed package `pkg`, name and version read from Pkg's own
# $VER, the CPU from its ELF header; and it adds, beside index and objects/:
#
#   Bootstrap/<cpu>/Pkg   the same binaries, to start from
#   Install-Pkg           the AmigaDOS script that uses them
#   ReadMe                what to do on AROS
#
# On AROS, one line:   Execute <channel>/Install-Pkg <channel> [<root>]
# tries each bootstrap binary until one runs on that machine, and that one
# installs the signed `pkg` package into <root> (SYS: by default) from the
# channel: from then on Pkg is a package like any other, verified, pinned to
# the publisher's key, and upgraded with `Pkg UPGRADE pkg`.
#
#   PKG_SIGNKEY=<publisher key> sh tools/make-aros-channel.sh <channel>

set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ch=${1:?usage: make-aros-channel.sh <channel>}
mkdir -p "$ch"
ch=$(CDPATH= cd -- "$ch" && pwd)
pkg="$repo_root/build/pkg"
[ -n "${PKG_SIGNKEY:-}" ] || {
    echo "make-aros-channel: set PKG_SIGNKEY to the publisher's key (pkg KEYINFO FILE <key> names it;" >&2
    echo "  a first publisher creates one with pkg KEYGEN FILE <key>)" >&2
    exit 20
}
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-arosch.XXXXXX")
trap 'rm -rf "$work"' EXIT

found=0
for b in "$repo_root/build/aros/Pkg" "$repo_root/build/aros-x86_64/Pkg" "$repo_root/build/aros-m68k/Pkg"; do
    [ -f "$b" ] || continue
    rm -rf "$work/d"
    mkdir -p "$work/d/C"
    cp "$b" "$work/d/C/Pkg"
    cpu=$("$pkg" MANIFEST "$work/d" KIND application | awk '/^Architecture: /{print $2}')
    "$pkg" PUBLISH "$work/d" CHANNEL "$ch" KIND application MACHINE > "$work/out" || {
        cat "$work/out" >&2; exit 1; }
    mkdir -p "$ch/Bootstrap/$cpu"
    cp "$b" "$ch/Bootstrap/$cpu/Pkg"
    echo "make-aros-channel: pkg for $cpu, $(awk '/^result: /{print $2}' "$work/out")"
    found=$((found + 1))
done
[ "$found" -gt 0 ] || { echo "make-aros-channel: no AROS build of Pkg; run tools/build-aros.sh or tools/build-aros-x86_64.sh" >&2; exit 69; }

{
    echo '.KEY CHANNEL/A,ROOT'
    echo '.DEF ROOT "SYS:"'
    echo '; Puts Pkg on this machine from <CHANNEL>, verified: the bootstrap binary'
    echo '; that runs here installs the signed pkg package, which is then the one in use.'
    echo 'FailAt 21'
    echo '; PKGCH: names the channel whatever its form (DEPOT: or DEPOT:channel):'
    echo '; "<CHANNEL>/Bootstrap" would mean the parent of a volume root.'
    echo 'Assign PKGCH: "<CHANNEL>"'
    echo 'If ERROR'
    echo '    Echo "There is no channel at <CHANNEL>."'
    echo '    Quit 20'
    echo 'EndIf'
    echo 'Set pkgboot ""'
    for d in "$ch"/Bootstrap/*/; do
        cpu=$(basename "$d")
        cat <<EOF
If "\$pkgboot" EQ ""
    If EXISTS "PKGCH:Bootstrap/$cpu/Pkg"
        "PKGCH:Bootstrap/$cpu/Pkg" HELP >T:pkgboot.out
        Search T:pkgboot.out "usage" QUIET >NIL:
        If NOT WARN
            Set pkgboot "PKGCH:Bootstrap/$cpu/Pkg"
            Echo "This machine runs the $cpu build."
        EndIf
    EndIf
EndIf
EOF
    done
    cat <<'EOF'
Delete T:pkgboot.out QUIET >NIL:
If "$pkgboot" EQ ""
    Echo "None of the Pkg builds in the channel's Bootstrap drawer runs on this machine."
    Assign PKGCH: REMOVE
    Quit 20
EndIf
"$pkgboot" INSTALL pkg ROOT "<ROOT>" CHANNEL PKGCH:
If ERROR
    Echo "Pkg did not install itself; the lines above say why."
    Assign PKGCH: REMOVE
    Quit 20
EndIf
Assign PKGCH: REMOVE
Echo "Pkg is in <ROOT>C. Try: Pkg HELP. Later: Pkg UPGRADE pkg ROOT <ROOT> CHANNEL <CHANNEL>"
EOF
} > "$ch/Install-Pkg"

cat > "$ch/ReadMe" <<'EOF'
This is a Pkg channel. To put Pkg on an AROS machine that can reach it:

    Execute <this directory>/Install-Pkg <this directory>

(add a root after it to install somewhere else than SYS:). Pkg then installs
itself from this channel as a signed package; `Pkg UPGRADE pkg ROOT SYS:
CHANNEL <this directory>` keeps it up to date.
EOF
echo "make-aros-channel: $ch is ready."
echo "  On each AROS machine, with <ch> the name that machine gives this directory"
echo "  (a volume such as DEPOT:, or a drawer such as Work:channel):"
echo "    Execute <ch>/Install-Pkg <ch>        (DEPOT:Install-Pkg DEPOT: for a volume root)"
