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
#   Bootstrap/SHA256SUMS  every file above in `shasum -a 256 -c` form, and
#   Bootstrap/SHA256SUMS.sig  its signature by the same key in OpenSSH's
#                         format, namespace aros-pkg-bootstrap: installers
#                         check the programs they fetch with ssh-keygen -Y
#                         verify before Pkg is there to check anything
#
# On AROS, one line:   Execute <channel>/Install-Pkg <channel> [<root>]
# tries each bootstrap binary until one runs on that machine, and that one
# installs the signed `pkg` package into <root> (SYS: by default) from the
# channel: from then on Pkg is a package like any other, verified, pinned to
# the publisher's key, and upgraded with `Pkg UPGRADE pkg`.
#
#   PKG_SIGNKEY=<publisher key> sh tools/make-aros-channel.sh <channel>
#
# When Pkg changes publisher, the channel's earlier versions name the old key
# and PUBLISH refuses the new one with 14; ACCEPTKEY=<the new public key>
# says the change is meant, as PUBLISH's own ACCEPTKEY does.

set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ch=${1:?usage: make-aros-channel.sh <channel>}
mkdir -p "$ch"
ch=$(CDPATH= cd -- "$ch" && pwd)
pkg="$repo_root/build/pkg"
# where people find newer versions without a network
homepage=${PKG_HOMEPAGE:-https://aros-pkg.azurewebsites.net/packages/pkg/pkg}
# the channel as an AROS machine reaches it: https, like every other system
channel_url=${PKG_CHANNEL_URL:-https://aros-pkg.azurewebsites.net/pkg}
# the host builds' drawers under Bootstrap/, as the portal names them: never
# probed on AROS, where a case-blind disk would take their pkg for Pkg
host_platforms="macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64"
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
    # what the portal and SHOW say about Pkg; later versions keep all but CHANGES
    "$pkg" PUBLISH "$work/d" CHANNEL "$ch" KIND application \
        SHORT "Installs and updates AROS software" DESCRIPTION "$repo_root/tools/pkg-about.txt" \
        CATEGORY util/sys TAGS "packages, install, update, signed" AUTHOR "John Knipper" \
        LICENSE MIT DISTRIBUTION open-source HOMEPAGE "$homepage" \
        CHANGES "$repo_root/tools/pkg-changes.txt" ${ACCEPTKEY:+ACCEPTKEY "$ACCEPTKEY"} MACHINE > "$work/out" || {
        cat "$work/out" >&2; exit 1; }
    mkdir -p "$ch/Bootstrap/$cpu"
    cp "$b" "$ch/Bootstrap/$cpu/Pkg"
    echo "make-aros-channel: pkg for $cpu, $(awk '/^result: /{print $2}' "$work/out")"
    found=$((found + 1))
done
[ "$found" -gt 0 ] || { echo "make-aros-channel: no AROS build of Pkg; run tools/build-aros.sh or tools/build-aros-x86_64.sh" >&2; exit 69; }
# The host builds, for the portal's download page: whichever `make build/pkg-macos
# build/pkg-linux-x86_64 build/pkg-linux-aarch64 build/pkg.exe` left.
host() {  # host <platform> <file name> <source>
    [ -f "$3" ] || return 0
    mkdir -p "$ch/Bootstrap/$1"
    cp "$3" "$ch/Bootstrap/$1/$2"
    echo "make-aros-channel: host build $1"
}
if [ -f "$repo_root/build/pkg-macos" ] && command -v lipo > /dev/null; then
    for a in arm64 x86_64; do
        lipo "$repo_root/build/pkg-macos" -thin $a -output "$work/pkg-macos-$a" 2>/dev/null \
            && host "macos-$a" pkg "$work/pkg-macos-$a"
    done
fi
host linux-x86_64 pkg "$repo_root/build/pkg-linux-x86_64"
host linux-arm64 pkg "$repo_root/build/pkg-linux-aarch64"
host windows-x86_64 pkg.exe "$repo_root/build/pkg.exe"

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
    echo 'Set pkgnoprobe ""'
    echo '; Each build is tried with HELP, its answer kept in RAM:, which every AROS'
    echo '; has: T: is an assign a minimal boot does not make.'
    aros_cpus=
    for d in "$ch"/Bootstrap/*/; do
        cpu=$(basename "$d")
        case " $host_platforms " in *" $cpu "*) continue ;; esac
        aros_cpus="$aros_cpus $cpu"
        cat <<EOF
If "\$pkgboot" EQ ""
    If EXISTS "PKGCH:Bootstrap/$cpu/Pkg"
        Delete RAM:pkgboot.out QUIET >NIL:
        "PKGCH:Bootstrap/$cpu/Pkg" HELP >RAM:pkgboot.out
        If NOT EXISTS RAM:pkgboot.out
            Echo "The $cpu build could not be tried: RAM:pkgboot.out could not be written."
            Set pkgnoprobe "yes"
        Else
            Search RAM:pkgboot.out "usage" QUIET >NIL:
            If NOT WARN
                Set pkgboot "PKGCH:Bootstrap/$cpu/Pkg"
                Echo "This machine runs the $cpu build."
            EndIf
        EndIf
    EndIf
EndIf
EOF
    done
    cat <<EOF
Delete RAM:pkgboot.out QUIET >NIL:
If "\$pkgboot" EQ ""
    If "\$pkgnoprobe" EQ "yes"
        Echo "Pkg could not try its builds here: the lines above say which and why."
    Else
        Echo "None of the Pkg builds in the channel's Bootstrap drawer runs on this machine."
        Echo "It holds Pkg for:$aros_cpus. For another CPU, take that CPU's drawer from the Downloads page."
        Echo "To see AROS's own reason, run one yourself: <CHANNEL>/Bootstrap/<cpu>/Pkg HELP"
    EndIf
    Assign PKGCH: REMOVE
    Quit 20
EndIf
"\$pkgboot" INSTALL pkg ROOT "<ROOT>" CHANNEL PKGCH:
If ERROR
    Echo "Pkg did not install itself; the lines above say why."
    Assign PKGCH: REMOVE
    Quit 20
EndIf
Assign PKGCH: REMOVE
Echo "Pkg is in <ROOT>C. Try: Pkg HELP."
Echo "To upgrade later, with the network started:"
Echo "  Pkg UPGRADE pkg ROOT <ROOT> CHANNEL $channel_url"
Echo "Without a network, bring the newer drawer from $homepage"
Echo "and name that drawer after CHANNEL."
EOF
} > "$ch/Install-Pkg"

cat > "$ch/ReadMe" <<EOF
This is a Pkg channel. To put Pkg on an AROS machine that can reach it:

    Execute <this directory>/Install-Pkg <this directory>

(add a root after it to install somewhere else than SYS:). Pkg then installs
itself from this channel as a signed package. With the network started,
\`Pkg UPGRADE pkg ROOT SYS: CHANNEL $channel_url\` keeps it
up to date; without one, bring the newer drawer from
$homepage and name it after CHANNEL.
EOF
# The bootstraps are programs a machine runs before Pkg can check anything:
# their digests, signed so that ssh-keygen, which every host has, verifies them.
(
    cd "$ch"
    # each drawer's entries as they are named, not a pattern per name: on a
    # file system that ignores case, Bootstrap/*/Pkg also matches .../pkg
    for f in Bootstrap/*/* Install-Pkg ReadMe; do
        case $f in Bootstrap/*/Pkg|Bootstrap/*/pkg|Bootstrap/*/pkg.exe|Install-Pkg|ReadMe) ;; *) continue ;; esac
        [ -f "$f" ] || continue
        if command -v shasum > /dev/null; then shasum -a 256 "$f"; else sha256sum "$f"; fi
    done
) > "$work/sums"
mv "$work/sums" "$ch/Bootstrap/SHA256SUMS"
"$pkg" SIGN "$ch/Bootstrap/SHA256SUMS" KEY "$PKG_SIGNKEY" OUT "$ch/Bootstrap/SHA256SUMS.sig" \
    SSH NAMESPACE aros-pkg-bootstrap MACHINE > "$work/out" || { cat "$work/out" >&2; exit 1; }
echo "make-aros-channel: Bootstrap/SHA256SUMS, $(wc -l < "$ch/Bootstrap/SHA256SUMS" | tr -d ' ') files, signed for ssh-keygen -Y verify"
echo "make-aros-channel: $ch is ready."
echo "  On each AROS machine, with <ch> the name that machine gives this directory"
echo "  (a volume such as DEPOT:, or a drawer such as Work:channel):"
echo "    Execute <ch>/Install-Pkg <ch>        (DEPOT:Install-Pkg DEPOT: for a volume root)"
