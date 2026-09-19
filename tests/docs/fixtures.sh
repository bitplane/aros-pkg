#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# What the documentation's examples find when they run:
#
#   $1/channel        a channel: helloworld 1.0 and 1.1, which need hellolib,
#                     hellolib 1.0, and notes 1.0, a program as an image
#   $1/portal/contrib-nightly   stands for the portal's channel of that name
#   $1/drawers/MyTool, MyTool-1.1   a program's drawer, as a publisher has it
#
# All signed with fixture.key, so that every output is the same on every run.

set -eu
out=$1
PKG=${PKG:-pkg}
key=$(cd "$(dirname "$0")" && pwd)/fixture.key
export PKG_SIGNKEY="$key"
d=$(mktemp -d "${TMPDIR:-/tmp}/pkg-docfix.XXXXXX")
trap 'rm -rf "$d"' EXIT
mkdir -p "$out/drawers"

elf() {  # elf <drawer> <file> <name> <version>: an x86_64 AROS program, as far as a header goes
    mkdir -p "$(dirname "$1/$2")"
    python3 -c "
import sys
h = bytearray(64); h[0:4] = b'\\x7fELF'; h[4] = 2; h[5] = 1; h[6] = 1; h[18] = 62
open(sys.argv[1], 'wb').write(bytes(h) + ('\\0\$VER: %s %s (19.9.2026)\\0' % (sys.argv[2], sys.argv[3])).encode())
" "$1/$2" "$3" "$4"
}

prog() {  # prog <drawer> <file> <name> <version>
    mkdir -p "$(dirname "$1/$2")"
    printf 'x\000$VER: %s %s (19.9.2026)\000' "$3" "$4" > "$1/$2"
}

prog "$d/hellolib" Libs/hello.library hello.library 1.0
"$PKG" PUBLISH "$d/hellolib" CHANNEL "$out/channel" NAME hellolib VERSION 1.0 KIND library > /dev/null
for v in 1.0 1.1; do
    prog "$d/hw$v" C/HelloWorld helloworld $v
    mkdir -p "$d/hw$v/S"
    printf 'Greeting=Hello\n' > "$d/hw$v/S/HelloWorld.prefs"
done
printf 'Greeting=Hello\nColour=Blue\n' > "$d/hw1.1/S/HelloWorld.prefs"
"$PKG" PUBLISH "$d/hw1.0" CHANNEL "$out/channel" KIND application DEPENDS hellolib \
    CONFIG S/HelloWorld.prefs > /dev/null
"$PKG" PUBLISH "$d/hw1.1" CHANNEL "$out/channel" > /dev/null
prog "$d/notes" Notes notes 1.0
printf 'Notes needs nothing else\n' > "$d/notes/ReadMe"
"$PKG" PUBLISH "$d/notes" CHANNEL "$out/channel" KIND image > /dev/null

mkdir -p "$d/lua/Extras/Developer/Lua"
printf 'lua\n' > "$d/lua/Extras/Developer/Lua/Lua"
"$PKG" PUBLISH "$d/lua" CHANNEL "$out/portal/contrib-nightly" NAME lua BUILD 20260918 \
    ARCH x86_64 KIND application > /dev/null

elf "$out/drawers/MyTool" C/MyTool mytool 1.0
elf "$out/drawers/MyTool-1.1" C/MyTool mytool 1.1
mkdir -p "$out/drawers/MyTool-1.1/S"
printf 'Window=640x480\n' > "$out/drawers/MyTool-1.1/S/MyTool.prefs"

# someone else's archive, in the shape of a nightly contrib archive
mkdir -p "$d/arc/Top/Extras/Tool" "$d/arc/Top/Extras/Other"
elf "$d/arc/Top/Extras/Tool" Tool tool 2.1
prog "$d/arc/Top/Extras/Other" Other other 1.0
(cd "$d/arc" && COPYFILE_DISABLE=1 tar -cjf "$out/drawers/nightly.tar.bz2" Top)
