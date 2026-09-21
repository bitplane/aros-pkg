#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Demonstrate an application's update check against a local HTTP channel.
# Requirements: Python 3, build/pkg and build/example-selfupdate.
# Run: sh examples/selfupdate-demo.sh
# KEEP=1 preserves the fixtures, config and HTTP log after stopping the server.
#
# The installed application is version 1.7.0+20260921. The publisher offers version 1.7.0+20260922
# and signed release notes. Both API forms discover it over HTTP. Checking
# leaves version 1.7.0+20260921 installed; an installer is a separate application action.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
pkg="$repo/build/pkg"
example="$repo/build/example-selfupdate"
[ -x "$pkg" ] && [ -x "$example" ] || {
    echo 'Build first: make build/pkg build/example-selfupdate' >&2
    exit 1
}
command -v python3 > /dev/null 2>&1 || { echo 'This demo needs Python 3.' >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-selfupdate-demo.XXXXXX")
server=
cleanup() {
    if [ -n "$server" ]; then
        kill "$server" 2>/dev/null || true
        wait "$server" 2>/dev/null || true
    fi
    if [ "${KEEP:-0}" = 1 ]; then
        printf '\nFixtures and server.log kept in %s\n' "$work"
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
export PKG_CACHE="$work/cache"

# 1. The publisher creates a key and publishes the first signed version.
printf '\n1. Publish hello 1.7.0+20260921 and install it into a temporary root.\n'
mkdir "$work/program"
printf 'Hello version 1.7.0+20260921\n' > "$work/program/Hello"
"$pkg" KEYGEN FILE "$work/key" > /dev/null
"$pkg" PUBLISH "$work/program" NAME hello VERSION 1.7.0+20260921 KIND data \
    CHANNEL "$work/channel" SIGN "$work/key" > /dev/null
"$pkg" INSTALL hello ROOT "$work/root" CHANNEL "$work/channel" > /dev/null

# 2. The publisher changes the program and publishes with the SAME key.
# CHANGES reads a text file; these paragraphs become signed manifest fields.
printf '\n2. Publish hello 1.7.0+20260922 with its release notes.\n'
printf 'Hello version 1.7.0+20260922\n' > "$work/program/Hello"
printf 'Adds a search window.\n\nFixes opening files with spaces in their names.\n' > "$work/changes"
"$pkg" PUBLISH "$work/program" NAME hello VERSION 1.7.0+20260922 KIND data \
    CHANNEL "$work/channel" SIGN "$work/key" CHANGES "$work/changes" > /dev/null

# 3. Serve the channel only on this machine. Port 0 lets the OS choose a
# free port. Python writes that port after binding, so the client can start.
python3 - "$work/channel" "$work/port" > "$work/server.log" 2>&1 <<'PY' &
import functools
import http.server
from pathlib import Path
import sys
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=sys.argv[1])
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
Path(sys.argv[2]).write_text(str(server.server_port))
server.serve_forever()
PY
server=$!
n=0
while [ ! -s "$work/port" ]; do
    n=$((n + 1))
    if [ "$n" -ge 50 ] || ! kill -0 "$server" 2>/dev/null; then
        cat "$work/server.log" >&2
        echo 'The test server did not start.' >&2
        exit 1
    fi
    sleep 0.1
done
channel="http://127.0.0.1:$(cat "$work/port")"
printf '\n3. Test channel: %s\n' "$channel"

# 4. The application supplies its package, installation root and channel
# directly to pkg_update_check. Its version is read from the root database.
printf '\n4. Check with configuration supplied by code:\n'
"$example" hello "$work/root" "$channel"

# 5. The same values can live in a .pkgupdate file. Root is relative to this
# file; Channel names the HTTP server. The example uses pkg_update_from_file.
cat > "$work/hello.pkgupdate" <<CONFIG
Format: pkg-update 1
Package: hello
Root: root
Channel: $channel
CONFIG
printf '\n5. Check with configuration supplied by a file:\n'
"$example" --config "$work/hello.pkgupdate"

# 6. Both checks report installed 1.7.0+20260921, offered 1.7.0+20260922 and the release notes.
# The installed version is 1.7.0+20260921. Cleanup stops the server even on failure.
printf '\n6. The installed version after both checks:\n'
"$pkg" LIST ROOT "$work/root"
