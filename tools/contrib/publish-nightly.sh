#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Publish a nightly contrib archive as packages, one per line of a split
# table (`<name> <kind> <path>[,<path>...]`, # for comments), each taking its
# version from its own $VER plus the nightly's date. The archive is read
# once, for its index; a package whose files equal its last version's is not
# published again. Ends with what was done, what was avoided, and how long
# it took.
#
#   PKG_SIGNKEY=<key> sh tools/contrib/publish-nightly.sh <channel> <archive> <top dir> <table> <build> [<url>]
#
# The archive must already be in <channel>/archives/. With ABOUT=<dir>, the
# output of tools/contrib/about.py, each package gets its catalogue fields. With <url>, where the
# archive is published (the nightly's SourceForge download), each manifest
# records it with the archive's size and SHA-256: installs download it from
# there, and PUSH leaves the archive off the portal.

set -u
ch=$1 archive=$2 top=$3 table=$4 build=$5 url=${6:-}
pkg=${PKG:-./build/pkg}
name=$(basename "$archive")
[ -f "$ch/archives/$name" ] || { echo "publish-nightly: put $name in $ch/archives/ first" >&2; exit 20; }
log=$(mktemp "${TMPDIR:-/tmp}/publish-nightly.XXXXXX")
trap 'rm -f "$log"' EXIT
start=$(date +%s)
published=0 unchanged=0 refused=0 files=0 bytes=0
# a table edited by hand: CRLF, trailing spaces and blank lines are fine
tr -d '\r' < "$table" | sed 's/[[:space:]]*$//' | grep -v '^#' | grep . | while read -r pname kind paths; do
    # the catalogue fields of tools/contrib/about.py, when ABOUT names its output
    set --
    if [ -n "${ABOUT:-}" ] && [ -f "$ABOUT/$pname.args" ]; then
        while IFS= read -r w; do set -- "$@" "$w"; done < "$ABOUT/$pname.args"
    fi
    out=$("$pkg" PUBLISH "$ch/archives/$name!/$top" FILES "$paths" CHANNEL "$ch" NAME "$pname" \
          KIND "$kind" BUILD "$build" ${url:+UPSTREAM "$url"} "$@" MACHINE 2>&1)
    rc=$?
    result=$(printf '%s\n' "$out" | awk -F': ' '$1=="result"{print $2}')
    version=$(printf '%s\n' "$out" | awk -F': ' '$1=="version"{print $2; exit}')
    n=$(printf '%s\n' "$out" | awk -F': ' '$1=="files"{print $2}')
    case $result in
        published) echo "published $pname $version ($n files)" ;;
        unchanged) echo "unchanged $pname $version" ;;
        *)         echo "REFUSED   $pname: $(printf '%s\n' "$out" | awk -F': ' '$1=="reason"{print $2}') (exit $rc)" ;;
    esac
done | tee "$log"
end=$(date +%s)
p=$(grep -c '^published' "$log"); u=$(grep -c '^unchanged' "$log"); r=$(grep -c '^REFUSED' "$log")
total=$((p + u + r))
idx="$ch/archives/$name.pkgidx"
echo
echo "== nightly $build, $(basename "$archive")"
echo "packages:          $total in the table: $p published, $u unchanged (no new version), $r refused"
if [ -f "$idx" ]; then
    awk 'NR>1{n++; s+=$2} END{printf "archive:           %d files, %.1f MB unpacked\n", n, s/1e6}' "$idx"
fi
echo "archive reads:     1 for the index, instead of $total (one per package)"
echo "time:              $((end - start)) s"
[ "$r" -eq 0 ]
