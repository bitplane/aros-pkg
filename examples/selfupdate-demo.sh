#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
# A local demonstration with version 1.0 installed and version 2.0 offered.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
pkg="$repo/build/pkg"
example="$repo/build/example-selfupdate"
[ -x "$pkg" ] && [ -x "$example" ] || {
    echo 'Build first: make build/pkg build/example-selfupdate' >&2
    exit 1
}
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-selfupdate-demo.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/program"
printf 'Hello version 1.0\n' > "$work/program/Hello"
"$pkg" KEYGEN FILE "$work/key" > /dev/null
"$pkg" PUBLISH "$work/program" NAME hello VERSION 1.0 KIND data \
    CHANNEL "$work/channel" SIGN "$work/key" > /dev/null
"$pkg" INSTALL hello ROOT "$work/root" CHANNEL "$work/channel" > /dev/null
printf 'Hello version 2.0\n' > "$work/program/Hello"
printf 'Adds a search window.\n\nFixes opening files with spaces in their names.\n' > "$work/changes"
"$pkg" PUBLISH "$work/program" NAME hello VERSION 2.0 KIND data \
    CHANNEL "$work/channel" SIGN "$work/key" CHANGES "$work/changes" > /dev/null
printf '\nConfiguration supplied by code:\n'
"$example" hello "$work/root" "$work/channel"
cat > "$work/hello.pkgupdate" <<'CONFIG'
Format: pkg-update 1
Package: hello
Root: root
Channel: channel
CONFIG
printf '\nConfiguration supplied by a file:\n'
"$example" --config "$work/hello.pkgupdate"
printf '\nThe installed version is unchanged:\n'
"$pkg" LIST ROOT "$work/root"
