#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# When the portal cannot answer. A machine is told in a record and a person
# sees the error page with a request; both must leave the same trace for the
# maintainers, with nothing about who asked. Without it, "Something went
# wrong" on a page is all anyone ever learns.
#
# Needs: make; the portal built (dotnet build portal/src/Portal -c Release).

set -u
repo_root=$(cd "$(dirname "$0")/.." && pwd)
P=${PKG:-$repo_root/build/pkg}
dll="$repo_root/portal/src/Portal/bin/Release/net10.0/Portal.dll"
export PATH="$HOME/.dotnet:$PATH"
command -v dotnet > /dev/null && [ -f "$dll" ] || { echo "failures: the portal is not built; skipped" >&2; exit 0; }
T=$(mktemp -d); port=5085; site="http://127.0.0.1:$port"
trap 'kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
unset PKG_PUSHKEY PKG_SIGNKEY; export PKG_CACHE="$T/cache"

# The portal is started so that one address fails on purpose, from this
# machine only; nothing else here can be made to fail on demand.
( cd "$repo_root/portal/src/Portal" && Portal__DataDir="$T/data" Portal__PkgPath="$P" \
  PORTAL_TEST_FAILURE=1 ASPNETCORE_ENVIRONMENT=Production ASPNETCORE_URLS="$site" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
srv=$!
for i in $(seq 1 40); do curl -fs "$site/health" > /dev/null && break; sleep 1; done
curl -fs "$site/health" > "$T/health";                  ok $? "the portal answers"

# Health must exercise the path a client uses. A portal that counts channel
# directories says ok while every read of them fails, which is how a real
# outage went unseen for two minutes.
mkdir -p "$T/data/channels/tools"
printf 'tool 1.0 generic %064d\n' 1 > "$T/data/channels/tools/index"
curl -fs "$site/health" > "$T/health.2"
grep -q '^ok: 1 channels, read tools/index' "$T/health.2"
                                                        ok $? "health says which channel file it read: $(cat "$T/health.2" 2>/dev/null | tr -d '\n')"
chmod 000 "$T/data/channels/tools/index"
code=$(curl -s -o "$T/health.3" -w '%{http_code}' "$site/health")
chmod 644 "$T/data/channels/tools/index"
[ "$code" = "503" ] && grep -q '^failing:' "$T/health.3"
                                                        ok $? "and says failing (503) when that file cannot be read"

code=$(curl -s -o "$T/page" -w '%{http_code}' "$site/_test/fail")
[ "$code" = "500" ];                                    ok $? "a page that fails answers 500 ($code)"
grep -q 'Something went wrong' "$T/page";               ok $? "and shows the error page, not a stack trace"
grep -qi 'at Portal\.\|\.cs:line' "$T/page" && r=1 || r=0
[ "$r" -eq 0 ];                                         ok $? "nothing of the inside of the portal is shown"
req=$(sed -n 's/.*Request <code>\([^<]*\)<\/code>.*/\1/p' "$T/page" | head -1)
[ -n "$req" ];                                          ok $? "the page names the request: ${req:-none}"

# The same failure, kept for the maintainers.
[ -f "$T/data/state/failures" ];                        ok $? "the failure is kept"
grep -q "/_test/fail" "$T/data/state/failures";          ok $? "with what was asked"
grep -qF "$req" "$T/data/state/failures";               ok $? "under the request the person was shown"
grep -q "Exception" "$T/data/state/failures";           ok $? "and what went wrong"
# Nothing about who asked: no address, no user agent, no cookie.
grep -qi '127\.0\.0\.1\|curl/\|Mozilla\|Cookie' "$T/data/state/failures" && r=1 || r=0
[ "$r" -eq 0 ];                                         ok $? "and nothing about who asked"

# A machine asking is answered in its own form, and that is kept too.
before=$(wc -l < "$T/data/state/failures")
curl -s "$site/api/../_test/fail" -o /dev/null > /dev/null 2>&1
code=$(curl -s -H 'Accept: text/plain' -o "$T/rec" -w '%{http_code}' "$site/_test/fail")
[ "$code" = "500" ];                                    ok $? "a machine asking the same fails the same way"
[ "$(wc -l < "$T/data/state/failures")" -gt "$before" ]
                                                        ok $? "and that failure is kept as well"

echo "failures: $((checks - fails))/$checks checks passed"
[ "$fails" -eq 0 ]
