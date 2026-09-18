#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The image writer judged by an FFS implementation written apart from it:
# amitools. For each drawer, `pkg IMAGE` writes an image; amitools validates
# it (boot and root blocks, directory tree, every file's block chain, and the
# bitmap against the blocks in use), unpacks it, and the unpacked tree must
# equal the drawer byte for byte. Two damaged images must fail validation, so
# the oracle is shown able to say no.
#
# amitools lives in a virtualenv: AMITOOLS names its bin directory, by default
# ~/aros-build-tools/amitools/bin. To make one:
#   python3 -m venv ~/aros-build-tools/amitools
#   ~/aros-build-tools/amitools/bin/pip install amitools

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PKG=${PKG:-"$repo_root/build/pkg"}
AMI=${AMITOOLS:-"$HOME/aros-build-tools/amitools/bin"}
[ -x "$AMI/python3" ] && [ -x "$AMI/xdftool" ] || {
    echo "image: amitools not found in $AMI; see the comment at the top of this file" >&2
    exit 69
}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-image.XXXXXX")
trap 'rm -rf "$T"' EXIT

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
validate() { "$AMI/python3" "$repo_root/tools/ffs-validate.py" "$1" > "$T/validate.out" 2>&1; }

# One drawer per shape worth proving.
mkdir -p "$T/small/C" "$T/small/Libs"
printf 'hello\n' > "$T/small/C/Hello"
printf 'data\n' > "$T/small/Libs/data.txt"
: > "$T/small/empty"

mkdir -p "$T/big/C"
# 5 MB: files with extension-block chains, data running past the root block,
# and three bitmap blocks.
head -c 5000000 /dev/urandom > "$T/big/C/big.bin"
head -c 36864 /dev/urandom > "$T/big/C/exactly72.bin"          # 72 blocks: one full header table
head -c 37376 /dev/urandom > "$T/big/C/73blocks.bin"           # one past it
printf 'x' > "$T/big/one"

mkdir -p "$T/many/a/b/c/d/e/f"
i=0
while [ $i -lt 400 ]; do printf "file %d\n" $i > "$T/many/a/f$i"; i=$((i + 1)); done
printf 'deep\n' > "$T/many/a/b/c/d/e/f/leaf"
printf 'spaces\n' > "$T/many/a/with some spaces.txt"
printf 'thirty\n' > "$T/many/a/abcdefghijklmnopqrstuvwxyz1234"

for d in small big many; do
    "$PKG" IMAGE "$T/$d" OUT "$T/$d.hdf" NAME "Vol$d" > /dev/null
                                                      ok $? "$d: pkg IMAGE writes it"
    validate "$T/$d.hdf";                             ok $? "$d: amitools validates it, $(tail -1 "$T/validate.out" | sed 's/.*hdf, //')"
    mkdir "$T/$d.out"
    "$AMI/xdftool" "$T/$d.hdf" unpack "$T/$d.out" > /dev/null 2>&1
    diff -r "$T/$d" "$T/$d.out/Vol$d" > /dev/null;    ok $? "$d: amitools unpacks exactly the drawer"
done

"$PKG" IMAGE "$T/big" OUT "$T/big2.hdf" NAME Volbig > /dev/null
cmp -s "$T/big.hdf" "$T/big2.hdf";                    ok $? "the same drawer gives the same image"

# The oracle must be able to fail.
python3 - "$T/small.hdf" "$T/bad-root.hdf" "$T/bad-map.hdf" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
root = (len(b) // 512 + 1) // 2
r = bytearray(b); r[root * 512 + 300] ^= 1
open(sys.argv[2], 'wb').write(r)
m = bytearray(b); bm = root + 1
ls = list(struct.unpack('>128I', bytes(m[bm * 512:bm * 512 + 512])))
ls[1] |= 1                        # block 2, in use, marked free
ls[0] = 0; ls[0] = (-sum(ls)) & 0xffffffff
m[bm * 512:bm * 512 + 512] = struct.pack('>128I', *ls)
open(sys.argv[3], 'wb').write(m)
PY
! validate "$T/bad-root.hdf";                         ok $? "control: a root block with one flipped bit fails validation"
! validate "$T/bad-map.hdf";                          ok $? "control: a bitmap freeing a used block fails validation"

echo "image: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
