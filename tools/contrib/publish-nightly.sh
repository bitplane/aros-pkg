#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Publish a nightly contrib archive as packages. A port that carries a
# .pkginfo (tools/contrib/PKGINFO.md) is published from it, and the paths it
# claims are left out of the rest; everything else comes from a line of a
# split table (`<name> <kind> <path>[,<path>...]`, # for comments), taking
# its version from its own $VER plus the nightly's date. The archive is read
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
info=$(mktemp -d "${TMPDIR:-/tmp}/publish-nightly-info.XXXXXX")
trap 'rm -f "$log"; rm -rf "$info"' EXIT
start=$(date +%s)
published=0 unchanged=0 refused=0 files=0 bytes=0

# The .pkginfo files the archive carries, extracted in one pass: reading each
# one out of the compressed archive on its own would unpack it again.
tar -tf "$ch/archives/$name" 2>/dev/null | grep '\.pkginfo$' > "$info/list" || true
: > "$info/claimed"
if [ -s "$info/list" ]; then
    tar -xf "$ch/archives/$name" -C "$info" -T "$info/list" 2>/dev/null \
        || { echo "publish-nightly: cannot extract the .pkginfo files from $name" >&2; exit 17; }
    while IFS= read -r pi; do
        tr -d '\r' < "$info/$pi" | sed -n 's/^Files:[[:space:]]*//p' >> "$info/claimed"
    done < "$info/list"
fi

# What one PUBLISH answered, as a line of the log.
report() {
    result=$(printf '%s\n' "$2" | awk -F': ' '$1=="result"{print $2}')
    version=$(printf '%s\n' "$2" | awk -F': ' '$1=="version"{print $2; exit}')
    pubname=$(printf '%s\n' "$2" | awk -F': ' '$1=="name"{print $2; exit}')
    n=$(printf '%s\n' "$2" | awk -F': ' '$1=="files"{print $2}')
    [ -n "$pubname" ] || pubname=$1
    case $result in
        published) echo "published $pubname $version ($n files)" ;;
        unchanged) echo "unchanged $pubname $version" ;;
        *)         echo "REFUSED   $pubname: $(printf '%s\n' "$2" | awk -F': ' '$1=="reason"{print $2}') (exit $3)" ;;
    esac
}

# The paths of a table line that no .pkginfo has claimed, so that nothing is
# published twice.
unclaimed() {
    printf '%s\n' "$1" | tr ',' '\n' | while IFS= read -r one; do
        [ -n "$one" ] || continue
        skip=no
        while IFS= read -r c; do
            [ -n "$c" ] || continue
            case $one in "$c"|"$c"/*) skip=yes ;; esac
        done < "$info/claimed"
        [ "$skip" = no ] && printf '%s,' "$one"
    done | sed 's/,$//'
}
# a table edited by hand: CRLF, trailing spaces and blank lines are fine
{
while IFS= read -r pi; do
    [ -n "$pi" ] || continue
    out=$("$pkg" PUBLISH "$ch/archives/$name!/$top" INFO "$info/$pi" CHANNEL "$ch" \
          BUILD "$build" ${url:+UPSTREAM "$url"} MACHINE 2>&1)
    report "$pi" "$out" "$?"
done < "$info/list"
tr -d '\r' < "$table" | sed 's/[[:space:]]*$//' | grep -v '^#' | grep . | while read -r pname kind paths; do
    paths=$(unclaimed "$paths")
    if [ -z "$paths" ]; then
        echo "from file  $pname: every path of it belongs to a .pkginfo"
        continue
    fi
    # the catalogue fields of tools/contrib/about.py, when ABOUT names its output
    set --
    if [ -n "${ABOUT:-}" ] && [ -f "$ABOUT/$pname.args" ]; then
        while IFS= read -r w; do set -- "$@" "$w"; done < "$ABOUT/$pname.args"
    fi
    out=$("$pkg" PUBLISH "$ch/archives/$name!/$top" FILES "$paths" CHANNEL "$ch" NAME "$pname" \
          KIND "$kind" BUILD "$build" ${url:+UPSTREAM "$url"} "$@" MACHINE 2>&1)
    report "$pname" "$out" "$?"
done
} | tee "$log"
end=$(date +%s)
p=$(grep -c '^published' "$log"); u=$(grep -c '^unchanged' "$log"); r=$(grep -c '^REFUSED' "$log")
i=$(grep -c . "$info/list" 2>/dev/null || echo 0)
total=$((p + u + r))
idx="$ch/archives/$name.pkgidx"
echo
echo "== nightly $build, $(basename "$archive")"
echo "packages:          $total: $p published, $u unchanged (no new version), $r refused"
echo "from a .pkginfo:   $i of them; the rest from the split table"
if [ -f "$idx" ]; then
    awk 'NR>1{n++; s+=$2} END{printf "archive:           %d files, %.1f MB unpacked\n", n, s/1e6}' "$idx"
fi
echo "archive reads:     1 for the index, instead of $total (one per package)"
echo "time:              $((end - start)) s"
[ "$r" -eq 0 ]
