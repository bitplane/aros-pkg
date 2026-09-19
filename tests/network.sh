#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Channels over HTTP. A small server on this machine serves a directory
# channel file for file, the layout the portal serves; Pkg reads it by URL:
# SHOW, INSTALL, UPGRADE, STATUS, an archive source, one downloaded from
# where its makers publish it, a redirect, a chunked reply; and refuses what it must: an unreachable host, a path with no
# channel, a file changed on the server, a PUBLISH into a URL.

set -u
PKG=${PKG:-./build/pkg}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-network.XXXXXX")
server=
trap '[ -z "$server" ] || { kill "$server" 2>/dev/null; wait "$server" 2>/dev/null; }; rm -rf "$T"' EXIT
cd "$T" || exit 1
export COPYFILE_DISABLE=1 PKG_CACHE="$T/cache"
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

cat > server.py <<'PY'
import http.server, os, sys
root = sys.argv[1]
log = open(sys.argv[2], "a")
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=root, **k)
    def log_message(self, *a):
        pass
    def do_GET(self):
        log.write(self.path + "\n"); log.flush()
        if self.path.startswith("/moved/"):
            self.send_response(302); self.send_header("Location", "/ch/" + self.path[7:]); self.end_headers(); return
        if self.path.startswith("/chunked/"):
            p = os.path.join(root, "ch", self.path[9:])
            if not os.path.isfile(p):
                self.send_error(404); return
            data = open(p, "rb").read()
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for i in range(0, len(data), 1000):
                part = data[i:i + 1000]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n"); return
        return super().do_GET()
http.server.ThreadingHTTPServer.allow_reuse_address = True
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(sys.argv[3], "w").write(str(s.server_address[1]))
s.serve_forever()
PY
mkdir -p srv
python3 server.py "$T/srv" "$T/requests" "$T/port" &
server=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s port ] && break; sleep 0.3; done
U="http://127.0.0.1:$(cat port)"

$PKG KEYGEN FILE key > /dev/null
export PKG_SIGNKEY="$T/key"
mkdir -p d1/C d2/C lib/Libs
printf 'x\000$VER: hello 1.0 (1.1.2026)\000' > d1/C/Hello
printf 'x\000$VER: hello 1.1 (1.1.2026)\000' > d2/C/Hello
printf 'lib' > lib/Libs/helper.library
$PKG PUBLISH lib CHANNEL srv/ch NAME helper VERSION 1 KIND library > /dev/null
$PKG PUBLISH d1 CHANNEL srv/ch KIND application DEPENDS helper > /dev/null

echo "read"
$PKG SHOW CHANNEL "$U/ch" MACHINE > o1 2>&1
[ $? -eq 0 ] && has o1 '^entry: hello 1.0 ' && has o1 '^bad: 0$';  ok $? "SHOW reads a channel by URL, every entry checked"
$PKG INSTALL hello ROOT r CHANNEL "$U/ch" MACHINE > o2 2>&1
[ $? -eq 0 ] && has o2 '^dependency: helper 1' && [ -f r/C/Hello ] && [ -f r/Libs/helper.library ]
                                                      ok $? "INSTALL over HTTP, with its dependency"
$PKG PUBLISH d2 CHANNEL srv/ch KIND application DEPENDS helper > /dev/null
$PKG STATUS ROOT r CHANNEL "$U/ch" MACHINE > o3 2>&1
has o3 '^package: hello 1.0 1.1 upgradable$';         ok $? "STATUS sees the new version on the server: the index is fetched again"
: > requests
$PKG UPGRADE hello ROOT r CHANNEL "$U/ch" MACHINE > o4 2>&1
[ $? -eq 0 ] && has o4 '^version: 1.1$';              ok $? "UPGRADE over HTTP"
hd=$(awk '$1=="helper"{print $4}' srv/ch/index)
grep -q '/ch/index$' requests && ! grep -q "$hd.manifest" requests
                                                      ok $? "the index is fetched again, what the cache holds by digest is not"

echo "redirect_and_chunked"
$PKG SHOW CHANNEL "$U/moved" MACHINE > o5 2>&1
[ $? -eq 0 ] && has o5 '^entry: hello 1.1 ';          ok $? "a channel behind a redirect is read"
$PKG INSTALL hello ROOT r2 CHANNEL "$U/chunked" MACHINE > o6 2>&1
[ $? -eq 0 ] && cmp -s r2/C/Hello d2/C/Hello;         ok $? "a server that answers in chunks is read byte for byte"

echo "archive"
mkdir -p arc/Top/Extras/Tool
printf 'x\000$VER: tool 3.0 (1.1.2026)\000' > arc/Top/Extras/Tool/Tool
(cd arc && tar -cjf ../night.tar.bz2 Top)
mkdir -p srv/ch/archives && cp night.tar.bz2 srv/ch/archives/
$PKG PUBLISH "srv/ch/archives/night.tar.bz2!/Top" FILES Extras/Tool CHANNEL srv/ch NAME tool BUILD 20260919 KIND application > /dev/null
$PKG INSTALL tool ROOT r3 CHANNEL "$U/ch" MACHINE > o7 2>&1
[ $? -eq 0 ] && cmp -s r3/Extras/Tool/Tool arc/Top/Extras/Tool/Tool;  ok $? "a package whose files stay in an archive installs over HTTP"

echo "upstream"
# The archive stays where its makers publish it (here /sf/, as on SourceForge);
# the channel holds only signed manifests saying where it is and what it is.
mkdir -p up/Top/Extras/Up1 up/Top/Extras/Up2 srv/sf lc/archives
printf 'x\000$VER: upone 1.0 (1.1.2026)\000' > up/Top/Extras/Up1/Up1
printf 'x\000$VER: uptwo 2.0 (1.1.2026)\000' > up/Top/Extras/Up2/Up2
(cd up && tar -cjf ../lc/archives/up.tar.bz2 Top)
cp lc/archives/up.tar.bz2 srv/sf/up.tar.bz2
$PKG PUBLISH "lc/archives/up.tar.bz2!/Top" FILES Extras/Up1 CHANNEL lc NAME upone BUILD 20260919 KIND application UPSTREAM "$U/sf/up.tar.bz2" MACHINE > o20 2>&1
[ $? -eq 0 ] && has o20 "^upstream: $U/sf/up.tar.bz2\$" && grep -q "^Archive: $(shasum -a 256 lc/archives/up.tar.bz2 | cut -d' ' -f1) $(wc -c < lc/archives/up.tar.bz2 | tr -d ' ') $U/sf/up.tar.bz2\$" lc/objects/*.manifest
                                                      ok $? "UPSTREAM records the archive's URL, size and SHA-256 in the signed manifest"
$PKG PUBLISH "lc/archives/up.tar.bz2!/Top" FILES Extras/Up2 CHANNEL lc NAME uptwo BUILD 20260919 KIND application UPSTREAM "$U/sf/up.tar.bz2" > /dev/null 2>&1
[ -s lc/archives/up.tar.bz2.sha256 ];                 ok $? "the archive's digest is kept beside it, read once for every package"
mkdir -p srv/upch && cp lc/index srv/upch/ && cp -R lc/objects srv/upch/
: > requests
$PKG INSTALL upone ROOT r6 CHANNEL "$U/upch" MACHINE > o21 2>&1
[ $? -eq 0 ] && cmp -s r6/Extras/Up1/Up1 up/Top/Extras/Up1/Up1 && grep -q '^/sf/up.tar.bz2$' requests && ! grep -q '/upch/archives' requests
                                                      ok $? "a channel with no archive installs from the upstream URL"
: > requests
$PKG INSTALL uptwo ROOT r6 CHANNEL "$U/upch" MACHINE > o22 2>&1
[ $? -eq 0 ] && [ -f r6/Extras/Up2/Up2 ] && ! grep -q '/sf/' requests
                                                      ok $? "the second package of that archive downloads nothing more"
$PKG SHOW CHANNEL "$U/upch" MACHINE > o23 2>&1
[ $? -eq 0 ] && has o23 '^bad: 0$';                   ok $? "SHOW checks the files against the downloaded copy"
PKG_CACHE="$T/cache2" $PKG SHOW CHANNEL "$U/upch" MACHINE > o24 2>&1
[ $? -eq 0 ] && has o24 "^archive: upone 1.0+20260919 upstream $U/sf/up.tar.bz2\$" && has o24 '^bad: 0$'
                                                      ok $? "with no copy, SHOW names where the archive is and counts nothing bad"
mkdir -p evil/Top/Extras/Up1; printf 'evil' > evil/Top/Extras/Up1/Up1
(cd evil && tar -cjf ../srv/sf/up.tar.bz2 Top)
PKG_CACHE="$T/cache3" $PKG INSTALL upone ROOT r7 CHANNEL "$U/upch" MACHINE > o25 2>&1
[ $? -eq 12 ] && has o25 'is not the one' && [ ! -e r7/Extras/Up1/Up1 ] && [ ! -e "$T/cache3/upstream/"*/up.tar.bz2 ]
                                                      ok $? "an archive changed upstream is refused with 12, deleted, nothing installed"
PKG_CACHE="$T/cache4" $PKG INSTALL upone ROOT r8 CHANNEL lc MACHINE > o26 2>&1
[ $? -eq 0 ] && cmp -s r8/Extras/Up1/Up1 up/Top/Extras/Up1/Up1 && [ ! -d "$T/cache4/upstream" ]
                                                      ok $? "the publisher's own channel reads its local copy, downloading nothing"
$PKG PUBLISH d1 CHANNEL lc2 NAME x KIND application UPSTREAM "$U/sf/up.tar.bz2" MACHINE > o27 2>&1
[ $? -eq 20 ] && has o27 'a drawer has no archive';   ok $? "UPSTREAM on a drawer is refused, saying why"

echo "refusals"
$PKG SHOW CHANNEL "http://127.0.0.1:1/ch" MACHINE > o8 2>&1
[ $? -eq 17 ] && has o8 'cannot reach the channel';  ok $? "an unreachable host is refused with 17, saying so"
$PKG SHOW CHANNEL "$U/nothing" MACHINE > o9 2>&1
[ $? -eq 11 ] && has o9 'there is no channel at';     ok $? "a URL with no channel behind it is refused with 11"
m=$(ls srv/ch/objects/*.manifest | head -1)
rm -rf cache
$PKG INSTALL hello ROOT r4 CHANNEL "$U/ch" MACHINE > /dev/null 2>&1
d=$(awk '$1=="hello" && $2=="1.1"{print $4}' srv/ch/index)
printf 'Kind: data\n' >> "srv/ch/objects/$d.manifest"
rm -rf cache
$PKG INSTALL hello ROOT r5 CHANNEL "$U/ch" MACHINE > o10 2>&1
[ $? -eq 12 ] && [ ! -e r5/C/Hello ];                  ok $? "a manifest changed on the server is refused, nothing placed"
$PKG PUBLISH d1 CHANNEL "$U/ch" KIND application MACHINE > o11 2>&1
[ $? -eq 20 ] && has o11 'then PUSH it';              ok $? "PUBLISH into a URL is refused, pointing at PUSH"

echo
echo "network: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
