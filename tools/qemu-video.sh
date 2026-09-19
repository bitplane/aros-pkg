#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The video of a QEMU run recorded by tools/qemu-frames.py:
#
#     qemu-video.sh <frames dir> <out.mp4> [max seconds a still frame is held, default 3]
#
# Every change on screen is kept; a screen that stayed still longer than the
# limit (the emulated CPU working behind it) is held for the limit only, so
# an hour of emulation plays in minutes and nothing that moved is skipped.
# Frames of different sizes (the BIOS text screen, AROS's graphics) are
# scaled into one 1280x800 picture. Needs ffmpeg.

set -eu
[ $# -ge 2 ] || { echo "usage: $0 <frames dir> <out.mp4> [max hold seconds]" >&2; exit 20; }
dir=$1 out=$2 hold=${3:-3}
command -v ffmpeg > /dev/null || { echo "qemu-video: ffmpeg not found (brew install ffmpeg)" >&2; exit 69; }
[ -s "$dir/frames.txt" ] || { echo "qemu-video: no frames.txt in $dir" >&2; exit 11; }
list=$(mktemp "${TMPDIR:-/tmp}/qemu-video.XXXXXX")
trap 'rm -f "$list"' EXIT
awk -v hold="$hold" -v dir="$dir" '
    /^file / { f = $0; sub(/^file '\''/, "", f); sub(/'\''$/, "", f); print "file '\''" dir "/" f "'\''"; last = f; next }
    /^duration / { d = $2 + 0; if (d > hold) d = hold; if (d < 0.04) d = 0.04; print "duration " d }
    END { if (last != "") print "file '\''" dir "/" last "'\''" }' "$dir/frames.txt" > "$list"
ffmpeg -loglevel error -y -f concat -safe 0 -i "$list" \
    -vf "scale=1280:800:force_original_aspect_ratio=decrease,pad=1280:800:(ow-iw)/2:(oh-ih)/2,fps=25" \
    -c:v libx264 -pix_fmt yuv420p -movflags +faststart "$out"
echo "qemu-video: $out, $(awk '/^duration/{s+=$2} END{printf "%.0f", s}' "$list") s from $(grep -c '^file' "$dir/frames.txt") frames"
