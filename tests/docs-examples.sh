#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Every command the documentation shows, run, its exit code checked:
#
#     sh tests/docs-examples.sh            check
#     sh tests/docs-examples.sh --write    also refresh the outputs in docs/*.md
#
# See tests/docs/examples.py for how an example is written.
PKG=${PKG:-./build/pkg}
here=$(dirname "$0")
# every verb, keyword and switch the command knows is in the reference
missing=
for w in $("$PKG" HELP | grep -o '\b[A-Z][A-Z]*[A-Z]\b' | sort -u) LOG UPSTREAM ORPHANS ARCHIVE; do
    case $w in AROS|FFS|ARG) continue ;; esac   # words of the prose, not of the command
    grep -q "\`$w\`" "$here/../docs/reference.md" || missing="$missing $w"
done
[ -z "$missing" ] || { echo "docs-examples: not in docs/reference.md:$missing"; exit 1; }
exec python3 "$here/docs/examples.py" "$PKG" "$@"
