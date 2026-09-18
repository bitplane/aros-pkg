#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Hostile inputs. Each check crafts something a careless tool would act on,
# and asserts the safe outcome: a refusal with the right class, nothing
# written or deleted outside the root or the channel, no crash. A check that
# fails here is a hole, to be fixed in the source, never in this file.
#
# Surfaces: paths in manifests and containers (AmigaDOS spellings included),
# archives used as a source, drawers, the channel, the root's own database,
# and the size of what an archive claims.

set -u
PKG=${PKG:-./build/pkg}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-hostile.XXXXXX")
trap '[ "${KEEP:-0}" = 1 ] || rm -rf "$T"' EXIT
cd "$T" || exit 1
export COPYFILE_DISABLE=1
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }
# Everything under $T except the places a command may legitimately write.
outside() { find "$T" -newer "$T/.mark" -type f ! -path "$1/*" ! -path "$T/out*" ! -name '.mark' | head -3; }
mark() { sleep 1; touch "$T/.mark"; }

$PKG KEYGEN FILE key > /dev/null
export PKG_SIGNKEY="$T/key"
PUB=$(awk '/^Public:/{print $2}' key)

# A signed channel entry with an arbitrary manifest: what a publisher who
# went bad, or a stolen key, could put in a channel. The container holds the
# files the manifest lists, so only Pkg's own path rules stand in the way.
craft() {  # craft <channel> <name> <path> [path...]: one version whose files have those paths
    ch=$1 name=$2; shift 2
    python3 - "$ch" "$name" "$T/key" "$PKG" "$@" <<'PY'
import hashlib, os, subprocess, sys
ch, name, key, pkg = sys.argv[1:5]
paths = sys.argv[5:]
work = os.path.join(os.path.dirname(ch), "craft-" + name)
os.makedirs(work, exist_ok=True)
# Build a real drawer, publish it, then rewrite the manifest's paths and
# re-sign it: the container is rebuilt by Pkg itself from the same bytes.
src = os.path.join(work, "src")
os.makedirs(os.path.join(src, "C"), exist_ok=True)
for i, _ in enumerate(paths):
    open(os.path.join(src, "C", "f%d" % i), "wb").write(b"payload %d\n" % i)
subprocess.run([pkg, "PUBLISH", src, "CHANNEL", ch, "NAME", name, "VERSION", "1", "KIND", "data"],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
PY
    # Swap the paths in the stored manifest (byte for byte, backslashes and
    # all) and sign it again with the same key.
    m=$(ls -t "$ch/objects/"*.manifest | head -1)
    old=$(basename "$m" .manifest)
    python3 - "$m" "$@" <<'PY2'
import sys
m = sys.argv[1]
text = open(m, "rb").read().decode("utf-8")
for i, p in enumerate(sys.argv[2:]):
    text = text.replace(" C/f%d\n" % i, " %s\n" % p)
open(m, "wb").write(text.encode("utf-8"))
PY2
    new=$(shasum -a 256 "$m" | awk '{print $1}')
    $PKG SIGN "$m" KEY "$T/key" OUT "$T/sig.new" > /dev/null 2>&1
    [ "$new" = "$old" ] || { mv "$m" "$ch/objects/$new.manifest"; rm -f "$ch/objects/$old.sig"; }
    cp "$T/sig.new" "$ch/objects/$new.sig"
    sed -i '' "s|$old|$new|" "$ch/index"
}

echo "paths"
# The control: the same forging with a harmless path must install, or every
# refusal below could be for another reason.
mkdir -p "$T/p0"
craft "$T/p0" fine 'C/f0' 2>/dev/null
$PKG INSTALL fine ROOT "$T/r0" CHANNEL "$T/p0" MACHINE > "$T/out-p0" 2>&1
[ $? -eq 0 ] && [ "$(cat "$T/r0/C/f0" 2>/dev/null)" = "payload 0" ]
                                                      ok $? "control: a manifest rewritten and signed again installs, so refusals below are the path rules'"
# Every path an AmigaDOS or POSIX reader could take outside the root.
n=0
for p in '../escape' 'C/../../escape' '/escape' 'C//escape' 'RAM:escape' 'SYS:C/escape' 'C/:escape' \
         './escape' 'C/./escape' '.pkg/db/escape' 'C\..\escape' ''; do
    n=$((n + 1))
    ch="$T/p$n"; mkdir -p "$ch"
    craft "$ch" "evil$n" "$p" 2>/dev/null
    mark
    $PKG INSTALL "evil$n" ROOT "$T/r$n" CHANNEL "$ch" MACHINE > "$T/out-p$n" 2>&1
    code=$?
    [ "$code" -eq 12 ] && has "$T/out-p$n" '^reason: .*manifest line .*: the path' \
        && [ -z "$(outside "$T/r$n")" ] && [ ! -e "$T/escape" ] && [ ! -e "$T/r$n/.pkg/db/escape" ]
                                                      ok $? "a manifest path '$p' is refused by the path rules (12), nothing written outside the root"
done

echo "another_package"
# A package that ships a file another installed package owns.
mkdir -p d1/C d2/C
printf 'first' > d1/C/Shared; printf 'second' > d2/C/Shared
$PKG PUBLISH d1 CHANNEL ch2 NAME one VERSION 1 KIND data > /dev/null
$PKG PUBLISH d2 CHANNEL ch2 NAME two VERSION 1 KIND data > /dev/null
$PKG INSTALL one ROOT r2 CHANNEL ch2 > /dev/null 2>&1
$PKG INSTALL two ROOT r2 CHANNEL ch2 MACHINE > out-two 2>&1
[ $? -eq 15 ] && [ "$(cat r2/C/Shared)" = first ];  ok $? "a second package cannot take over a file the first installed"
mkdir -p d3/c
printf 'third' > d3/c/shared
$PKG PUBLISH d3 CHANNEL ch2 NAME three VERSION 1 KIND data > /dev/null
$PKG INSTALL three ROOT r2 CHANNEL ch2 MACHINE > out-three 2>&1
code=$?
[ "$(cat r2/C/Shared)" = first ];                     ok $? "nor by spelling its path in another case (exit $code)"

echo "database"
# The root's own database, tampered: REMOVE must never follow it outside.
printf 'precious' > "$T/precious"
mkdir -p d4/C; printf 'x' > d4/C/X
$PKG PUBLISH d4 CHANNEL ch4 NAME victim VERSION 1 KIND data > /dev/null
$PKG INSTALL victim ROOT r4 CHANNEL ch4 > /dev/null 2>&1
sed -i '' 's| C/X$| ../../precious|' r4/.pkg/db/victim
$PKG REMOVE victim ROOT r4 MACHINE > out-db 2>&1
code=$?
[ -f "$T/precious" ] && [ "$code" -ne 0 ];            ok $? "a database entry naming a path outside the root is refused (exit $code), and deletes nothing"
sed -i '' 's| ../../precious$| C/X|' r4/.pkg/db/victim
ln -sf "$T/precious" r4/C/X 2>/dev/null
$PKG REMOVE victim ROOT r4 MACHINE > out-db2 2>&1
[ -f "$T/precious" ];                                 ok $? "an installed file replaced by a link out of the root: REMOVE leaves the target alone"

echo "archives"
mkdir -p a/Top/Extras/App
printf 'app' > a/Top/Extras/App/App
python3 - <<'PY'
import io, tarfile
def add(t, name, data=b"x", kind=tarfile.REGTYPE, link=""):
    i = tarfile.TarInfo(name); i.type = kind; i.linkname = link; i.size = len(data) if kind == tarfile.REGTYPE else 0
    t.addfile(i, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
with tarfile.open("evil.tar.bz2", "w:bz2") as t:
    add(t, "Top/Extras/App/App", b"app")
    add(t, "Top/../escape", b"out")
    add(t, "/abs-escape", b"out")
    add(t, "Top/Extras/App/Link", kind=tarfile.SYMTYPE, link="../../../../precious")
    add(t, "Top/Extras/App/Hard", kind=tarfile.LNKTYPE, link="Top/../../precious")
PY
mkdir -p cha/archives; cp evil.tar.bz2 cha/archives/
mark
$PKG PUBLISH "cha/archives/evil.tar.bz2!/Top" CHANNEL cha NAME arch VERSION 1 KIND data MACHINE > out-a1 2>&1
code=$?
[ -z "$(outside "$T/cha")" ];                          ok $? "publishing from an archive with ../, absolute and link members writes nothing outside the channel (exit $code)"
if [ "$code" -eq 0 ]; then
    m=$(ls cha/objects/*.manifest)
    ! grep -q -E ' (\.\./|/|Extras/App/Link|Extras/App/Hard)' $m ;  ok $? "and the manifest names none of them"
    $PKG INSTALL arch ROOT ra CHANNEL cha MACHINE > out-a2 2>&1
    [ ! -e "$T/escape" ] && [ ! -e /abs-escape ] && [ ! -L ra/Extras/App/Link ] && [ ! -e ra/Extras/App/Hard ]
                                                      ok $? "installing it writes only the plain file, inside the root"
fi

echo "bomb"
# A small archive that claims or unpacks to far more than it holds: Pkg must
# refuse it in bounded time and memory, not try to hold it.
python3 - <<'PY'
import bz2
# 1 GiB of zeros compresses to about 750 bytes per 45 MB block: a few KB
header = bytearray(512)
name = b"Top/zeros"
header[:len(name)] = name
size = 1 << 30
header[100:108] = b"0000644\0"; header[108:116] = b"0000000\0"; header[116:124] = b"0000000\0"
header[124:136] = ("%011o" % size).encode() + b"\0"
header[136:148] = b"00000000000\0"; header[156:157] = b"0"; header[257:263] = b"ustar\0"; header[263:265] = b"00"
header[148:156] = b"        "
header[148:156] = ("%06o\0 " % sum(header)).encode()
c = bz2.BZ2Compressor(9)
out = [c.compress(bytes(header))]
chunk = bytes(1 << 20)
for _ in range(size >> 20):
    out.append(c.compress(chunk))
out.append(c.compress(bytes(1024)))
out.append(c.flush())
open("bomb.tar.bz2", "wb").write(b"".join(out))
PY
mkdir -p chb/archives; cp bomb.tar.bz2 chb/archives/
rss() { awk '/maximum resident set size/{print int($1 / 1048576)}' "$1"; }
/usr/bin/time -l $PKG PUBLISH "chb/archives/bomb.tar.bz2!/Top" CHANNEL chb NAME bomb VERSION 1 KIND data MACHINE > out-b1 2> out-b1.time
code=$?
[ "$code" -lt 128 ] && [ "$(rss out-b1.time)" -lt 64 ]
                                                      ok $? "publishing from a $(wc -c < bomb.tar.bz2 | tr -d ' ')-byte archive of 1 GiB stays under 64 MB of memory ($(rss out-b1.time) MB, exit $code)"
# Install: a channel whose archive was swapped for the bomb, same member
# name, where the signed manifest says the file is 3 bytes.
python3 - <<'PY2'
import io, tarfile
with tarfile.open("small.tar.bz2", "w:bz2") as t:
    i = tarfile.TarInfo("Top/zeros"); i.size = 3; t.addfile(i, io.BytesIO(b"abc"))
PY2
mkdir -p chs/archives; cp small.tar.bz2 chs/archives/bomb.tar.bz2
$PKG PUBLISH "chs/archives/bomb.tar.bz2!/Top" CHANNEL chs NAME small VERSION 1 KIND data > /dev/null 2>&1
cp bomb.tar.bz2 chs/archives/bomb.tar.bz2
/usr/bin/time -l $PKG INSTALL small ROOT rb CHANNEL chs MACHINE > out-b2 2> out-b2.time
code=$?
[ "$code" -eq 12 ] && has out-b2 'longer than the 3 bytes' && [ "$(rss out-b2.time)" -lt 64 ] && [ ! -e rb/zeros ]
                                                      ok $? "installing from an archive swapped for a bomb stops at the manifest's size: 12, $(rss out-b2.time) MB"

echo "channel"
mkdir -p d5/C; printf 'ok' > d5/C/Ok
$PKG PUBLISH d5 CHANNEL ch5 NAME good VERSION 1 KIND data > /dev/null
printf 'good 1 generic %s\nevil 1 generic ../../%s\n' "$(awk '{print $4}' ch5/index)" "$(awk '{print $4}' ch5/index)" > ch5/index.new
mv ch5/index.new ch5/index
$PKG SHOW CHANNEL ch5 MACHINE > out-c1 2>&1
code=$?
[ "$code" -ne 0 ] || ! has out-c1 '^entry: evil ';     ok $? "an index line whose digest is a path is refused (exit $code)"
printf 'good 1 generic %s\n' "$(ls ch5/objects/*.manifest | head -1 | xargs basename | cut -d. -f1)" > ch5/index
m=$(ls ch5/objects/*.manifest | head -1)
printf 'Name: good\n' >> "$m"
$PKG INSTALL good ROOT r5 CHANNEL ch5 MACHINE > out-c2 2>&1
[ $? -eq 12 ] && [ ! -e r5/C/Ok ];                    ok $? "a manifest changed after signing is refused, nothing placed"
Src='"$T/x"'
mkdir -p d6/C; printf 's' > d6/C/S
$PKG PUBLISH d6 CHANNEL ch6 NAME src VERSION 1 KIND data > /dev/null
m=$(ls ch6/objects/*.manifest)
sed -i '' 's|^Payload: .*|Source: ../../evil.tar.bz2!/Top|' "$m"
$PKG INSTALL src ROOT r6 CHANNEL ch6 MACHINE > out-c3 2>&1
code=$?
[ "$code" -ne 0 ] && [ ! -e r6/C/S ];                 ok $? "a Source naming an archive outside the channel is refused (exit $code)"

echo
echo "hostile: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
