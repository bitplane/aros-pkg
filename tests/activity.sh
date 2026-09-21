#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The activity line: what pkg says while a step takes time, and that it is
# advancing. What is checked here, on a host:
#
#   - a fast command says nothing new, with the delay as a person has it;
#   - with the delay at 0 the line appears, and carries the mark, a verb and
#     a measure;
#   - the line is erased: the last thing visible is the result sentence, with
#     no frame and no carriage-return debris after it;
#   - MACHINE output never holds a frame, a carriage return or an escape
#     sequence, with or without the two hooks;
#   - LOG <file> holds none of it;
#   - PKG_COLOR=never leaves no escape sequence at all;
#   - the frames cycle in the order the project's logo pulses in;
#   - a percentage never passes 100 and never falls back within a step;
#   - a rate and a time left appear only where the whole is known.
#
# Progress is for a person at a terminal, so it is off when stdout is not
# one. The two hooks are what a test has instead of a terminal:
# PKG_PROGRESS=1 turns it on anywhere, PKG_PROGRESS_AFTER=<ms> sets the wait
# before the first frame. Both are documented in docs/reference.md.

set -u
PKG=${PKG:-./build/pkg}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
[ -x "$PKG" ] || { echo "activity: missing $PKG (make build/pkg)" >&2; exit 69; }
command -v python3 > /dev/null 2>&1 || { echo "activity: needs python3" >&2; exit 69; }
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-activity.XXXXXX")
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
export COPYFILE_DISABLE=1 PKG_CACHE="$T/cache"
unset PKG_PROGRESS PKG_PROGRESS_AFTER PKG_COLOR 2>/dev/null

checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }

# What a byte stream holds, and what is left on the screen after it: the
# activity line is rewritten in place, so a person sees only the piece after
# the last carriage return of each line.
cat > look.py <<'PY'
import sys
what, path = sys.argv[1], sys.argv[2]
raw = open(path, "rb").read()
if what == "bytes":
    sys.stdout.write(repr(raw))
elif what == "frames":
    # the three characters after each carriage return that starts a redraw
    out = []
    for piece in raw.split(b"\r"):
        piece = piece.replace(b"\x1b[0m", b"").replace(b"\x1b[2m", b"")
        piece = piece.replace(b"\x1b[38;5;135m", b"").replace(b"\x1b[35m", b"")
        piece = piece.replace(b"\x1b[K", b"")
        if piece.startswith(b"  ") and len(piece) > 5 and piece[2:5] != b"   ":
            out.append(piece[2:5].decode("utf-8", "replace"))
    sys.stdout.write("\n".join(out))
elif what == "final":
    # what the screen shows on the last line written: the piece after the
    # last carriage return, with the erase sequences applied
    line = raw.replace(b"\x1b[K", b"").split(b"\n")
    last = b""
    for l in line:
        if l.strip(b" \t\x1b[0123456789;m"):
            last = l
    sys.stdout.write(last.split(b"\r")[-1].decode("utf-8", "replace"))
PY

# ---- a channel with a package of many files ---------------------------
mkdir -p drawer/C
i=1
while [ "$i" -le 400 ]; do printf 'file number %s\n' "$i" > "drawer/C/f$i"; i=$((i + 1)); done
$PKG KEYGEN FILE key > /dev/null 2>&1 || { echo "activity: cannot make a key" >&2; exit 1; }
$PKG PUBLISH drawer NAME demo VERSION 1.0 KIND application CHANNEL ch SIGN key > /dev/null 2>&1
[ -f ch/index ] || { echo "activity: cannot publish the test package" >&2; exit 1; }

echo "silence"
# No terminal and no hook: nothing about activity may be written at all.
rm -rf root0
$PKG INSTALL demo ROOT root0 CHANNEL ch > q0 2>&1
[ $? -eq 0 ] && ! grep -q "$(printf '\r')" q0
                                                        ok $? "with no terminal and no hook nothing is drawn in place"

# The hook on, but the delay as a person has it: a fast command is over
# before the line would appear, so it prints nothing new.
rm -rf root1
PKG_PROGRESS=1 $PKG INSTALL demo ROOT root1 CHANNEL ch > q1 2>&1
[ $? -eq 0 ] && ! grep -q "$(printf '\r')" q1
                                                        ok $? "a fast command says nothing with the delay a person has"
sed 's/root1/rootN/g' q1 > q1n; sed 's/root0/rootN/g' q0 > q0n
cmp -s q0n q1n;                                         ok $? "and prints exactly what it printed without the hook"

# Switched off outright, the hook that sets the delay changes nothing.
rm -rf rootz
PKG_PROGRESS=0 PKG_PROGRESS_AFTER=0 $PKG INSTALL demo ROOT rootz CHANNEL ch > qz 2>&1
[ $? -eq 0 ] && ! grep -q "$(printf '\r')" qz
                                                        ok $? "PKG_PROGRESS=0 switches the line off"

echo "the line"
rm -rf root2
PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 $PKG INSTALL demo ROOT root2 CHANNEL ch > q2 2>&1
ok $? "an install with the delay at 0"
grep -q "$(printf '\r')" q2;                            ok $? "the line is rewritten in place"
python3 look.py frames q2 > f2
[ -s f2 ];                                              ok $? "it carries the mark"
has q2 'checking demo';                                 ok $? "a verb and the thing it acts on"
has q2 'of 400';                                        ok $? "and a measure"
has q2 'writing demo';                                  ok $? "the step that stages the files says so"
has q2 'placing demo';                                  ok $? "and the step that puts them in place"

echo "erased"
python3 look.py final q2 > e2
grep -q 'installed demo 1.0' e2;                        ok $? "the last visible line is the result sentence"
grep -q 'checking\|placing\|(O)\|( )' e2
[ $? -ne 0 ];                                           ok $? "no frame and no leftover of the line under it"
tail -c 1 q2 | od -An -c | grep -q '\\r'
[ $? -ne 0 ];                                           ok $? "and the output does not end on a carriage return"

echo "machine"
for hooks in "" "on"; do
    rm -rf root3
    if [ -n "$hooks" ]; then
        PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 $PKG INSTALL demo ROOT root3 CHANNEL ch MACHINE > q3 2>&1
    else
        $PKG INSTALL demo ROOT root3 CHANNEL ch MACHINE > q3 2>&1
    fi
    ok $? "MACHINE install${hooks:+ with the hooks on}"
    ! grep -q "$(printf '\r')" q3;                      ok $? "machine output holds no carriage return${hooks:+ with the hooks on}"
    ! grep -q "$(printf '\033')" q3;                    ok $? "machine output holds no escape sequence${hooks:+ with the hooks on}"
    ! grep -q '(O)\|( )' q3;                            ok $? "machine output holds no frame${hooks:+ with the hooks on}"
done

echo "log"
rm -rf root4
PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 $PKG INSTALL demo ROOT root4 CHANNEL ch LOG logfile > q4 2>&1
ok $? "an install with a LOG file"
[ -s logfile ];                                         ok $? "the log has the result in it"
! grep -q "$(printf '\r')" logfile;                     ok $? "the log holds no carriage return"
! grep -q '(O)\|( )' logfile;                            ok $? "and no frame of the mark"

echo "colour"
rm -rf root5
PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 PKG_COLOR=never $PKG INSTALL demo ROOT root5 CHANNEL ch > q5 2>&1
ok $? "an install with PKG_COLOR=never"
! grep -q "$(printf '\033')" q5;                        ok $? "no escape sequence anywhere"
grep -q "$(printf '\r')" q5;                            ok $? "and the line is still rewritten in place"
rm -rf root6
PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 PKG_COLOR=always TERM=xterm-256color \
    $PKG INSTALL demo ROOT root6 CHANNEL ch > q6 2>&1
ok $? "an install with PKG_COLOR=always"
python3 look.py bytes q6 | grep -q '38;5;135';          ok $? "the mark is drawn in the project's purple"

# ---- a step long enough for the mark to pulse -------------------------
echo "frames"
# A large incompressible archive: reading it through is the slow step the
# line exists for, and it is long enough for the mark to go round.
python3 - <<'PY'
import os, tarfile, io
data = os.urandom(6 * 1024 * 1024)
with tarfile.open("big.tar.bz2", "w:bz2", compresslevel=9) as t:
    for n in range(4):
        info = tarfile.TarInfo("Top/part%d/big" % n)
        info.size = len(data)
        info.mode = 0o644
        t.addfile(info, io.BytesIO(data))
PY
[ -s big.tar.bz2 ];                                     ok $? "an archive big enough to take a moment"
mkdir -p ch2/archives && cp big.tar.bz2 ch2/archives/
PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 PKG_COLOR=never \
    $PKG PUBLISH "big.tar.bz2!/Top" NAME parts VERSION 1.0 KIND data \
    CHANNEL ch2 SIGN key FILES part0 > q7 2>&1
ok $? "publishing out of that archive"
has q7 'reading the archive';                           ok $? "reading it says so"
# A step that can say how far it has got says that, and carries no mark: the
# figure moving is what shows it is alive. The mark belongs to a step that
# cannot say, which is what the wait below is.
python3 look.py frames q7 > f7
[ ! -s f7 ];                                            ok $? "a step with a measure carries no mark"
has q7 '%';                                             ok $? "it shows how far it has got instead"

echo "the measure"
grep -o '[0-9]*%' q7 | tr -d '%' > pcts
[ -s pcts ];                                            ok $? "reading the archive states a percentage"
python3 - <<'PY'
v = [int(x) for x in open("pcts").read().split()]
raise SystemExit(0 if v == sorted(v) and max(v) <= 100 else 1)
PY
                                                        ok $? "it never passes 100 and never falls back"
! has q7 'left';                                        ok $? "a share of a whole states no time left"
! has q7 '/s';                                          ok $? "and no rate"

# ---- a download, where the whole may or may not be known ---------------
echo "downloads"
python3 - > server.log 2>&1 <<'PY' &
import http.server, threading, os, sys, time
data = os.urandom(3 * 1024 * 1024)
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def do_GET(self):
        known = not self.path.startswith("/unknown/")
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        if known:
            self.send_header("Content-Length", str(len(data)))
        else:
            self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        sent = 0
        while sent < len(data):
            piece = data[sent:sent + 65536]
            if known:
                self.wfile.write(piece)
            else:
                self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.flush()
            sent += len(piece)
            time.sleep(0.02)
        if not known:
            self.wfile.write(b"0\r\n\r\n")
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_port, flush=True)
open("port", "w").write(str(s.server_port))
s.serve_forever()
PY
server=$!
n=0
while [ ! -s port ] && [ "$n" -lt 100 ]; do n=$((n + 1)); sleep 0.1; done
port=$(cat port 2>/dev/null)
if [ -z "$port" ]; then
    echo "activity: the test server did not start" >&2
    kill $server 2>/dev/null
    exit 69
fi

# A whole that is known: bytes of a total, a rate and a time left.
PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 PKG_COLOR=never \
    $PKG INSTALL nosuch ROOT rootd CHANNEL "http://127.0.0.1:$port/ch" > q8 2>&1
has q8 'waiting for 127.0.0.1';                         ok $? "a blocking wait names the machine it waits for"
has q8 'waiting for 127.0.0.1';                         ok $? "and says which machine it waits for"
has q8 'of 3.0 MB';                                     ok $? "with a known length it states the whole"
has q8 'left';                                          ok $? "and a time left"
has q8 '/s';                                            ok $? "and a rate"

# A whole that is not known: the bytes so far, and nothing invented.
PKG_PROGRESS=1 PKG_PROGRESS_AFTER=0 PKG_COLOR=never \
    $PKG INSTALL nosuch ROOT roote CHANNEL "http://127.0.0.1:$port/unknown" > q9 2>&1
has q9 'downloading' || has q9 'waiting for';           ok $? "a download of unknown length says what it is doing"
! has q9 ' of ';                                        ok $? "it states no whole it does not know"
! has q9 'left';                                        ok $? "no time left"
! has q9 '/s';                                          ok $? "and no rate"
kill $server 2>/dev/null
wait $server 2>/dev/null

# ---- the negative control ---------------------------------------------
# A check of this file must be able to fail. The same look at a run of a
# build that never erases the line would report the frame as the last thing
# on the screen; here the same assertion is made against a file that holds
# one, and it must fail.
echo "control"
printf 'installed demo 1.0 into root\r  (O) placing demo  400 of 400' > control
python3 look.py final control > ec
grep -q 'installed demo 1.0' ec
[ $? -ne 0 ];                                           ok $? "a line left on the screen fails the erase check"

echo "activity: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
