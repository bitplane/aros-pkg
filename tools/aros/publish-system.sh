#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Publishes an AROS system tree (a boot ISO's contents) as the packages of
# split-system.txt, versioned by the nightly it came from:
#
#     publish-system.sh <tree> <channel> <nightly date, e.g. 20260918>
#
# ARCH (default x86_64) is the machine the tree is for; the boot and sdk
# packages carry files of other CPUs too (GRUB's i386-pc, 32-bit libraries).
# The version is 0+<date> until AROS has release numbers; a nightly after a
# release <r> would be <r>+<date>. Every package is attempted; the
# script ends with a line per package and the time taken.

set -u
[ $# -eq 3 ] || { echo "usage: $0 <tree> <channel> <yyyymmdd>" >&2; exit 20; }
tree=$1 channel=$2 date=$3 arch=${ARCH:-x86_64}
PKG=${PKG:-$(cd "$(dirname "$0")/../.." && pwd)/build/pkg}
table=$(dirname "$0")/split-system.txt
[ -d "$tree/C" ] && [ -d "$tree/Libs" ] || { echo "$tree does not hold an AROS system (no C or Libs)" >&2; exit 11; }
start=$(date +%s)
rc=0 published=0 unchanged=0 refused=0
grep -v '^#' "$table" | while IFS='|' read -r name kind files config; do
    name=$(echo $name) kind=$(echo $kind) files=$(echo $files) config=$(echo $config)
    [ -n "$name" ] || continue
    set -- PUBLISH "$tree" FILES "$files" CHANNEL "$channel" NAME "$name" BUILD "$date" ARCH "$arch" MACHINE
    # KIND and CONFIG go with the first version; later ones inherit them.
    if ! grep -q "^$name " "$channel/index" 2>/dev/null; then
        set -- "$@" KIND "$kind"
        [ "$config" = "-" ] || set -- "$@" CONFIG "$config"
    fi
    out=$("$PKG" "$@" 2>&1); code=$?
    res=$(echo "$out" | sed -n 's/^result: //p' | head -1)
    ver=$(echo "$out" | sed -n 's/^version: //p' | head -1)
    n=$(echo "$out" | sed -n 's/^files: //p' | head -1)
    if [ $code -eq 0 ]; then
        echo "$name $ver: $res${n:+, $n files}"
    else
        echo "$name: refused ($code): $(echo "$out" | sed -n 's/^reason: //p' | head -1)"
    fi
done
echo "time: $(( $(date +%s) - start )) s"
