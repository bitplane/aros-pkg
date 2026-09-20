#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# SEARCH on the host: a directory channel read package by package, and a
# portal channel answered by its search API.
#
# The local half checks every field a word may match (name, Short, Tags,
# Category, Description, Provides), that several words all have to match,
# that nothing matching is an answer and not a refusal (exit 0, count 0),
# the ARCH filter, and that a withdrawn version is not offered.
#
# The API half runs two small servers on this machine over the same
# published channel. One answers /api/search in the portal's JSON shape,
# with a Short that is deliberately not the one in the manifest: seeing that
# Short proves the API was asked and its answer used. The other answers
# /api/search with garbage, and the same search then has to give the
# manifest's own Short, which proves the fall-back reads the channel itself
# without a word of complaint. The last check is one read of the live portal,
# skipped with a message when this machine is offline.

set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-search.XXXXXX")
good=
bad=
trap 'for p in $good $bad; do { kill "$p"; wait "$p"; } 2>/dev/null; done
      [ "${KEEP:-0}" = 1 ] || rm -rf "$T"' EXIT

checks=0
fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
has() { grep -q -- "$2" "$1"; }

$PKG KEYGEN FILE "$T/k" > /dev/null
export PKG_SIGNKEY="$T/k"
C=$T/ch

elf() {  # a header with an e_machine, so a version can be published for one CPU
    python3 -c "
import sys
b = bytearray(64); b[0:4] = b'\x7fELF'; b[4] = 2; b[5] = 1; b[18] = int(sys.argv[2])
open(sys.argv[1], 'wb').write(bytes(b) + sys.argv[3].encode())
" "$1" "$2" "$3"
}

# ---- a channel to search ----------------------------------------------
printf 'Talking about hedgehogs at length.\nSecond line of it.\n' > "$T/desc"
rm -rf "$T/src"; mkdir -p "$T/src/Libs"
printf 'a\n' > "$T/src/Libs/melody.library"
$PKG PUBLISH "$T/src" CHANNEL "$C" NAME melody VERSION 1.0 KIND library \
     SHORT "Plays a tune out loud" TAGS "audio,tracker" CATEGORY mus/play \
     DESCRIPTION "$T/desc" > /dev/null 2>&1
ok $? "published melody"

rm -rf "$T/src"; mkdir -p "$T/src/C"
printf 'b\n' > "$T/src/C/Quiet"
$PKG PUBLISH "$T/src" CHANNEL "$C" NAME quiet VERSION 2.0 KIND application \
     SHORT "Does nothing at all" CATEGORY util/misc > /dev/null 2>&1
ok $? "published quiet"

rm -rf "$T/src"; mkdir -p "$T/src/Libs"
elf "$T/src/Libs/onearch.library" 62 x
$PKG PUBLISH "$T/src" CHANNEL "$C" NAME onearch VERSION 1.0 KIND library \
     SHORT "Only for one CPU" > /dev/null 2>&1
ok $? "published onearch for x86_64"

rm -rf "$T/src"; mkdir -p "$T/src/C"
printf 'c\n' > "$T/src/C/Old"
$PKG PUBLISH "$T/src" CHANNEL "$C" NAME oldthing VERSION 1.0 KIND application \
     SHORT "Withdrawn hedgehog tool" > /dev/null 2>&1
$PKG WITHDRAW oldthing VERSION 1.0 CHANNEL "$C" > /dev/null 2>&1
ok $? "withdrew oldthing 1.0"

# ---- every field -------------------------------------------------------
$PKG SEARCH melody CHANNEL "$C" MACHINE > "$T/f1" 2>&1
grep -q '^package: melody 1.0 generic Plays a tune out loud$' "$T/f1"
ok $? "a word in the name matches, and the record carries version, arch and Short"

$PKG SEARCH tune CHANNEL "$C" MACHINE > "$T/f2" 2>&1
has "$T/f2" '^package: melody' ;                 ok $? "a word in Short matches"
$PKG SEARCH tracker CHANNEL "$C" MACHINE > "$T/f3" 2>&1
has "$T/f3" '^package: melody' ;                 ok $? "a word in Tags matches"
$PKG SEARCH mus/play CHANNEL "$C" MACHINE > "$T/f4" 2>&1
has "$T/f4" '^package: melody' ;                 ok $? "a word in Category matches"
$PKG SEARCH hedgehogs CHANNEL "$C" MACHINE > "$T/f5" 2>&1
has "$T/f5" '^package: melody' ;                 ok $? "a word in Description matches"
$PKG SEARCH melody.library CHANNEL "$C" MACHINE > "$T/f6" 2>&1
has "$T/f6" '^package: melody' ;                 ok $? "a word in Provides matches"
$PKG SEARCH TUNE CHANNEL "$C" MACHINE > "$T/f7" 2>&1
has "$T/f7" '^package: melody' ;                 ok $? "matching ignores case"

# ---- several words -----------------------------------------------------
$PKG SEARCH tune audio CHANNEL "$C" MACHINE > "$T/w1" 2>&1
[ "$(awk '/^count:/{print $2}' "$T/w1")" = 1 ] ; ok $? "two words that both match give the package"
$PKG SEARCH tune nothing CHANNEL "$C" MACHINE > "$T/w2" 2>&1
[ "$(awk '/^count:/{print $2}' "$T/w2")" = 0 ] ; ok $? "every word must match: one that does not gives nothing"

# ---- nothing matched is an answer --------------------------------------
$PKG SEARCH zzzznothing CHANNEL "$C" MACHINE > "$T/n1" 2>&1
[ $? = 0 ] ;                                     ok $? "nothing matching exits 0"
[ "$(awk '/^count:/{print $2}' "$T/n1")" = 0 ] ; ok $? "nothing matching says count: 0"

# ---- withdrawn versions ------------------------------------------------
$PKG SEARCH oldthing CHANNEL "$C" MACHINE > "$T/x1" 2>&1
[ "$(awk '/^count:/{print $2}' "$T/x1")" = 0 ] ; ok $? "a withdrawn version is not offered"

# ---- ARCH --------------------------------------------------------------
$PKG SEARCH onearch CHANNEL "$C" ARCH x86_64 MACHINE > "$T/a1" 2>&1
[ "$(awk '/^count:/{print $2}' "$T/a1")" = 1 ] ; ok $? "ARCH x86_64 finds a package published for it"
$PKG SEARCH onearch CHANNEL "$C" ARCH m68k MACHINE > "$T/a2" 2>&1
[ "$(awk '/^count:/{print $2}' "$T/a2")" = 0 ] ; ok $? "ARCH m68k does not"
$PKG SEARCH quiet CHANNEL "$C" ARCH m68k MACHINE > "$T/a3" 2>&1
[ "$(awk '/^count:/{print $2}' "$T/a3")" = 1 ] ; ok $? "ARCH still gives the generic packages"

# ---- a root's channel list ---------------------------------------------
R=$T/root
mkdir -p "$R"
$PKG CHANNEL ADD "$C" ROOT "$R" > /dev/null 2>&1
$PKG SEARCH tune ROOT "$R" MACHINE > "$T/r1" 2>&1
has "$T/r1" '^package: melody' ;                 ok $? "SEARCH with no CHANNEL uses the root's list"

# ---- the portal's API, and the fall-back -------------------------------
cat > "$T/server.py" <<'PY'
import http.server, json, sys, os
root, mode, portfile = sys.argv[1], sys.argv[2], sys.argv[3]

class H(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        return os.path.join(root, path.lstrip("/").split("?")[0])
    def log_message(self, *a):
        pass
    def do_GET(self):
        if self.path.startswith("/api/search"):
            if mode == "garbage":
                body = b"<html>no api here</html>"
            else:
                body = json.dumps({
                    "query": {"q": "", "channel": "ch"},
                    "total": 1,
                    "results": [{
                        "channel": "ch", "name": "melody", "version": "1.0",
                        "kind": "library", "archs": ["generic"],
                        "short": "ANSWERED BY THE API",
                        "category": "mus/play", "tags": ["audio"],
                        "provides": [], "authors": [], "license": None,
                        "signer": "00", "updated": "2026-09-20T00:00:00Z",
                        "downloads": 0, "why": None,
                        "page": "http://x/p", "channelUrl": "http://x/ch",
                        "install": "pkg INSTALL melody", "versions": None}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        http.server.SimpleHTTPRequestHandler.do_GET(self)

http.server.ThreadingHTTPServer.allow_reuse_address = True
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(portfile, "w").write(str(s.server_address[1]))
s.serve_forever()
PY
mkdir -p "$T/srv"
cp -R "$C" "$T/srv/ch"

python3 "$T/server.py" "$T/srv" api "$T/p1" > /dev/null 2>&1 &
good=$!
python3 "$T/server.py" "$T/srv" garbage "$T/p2" > /dev/null 2>&1 &
bad=$!
i=0
while [ ! -s "$T/p1" ] || [ ! -s "$T/p2" ]; do
    i=$((i + 1))
    [ "$i" -gt 100 ] && break
    python3 -c 'import time; time.sleep(0.1)'
done
P1=$(cat "$T/p1" 2>/dev/null || echo 0)
P2=$(cat "$T/p2" 2>/dev/null || echo 0)

$PKG SEARCH melody CHANNEL "http://127.0.0.1:$P1/ch" MACHINE > "$T/api" 2>&1
ok $? "SEARCH against a server that answers /api/search"
has "$T/api" 'ANSWERED BY THE API' ;             ok $? "the API's answer is what is shown"

$PKG SEARCH melody CHANNEL "http://127.0.0.1:$P2/ch" MACHINE > "$T/fb" 2>&1
ok $? "SEARCH against a server whose /api/search is garbage"
has "$T/fb" 'Plays a tune out loud' ;            ok $? "it falls back to reading the channel itself"
grep -q 'ANSWERED BY THE API\|no api here' "$T/fb"
[ $? = 1 ] ;                                     ok $? "nothing of the garbage answer reaches the output"

# ---- one live read of the portal ---------------------------------------
LIVE=https://aros-pkg.azurewebsites.net/contrib-nightly
if curl -s -m 20 -o /dev/null "$LIVE/index" 2>/dev/null; then
    $PKG SEARCH lua CHANNEL "$LIVE" MACHINE > "$T/live" 2>&1
    ok $? "SEARCH lua against the live portal"
    has "$T/live" '^package: lua ' ;             ok $? "the live portal answers with lua"
else
    echo "  SKIP the live portal: this machine cannot reach $LIVE"
fi

echo "search: $checks checks, $fails failed"
[ "$fails" = 0 ]
