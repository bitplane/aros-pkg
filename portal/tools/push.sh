#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Push a local channel directory to the portal, with the protocol agreed for
# Pkg's own PUSH verb (plan, files in parts of 32 MiB, commit). For manual
# publishing until `pkg PUSH` exists, and as its reference.
#
#   PKG_PUSHKEY=<key> sh portal/tools/push.sh <local channel dir> <channel url>
#
# e.g. sh portal/tools/push.sh ~/pkg-channel https://aros-pkg.azurewebsites.net/pkg
# The answers are Pkg records (key: value). Exit 0 when nothing was refused.

set -u
dir=${1:?local channel directory}
url=${2:?channel url, e.g. https://host/contrib-nightly}
url=${url%/}
key=${PKG_PUSHKEY:?set PKG_PUSHKEY to your push key}
part=$((32 * 1024 * 1024))
[ -f "$dir/index" ] || { echo "push: $dir has no index, so it is not a channel" >&2; exit 20; }
work=$(mktemp -d "${TMPDIR:-/tmp}/pkg-push.XXXXXX")
trap 'rm -rf "$work"' EXIT

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
size() { wc -c < "$1" | tr -d ' '; }
call() { # method path [curl args...]: the answer on stdout, the HTTP status in $work/status
    m=$1 p=$2; shift 2
    curl -sS -X "$m" -H "Authorization: Bearer $key" -w '%{http_code}' -o "$work/answer" "$@" "$url/_push/$p" > "$work/status"
    cat "$work/answer"
}
field() { awk -F': ' -v k="$1" '$1==k{print substr($0, length(k)+3)}' "$work/answer" | tail -1; }

# 1. The plan: every file the channel holds, with its digest and size.
( cd "$dir" && find objects archives Bootstrap Install-Pkg ReadMe -type f 2>/dev/null ) | sort |
    grep -v -e '\.pkgidx$' -e '\.sha256$' -e '\.url$' -e '\.part$' |
    while read -r rel; do printf '%s %s %s\n' "$rel" "$(sha "$dir/$rel")" "$(size "$dir/$rel")"; done > "$work/plan"
echo "== plan: $(wc -l < "$work/plan" | tr -d ' ') files"
call POST plan --data-binary @"$work/plan" -H 'Content-Type: text/plain' | grep -v '^need: '
[ "$(cat "$work/status")" = 200 ] || exit 1
grep '^need: ' "$work/answer" | sed 's/^need: //' > "$work/need"

# 2. The files the portal lacks; large ones in parts, resuming where it stopped.
while read -r rel; do
    f="$dir/$rel" total=$(size "$dir/$rel")
    if [ "$total" -le "$part" ]; then
        call PUT "files/$rel" --data-binary @"$f" -H 'Content-Type: application/octet-stream' > /dev/null
        echo "sent $rel: $(field summary)"
        continue
    fi
    from=0
    while [ "$from" -lt "$total" ]; do
        to=$((from + part - 1)); [ "$to" -ge "$total" ] && to=$((total - 1))
        dd if="$f" of="$work/part" bs=1048576 skip=$((from / 1048576)) count=$((part / 1048576)) 2>/dev/null
        head -c $((to - from + 1)) "$work/part" > "$work/part.cut"
        call PUT "files/$rel" --data-binary @"$work/part.cut" -H "Content-Range: bytes $from-$to/$total" \
             -H 'Content-Type: application/octet-stream' > /dev/null
        case $(field result) in
            partial)  from=$(field received); printf '\r%s: %s' "$rel" "$(field summary)" ;;
            received|unchanged) echo; echo "sent $rel: $(field summary)"; break ;;
            *) echo; echo "push: $rel: $(field reason)" >&2; exit 1 ;;
        esac
    done
done < "$work/need"

# 3. The commit: the local index; the portal publishes what Pkg accepts.
echo "== commit"
call POST commit --data-binary @"$dir/index" -H 'Content-Type: text/plain'
[ "$(field result)" != refused ] && ! grep -q '^refused: ' "$work/answer"
