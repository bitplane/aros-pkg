#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The portal's maintainers' API, from a shell. Answers are Pkg records.
#
#   PKG_ADMINKEY=<key> sh portal/tools/admin.sh <portal> remove <channel> [--dry-run] '<name> *' ['<name> <version> <arch>' ...]
#   PKG_ADMINKEY=<key> sh portal/tools/admin.sh <portal> restore <stash>
#   PKG_ADMINKEY=<key> sh portal/tools/admin.sh <portal> log
#
# A removal takes versions off a channel; their files go to a stash on the
# server, and restore puts them back. Exit 0 unless the answer is a refusal.

set -eu
portal=${1:?portal address, e.g. https://aros-pkg.azurewebsites.net}
portal=${portal%/}
verb=${2:?remove, restore or log}
key=${PKG_ADMINKEY:?set PKG_ADMINKEY to a maintainer key}
call() { curl -sS -H "Authorization: Bearer $key" "$@"; }
case $verb in
    remove)
        channel=${3:?channel}; shift 3
        q=""; [ "${1:-}" = --dry-run ] && { q="?dryrun=1"; shift; }
        [ $# -gt 0 ] || { echo "admin: name what to remove, e.g. 'pkg *'" >&2; exit 20; }
        answer=$(printf '%s\n' "$@" | call -X POST --data-binary @- "$portal/_admin/channels/$channel/remove$q") ;;
    restore) answer=$(call -X POST "$portal/_admin/restore/${3:?stash}") ;;
    log)     answer=$(call "$portal/_admin/log") ;;
    *) echo "admin: remove, restore or log" >&2; exit 20 ;;
esac
printf '%s\n' "$answer"
! printf '%s\n' "$answer" | grep -q '^result: refused'
