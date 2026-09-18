#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The channel every host runs the contract sequence against, so that AROS,
# macOS and Windows answer the same questions about the same bytes.
#
#   sh tests/contract-channel.sh <host pkg> <work dir> <out dir>
#
# Writes <out>/channel, <out>/tampered (one byte of hello 1.2's payload
# flipped) and <out>/unsigned (hello 1.2's signature removed). In the channel:
# hello 1.2 and 1.3 signed with one key and 1.4 with another; hlib 1.0, a
# library; happ 1, an image depending on hlib >= 1.0.

set -eu
pkg=$1 work=$2 out=$3

mkdir -p "$work/d12/C" "$work/d12/Libs" "$work/lib/Libs" "$work/app/C"
printf 'binary\000$VER: Hello 1.2 (18.9.2026)\000tail' > "$work/d12/C/Hello"
printf 'library data\n' > "$work/d12/Libs/data.txt"
cp -R "$work/d12" "$work/d13"
printf 'binary 2\000$VER: Hello 1.3 (19.9.2026)\000tail' > "$work/d13/C/Hello"
cp -R "$work/d12" "$work/d14"
printf 'binary 3\000$VER: Hello 1.4 (20.9.2026)\000tail' > "$work/d14/C/Hello"
printf 'a library\n' > "$work/lib/Libs/hlib.library"
printf 'an application\n' > "$work/app/C/HApp"
"$pkg" KEYGEN FILE "$work/dev.key" > /dev/null
"$pkg" KEYGEN FILE "$work/other.key" > /dev/null
for v in 12 13; do
    PKG_SIGNKEY="$work/dev.key" "$pkg" PUBLISH "$work/d$v" CHANNEL "$out/channel" > /dev/null
done
PKG_SIGNKEY="$work/other.key" "$pkg" PUBLISH "$work/d14" CHANNEL "$out/channel" > /dev/null
PKG_SIGNKEY="$work/dev.key" "$pkg" PUBLISH "$work/lib" CHANNEL "$out/channel" \
    NAME hlib VERSION 1.0 KIND library > /dev/null
PKG_SIGNKEY="$work/dev.key" "$pkg" PUBLISH "$work/app" CHANNEL "$out/channel" \
    NAME happ VERSION 1 KIND image DEPENDS "hlib >= 1.0" > /dev/null

m12=$(awk '$1=="hello" && $2=="1.2"{print $3}' "$out/channel/index")
p12=$(awk '/^Payload:/{print $2}' "$out/channel/objects/$m12.manifest")
cp -R "$out/channel" "$out/tampered"
python3 -c "
import sys
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=1; open(p,'wb').write(b)
" "$out/tampered/objects/$p12.pkg"
cp -R "$out/channel" "$out/unsigned"
rm "$out/unsigned/objects/$m12.sig"
