#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The test kit for Windows, macOS and Linux: Pkg built for each (Windows with
# mingw-w64, macOS universal, Linux static with zig), the contract channel,
# the shared step list, what this reference build answers to each step, and
# tests/kit/run.ps1 to compare on the target. Output: build/pkg-test-kit.zip.
#
# Needs x86_64-w64-mingw32-gcc (Homebrew: mingw-w64) and zig. The x86_64
# Windows binary also runs on Windows on Arm, under its x64 emulation.

set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
make -s build/pkg build/pkg.exe build/pkg-macos build/pkg-linux-x86_64 build/pkg-linux-aarch64
kit=build/pkg-test-kit
rm -rf "$kit" build/pkg-test-kit.zip
mkdir -p "$kit/expected" "$kit/bin/windows-x86_64" "$kit/bin/macos" "$kit/bin/linux-x86_64" "$kit/bin/linux-aarch64"
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-winkit.XXXXXX")
trap 'rm -rf "$work"' EXIT

cp tests/kit/run.ps1 "$kit/"
cp build/pkg.exe "$kit/bin/windows-x86_64/pkg.exe"
cp build/pkg-macos "$kit/bin/macos/pkg"
cp build/pkg-linux-x86_64 "$kit/bin/linux-x86_64/pkg"
cp build/pkg-linux-aarch64 "$kit/bin/linux-aarch64/pkg"
grep -v '^#' tests/contract-steps.txt > "$kit/steps.txt"
sh tests/contract-channel.sh "$repo_root/build/pkg" "$work" "$kit"

# macOS's answers, with every path written as its placeholder.
H="$work/host"
mkdir -p "$H"
grep -v '^#' tests/contract-steps.txt | while read -r name code args; do
    if [ "$name" = edit ]; then printf 'edited\n' > "$H/root/C/Hello"; continue; fi
    # shellcheck disable=SC2086
    set -- $(printf '%s\n' "$args" | sed -e "s|{R3}|$H/root3|g" -e "s|{R2}|$H/root2|g" \
        -e "s|{R}|$H/root|g" -e "s|{CH}|$repo_root/$kit/channel|g" \
        -e "s|{T}|$repo_root/$kit/tampered|g" -e "s|{U}|$repo_root/$kit/unsigned|g")
    rc=0
    ./build/pkg "$@" > "$H/out" 2>/dev/null || rc=$?
    [ "$rc" = "$code" ] || { echo "make-test-kit: $name exits $rc on macOS, not $code" >&2; exit 1; }
    sed -e "s|$H/root3|{R3}|g" -e "s|$H/root2|{R2}|g" -e "s|$H/root|{R}|g" \
        -e "s|$repo_root/$kit/channel|{CH}|g" -e "s|$repo_root/$kit/tampered|{T}|g" \
        -e "s|$repo_root/$kit/unsigned|{U}|g" "$H/out" > "$kit/expected/$name.o"
done

mkdir -p "$work/drawer/C"
printf 'tool\n' > "$work/drawer/C/Tool"
printf '\001\002\003' > "$work/drawer/Thumbs.db"
./build/pkg IMAGE "$work/drawer" OUT "$work/tool.hdf" NAME Tool > /dev/null
shasum -a 256 "$work/tool.hdf" | cut -c1-64 > "$kit/expected/image.sha256"

cat > "$kit/README.txt" <<'TXT'
Pkg: the contract test, for Windows, macOS and Linux

1. Unzip anywhere.
2. Run, in that folder:
     Windows:       powershell -ExecutionPolicy Bypass -File run.ps1
     macOS, Linux:  pwsh run.ps1          (PowerShell 7)
   On macOS, a kit downloaded through a browser is quarantined; clear it
   first with:  xattr -dr com.apple.quarantine .
3. It prints "N checks, M failures" and writes report.txt. Send report.txt back.

It runs the same Pkg sequence hosted AROS runs, against the same signed
channel in this folder, and compares every exit code and every
machine-readable output with the reference answers in expected/. Nothing is
written outside the "work" folder it creates here.
TXT
(cd build && zip -qr pkg-test-kit.zip pkg-test-kit)
echo "make-test-kit: $repo_root/build/pkg-test-kit.zip"
