#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# How much room the portal takes, as a maintainer reads it: what each channel
# holds on this machine, what the portal keeps for itself, what is in R2, and
# how much would move there. The numbers are checked against files of a known
# size, so a wrong one is a failure and not a matter of opinion.
#
# Needs: make; the portal built (dotnet build portal/src/Portal -c Release).

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
dll="$repo_root/portal/src/Portal/bin/Release/net10.0/Portal.dll"
export PATH="$HOME/.dotnet:$PATH"
command -v dotnet > /dev/null && [ -f "$dll" ] || { echo "space: the portal is not built; skipped" >&2; exit 0; }
newer=$(find "$repo_root/portal/src/Portal" \( -name obj -o -name bin \) -prune -o \
        \( -name '*.cs' -o -name '*.cshtml' \) -newer "$dll" -print -quit)
[ -z "$newer" ] || { echo "space: $dll is older than $newer; rebuild it (dotnet build portal/src/Portal -c Release)" >&2; exit 69; }
T=$(mktemp -d); port=5086; site="http://127.0.0.1:$port"
trap 'kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
unset PKG_PUSHKEY PKG_SIGNKEY; export PKG_CACHE="$T/cache"

# A channel with files of sizes we chose: 4 KiB of package objects and a
# 64 KiB archive, plus one archive already in R2 (a .url of 30 bytes).
ch="$T/data/channels/tools"
mkdir -p "$ch/objects" "$ch/archives"
printf 'tool 1.0 generic %064d\n' 1 > "$ch/index"
dd if=/dev/zero of="$ch/objects/$(printf '%064d' 1).manifest" bs=1024 count=4 2>/dev/null
dd if=/dev/zero of="$ch/archives/big.tar.bz2" bs=1024 count=64 2>/dev/null
printf 'https://pub-example.r2.dev/tools/archives/gone.tar.bz2\n' > "$ch/archives/gone.tar.bz2.url"

admin=$(cd "$repo_root/portal/src/Portal" && dotnet "$dll" adminkey tester)
key=$(printf '%s\n' "$admin" | awk '/^key:/{print $2}')
config=$(printf '%s\n' "$admin" | awk '/^config:/{print $2}')
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" \
  Portal__AdminKeys="$config" Portal__AllowLoopbackHttpPush=true ASPNETCORE_URLS="$site" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done

space() { curl -fs -H "Authorization: Bearer $key" "$site/_admin/space$1"; }
space "" > "$T/s1"
                                                        ok $? "a maintainer reads how much room the portal takes"
grep -q '^result: shown' "$T/s1";                       ok $? "in a record, as every admin answer is"
curl -s -o "$T/anon" -w '%{http_code}' "$site/_admin/space" | grep -q '^401'
                                                        ok $? "without the key, nothing (401)"

# 4 KiB of objects, 64 KiB of archive, 65 bytes of index, 55 of .url.
want_objects=4096
want_archives=$((65536 + 55))
grep -q "^channel: tools .* objects $want_objects, archives $want_archives," "$T/s1"
                                                        ok $? "the channel's files are counted as they are on disk"
grep -q "^movable: $want_archives bytes in 2 files would go to R2" "$T/s1"
                                                        ok $? "and the archives are named as what R2 would take"
grep -q '^r2: R2 is not configured' "$T/s1";            ok $? "with no R2 configured, it says so instead of guessing"
disk=$(awk '/^disk:/{print $2}' "$T/s1")
[ "$disk" -ge $((4096 + 65536)) ];                      ok $? "the total covers at least the files we wrote ($disk bytes)"
grep -q '^volume: [0-9]* bytes free of [0-9]*' "$T/s1"; ok $? "and the volume says what is left on it"

# Counting walks every file, so a reading is kept and given again.
taken=$(awk '/^taken:/{print $2}' "$T/s1")
space "" > "$T/s2"
[ "$(awk '/^taken:/{print $2}' "$T/s2")" = "$taken" ];  ok $? "asked again, the same reading is given, not a new walk"
sleep 1
space "?fresh" > "$T/s3"
[ "$(awk '/^taken:/{print $2}' "$T/s3")" != "$taken" ]; ok $? "asking for a fresh one walks again"

# A file added is seen by the next fresh reading, and only by it.
dd if=/dev/zero of="$ch/objects/$(printf '%064d' 2).pkg" bs=1024 count=8 2>/dev/null
space "" > "$T/s4"
grep -q "objects $want_objects," "$T/s4";               ok $? "the kept reading does not change under it"
space "?fresh" > "$T/s5"
grep -q "objects $((want_objects + 8192))," "$T/s5";    ok $? "the fresh one counts the new file"

echo "space: $((checks - fails))/$checks checks passed"
[ "$fails" -eq 0 ]
