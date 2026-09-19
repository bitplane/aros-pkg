#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Eyes on the portal: screenshots of its pages, as a browser renders them.
#
#   sh portal/tools/shot.sh /publishers /account            the local portal, signed out
#   sh portal/tools/shot.sh --as jane /account /publishers  signed in as a test account
#   sh portal/tools/shot.sh --as jonx --admin /admin        ... who is a maintainer
#   sh portal/tools/shot.sh --live / /downloads             the live site (signed out only)
#   --dark, --phone (390 px wide) change how it is looked at
#
# Local runs start the portal in Development on sample data (two publishers, a
# listed and an unlisted channel, a registered and a suspended account) and stop
# it afterwards. Each page becomes portal/captures/<time>_<page>.png.
set -u
here=$(cd "$(dirname "$0")/.." && pwd); repo=$(cd "$here/.." && pwd)
chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
[ -x "$chrome" ] || chrome=$(command -v chromium || command -v google-chrome || true)
[ -n "$chrome" ] || { echo "shot: no Chrome or Chromium found" >&2; exit 69; }
export PATH="$HOME/.dotnet:$PATH"
live=; as=; admin=; dark=; size=1280,1800; pages=()
while [ $# -gt 0 ]; do case "$1" in
    --live) live=https://aros-pkg.azurewebsites.net ;; --as) shift; as=$1 ;; --admin) admin=1 ;;
    --dark) dark=--force-dark-mode ;; --phone) size=390,1600 ;; *) pages+=("$1") ;; esac; shift; done
[ ${#pages[@]} -gt 0 ] || pages=(/)
out="$here/captures"; mkdir -p "$out"; stamp=$(date +%Y%m%dT%H%M%S)
T=$(mktemp -d); srv=
trap '[ -z "$srv" ] || kill $srv 2>/dev/null; rm -rf "$T"' EXIT

if [ -n "$live" ]; then base=$live
else
    base=http://127.0.0.1:5090; P="$repo/build/pkg"
    [ -x "$P" ] || { echo "shot: build Pkg first (make)" >&2; exit 69; }
    dll="$here/src/Portal/bin/Release/net10.0/Portal.dll"
    ( cd "$here" && dotnet build src/Portal -c Release -v q 2>&1 | grep -E ' error ' ) && exit 1
    # sample data: a project channel, a publisher's unlisted channel, accounts
    "$P" KEYGEN FILE "$T/a.key" > /dev/null; "$P" KEYGEN FILE "$T/b.key" > /dev/null
    ka=$("$P" KEYINFO FILE "$T/a.key" MACHINE | awk '/^public:/{print $2}'); kb=$("$P" KEYINFO FILE "$T/b.key" MACHINE | awk '/^public:/{print $2}')
    mkdir -p "$T/d/C"; printf 'x\000$VER: Hello 1.0 (20.9.2026)\000' > "$T/d/C/Hello"
    "$P" PUBLISH "$T/d" CHANNEL "$T/data/channels/demo" KIND data ARCH generic SIGN "$T/a.key" SHORT "A sample package" CATEGORY util/misc > /dev/null 2>&1
    "$P" PUBLISH "$T/d" CHANNEL "$T/data/channels/janes-tools" KIND data ARCH generic SIGN "$T/b.key" SHORT "Jane's sample" > /dev/null 2>&1
    mkdir -p "$T/data/state/janes-tools"; echo sample > "$T/data/state/janes-tools/unlisted"
    cat > "$T/data/state/publishers.json" <<JSON
[{"GitHubId":2,"Login":"jane","Name":"Jane Roe","Key":"$kb","Channels":["janes-tools"],"Files":false,"Suspended":false,"Since":"2026-09-20T00:00:00Z"},
 {"GitHubId":3,"Login":"mallory","Name":"Mallory","Key":"$(printf '3%.0s' $(seq 1 64))","Channels":["mal"],"Files":false,"Suspended":true,"Since":"2026-09-20T00:00:00Z"}]
JSON
    ( cd "$here/src/Portal" && ASPNETCORE_ENVIRONMENT=Development Portal__DataDir="$T/data" Portal__PkgPath="$P" Portal__Pinned=demo/hello \
      Portal__GitHub__ClientId=dev Portal__GitHub__ClientSecret=dev Portal__Admins="${admin:+$as}" \
      Portal__SignedKeys="owner:$ka:*:files" ASPNETCORE_URLS="$base" exec dotnet "$dll" > "$T/portal.log" 2>&1 ) &
    srv=$!
    for i in $(seq 1 40); do curl -fs "$base/health" > /dev/null && break; sleep 1; done
    curl -fs "$base/health" > /dev/null || { echo "shot: the portal did not start"; tail -5 "$T/portal.log"; exit 1; }
fi
prof="$T/profile"
# Chrome does not always exit after a headless capture: it gets twenty seconds.
look() {
    "$chrome" --headless=new --disable-gpu --hide-scrollbars --no-first-run --no-default-browser-check \
        --user-data-dir="$prof" $dark --window-size=$size --timeout=8000 "$@" > /dev/null 2>&1 &
    local c=$! w=0
    while kill -0 $c 2>/dev/null && [ $w -lt 40 ]; do sleep 0.5; w=$((w + 1)); done
    kill $c 2>/dev/null; wait $c 2>/dev/null
}
q=
if [ -n "$as" ] && [ -z "$live" ]; then
    case "$as" in jane) id=2 ;; mallory) id=3 ;; *) id=1 ;; esac
    q="dev-as=$as&dev-id=$id"
fi
for p in "${pages[@]}"; do
    name=$(printf '%s' "${p#/}" | tr -c 'A-Za-z0-9\n' '-'); f="$out/${stamp}_${as:+$as-}${name:-home}${dark:+-dark}.png"
    case "$p" in *\?*) u="$base$p${q:+&$q}" ;; *) u="$base$p${q:+?$q}" ;; esac
    look --screenshot="$f" "$u"; [ -s "$f" ] && echo "$f" || echo "shot: nothing captured for $p" >&2
done
