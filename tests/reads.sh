#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Reading a channel under load. The machine serving the site is a small one,
# so an address may have only a few files in flight at once; the rest wait
# their turn and are served. A reader taking a whole channel file by file, as
# pkg does, must never be refused for going too fast.
#
# Needs: make; the portal built (dotnet build portal/src/Portal -c Release).

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
dll="$repo_root/portal/src/Portal/bin/Release/net10.0/Portal.dll"
export PATH="$HOME/.dotnet:$PATH"
command -v dotnet > /dev/null && [ -f "$dll" ] || { echo "reads: the portal is not built; skipped" >&2; exit 0; }
T=$(mktemp -d); port=5084; site="http://127.0.0.1:$port"
trap 'kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
unset PKG_PUSHKEY PKG_SIGNKEY; export PKG_CACHE="$T/cache"

"$P" KEYGEN FILE "$T/pub.key" > /dev/null
pub=$("$P" KEYINFO FILE "$T/pub.key" MACHINE | awk '/^public:/{print $2}')
mkdir -p "$T/d/C"
for v in $(seq 1 12); do
    printf 'x\000$VER: tool 1.%s (19.9.2026)\000' "$v" > "$T/d/C/tool"
    "$P" PUBLISH "$T/d" CHANNEL "$T/ch" KIND data ARCH generic SIGN "$T/pub.key" > /dev/null 2>&1
done
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" \
  Portal__SignedKeys="tester:$pub:tools:files" ASPNETCORE_URLS="$site" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done
"$P" PUSH CHANNEL "$T/ch" TO "$site/tools" SIGN "$T/pub.key" > /dev/null 2>&1
                                                        ok $? "a channel of twelve versions is published"

# One reader, file after file: what pkg does, and what a count per minute would
# have refused first.
n=0; bad=0
for d in $(awk '{print $4}' "$T/ch/index"); do
    for ext in manifest sig; do
        n=$((n + 1))
        curl -fs -o /dev/null "$site/tools/objects/$d.$ext" || bad=$((bad + 1))
    done
done
[ "$bad" -eq 0 ];                                       ok $? "$n requests one after another, none refused"

# Many at once from one address: more than the site serves at a time, so most
# of them wait. They are still answered.
pids=""
for i in $(seq 1 60); do
    ( curl -s -o /dev/null -w '%{http_code}\n' "$site/tools/index" >> "$T/codes" ) &
    pids="$pids $!"
done
# Only these; the portal itself is a background job of this shell as well.
for p in $pids; do wait "$p"; done
[ "$(wc -l < "$T/codes" | tr -d ' ')" = "60" ] && ! grep -qv '^200$' "$T/codes"
                                                        ok $? "60 reads at once are all answered: $(sort "$T/codes" | uniq -c | tr -d '\n' | tr -s ' ')"

# Where a count does apply (the admin API: sixty a minute), the refusal says
# when to come back, in the header a machine reads and in the record.
code=""; i=0
while [ "$code" != "429" ] && [ $i -lt 80 ]; do
    i=$((i + 1))
    code=$(curl -s -D "$T/h" -o "$T/b" -w '%{http_code}' "$site/_admin/failures")
done
[ "$code" = "429" ];                                    ok $? "the admin API refuses the 61st request in a minute (429 after $i)"
grep -qi '^retry-after: 60' "$T/h";                     ok $? "the refusal says when to come back: Retry-After"
grep -q '^result: refused' "$T/b" && grep -q '^next: ' "$T/b"
                                                        ok $? "and says it as a record pkg can read"

echo "reads: $((checks - fails))/$checks checks passed"
[ "$fails" -eq 0 ]
