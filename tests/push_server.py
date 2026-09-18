#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""A stand-in for the portal's push API, for tests/push.sh: the paths,
answers and rules agreed with the portal (plan, files in resumable parts,
commit checked with Pkg itself), over plain HTTP on this machine.

    push_server.py <root dir> <pkg binary> <port file> [<log>]

Serves <root>/<channel>/... for reading. Key: "testkey", for every
channel. PKG_TEST_FAIL_PART=<n> makes the n-th part received fail once, to
test resuming."""
import hashlib, http.server, os, re, shutil, subprocess, sys, tempfile

ROOT, PKG, PORTFILE = sys.argv[1], sys.argv[2], sys.argv[3]
LOG = open(sys.argv[4], "a") if len(sys.argv) > 4 else None
KEY = "testkey"
STAGE = os.path.join(ROOT, "_staging")
IMMUTABLE = re.compile(r"^(objects/[0-9a-f]{64}\.(manifest|sig|pkg|withdrawn|withdrawn\.sig)|archives/[^/]+)$")
MUTABLE = re.compile(r"^(Bootstrap/[^/]+/Pkg|Install-Pkg|ReadMe)$")
fail_part = int(os.environ.get("PKG_TEST_FAIL_PART", "0"))
parts_seen = [0]

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

def allowed(rel):
    if rel.endswith(".pkgidx") or rel.endswith(".sha256"):
        return False
    return bool(IMMUTABLE.match(rel) or MUTABLE.match(rel))

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def log_message(self, *a):
        pass

    def answer(self, code, lines):
        body = ("\n".join(lines) + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def split(self):
        m = re.match(r"^/([^/]+)/_push/(plan|files|commit)(?:/(.*))?$", self.path)
        return m.groups() if m else (None, None, None)

    def authorised(self):
        if self.headers.get("Authorization") != "Bearer " + KEY:
            self.answer(401, ["result: refused", "class: key", "code: 14",
                              "reason: this key may not push here", "next: ask-requester"])
            return False
        return True

    def body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n)

    def do_POST(self):
        ch, verb, _ = self.split()
        if LOG: LOG.write("POST %s\n" % self.path); LOG.flush()
        if ch is None or not self.authorised():
            if ch is None: self.answer(400, ["result: refused", "class: usage", "code: 20", "reason: no such call"])
            return
        chan = os.path.join(ROOT, ch)
        stage = os.path.join(STAGE, ch)
        data = self.body().decode()
        if verb == "plan":
            need, total = [], 0
            for line in data.splitlines():
                if not line.strip():
                    continue
                rel, digest, size = line.rsplit(" ", 2)
                total += 1
                if not allowed(rel):
                    continue
                have = os.path.join(chan, rel)
                staged = os.path.join(stage, rel)
                if os.path.isfile(have) and sha(have) == digest:
                    continue
                if os.path.isfile(staged) and sha(staged) == digest:
                    continue
                need.append(rel)
            self.answer(200, ["need: " + r for r in need] +
                        ["summary: %d of %d files are needed" % (len(need), total)])
        elif verb == "commit":
            work = tempfile.mkdtemp(dir=STAGE)
            cand = os.path.join(work, "ch")
            if os.path.isdir(chan):
                shutil.copytree(chan, cand)
            else:
                os.makedirs(cand)
            if os.path.isdir(stage):
                for dp, _, fs in os.walk(stage):
                    for f in fs:
                        src = os.path.join(dp, f)
                        rel = os.path.relpath(src, stage)
                        os.makedirs(os.path.dirname(os.path.join(cand, rel)), exist_ok=True)
                        shutil.copy2(src, os.path.join(cand, rel))
            old = open(os.path.join(chan, "index")).read().splitlines() if os.path.isfile(os.path.join(chan, "index")) else []
            new = [l for l in data.splitlines() if l.strip()]
            merged = old + [l for l in new if l not in old]
            open(os.path.join(cand, "index"), "w").write("".join(l + "\n" for l in merged))
            r = subprocess.run([PKG, "SHOW", "CHANNEL", cand, "METADATA", "MACHINE"], capture_output=True, text=True)
            published = [l for l in new if l not in old]
            unchanged = [l for l in new if l in old]
            if "bad: 0" not in r.stdout:
                problems = [l[len("problem: "):] for l in r.stdout.splitlines() if l.startswith("problem: ")]
                shutil.rmtree(work)
                self.answer(200, ["refused: " + p for p in problems] +
                            ["result: refused", "class: integrity", "code: 12",
                             "reason: %d of the pushed entries do not check out; nothing was published" % len(problems),
                             "summary: nothing was published: %d problem%s" % (len(problems), "" if len(problems) == 1 else "s"),
                             "next: stop"])
                return
            if os.path.isdir(chan):
                shutil.rmtree(chan)
            shutil.move(cand, chan)
            shutil.rmtree(work, ignore_errors=True)
            shutil.rmtree(stage, ignore_errors=True)
            self.answer(200, ["published: " + " ".join(l.split()[:3]) for l in published] +
                        ["result: " + ("published" if published else "unchanged"),
                         "summary: %d published, %d unchanged" % (len(published), len(unchanged))])
        else:
            self.answer(400, ["result: refused", "class: usage", "code: 20", "reason: no such call"])

    def do_PUT(self):
        ch, verb, rel = self.split()
        if LOG: LOG.write("PUT %s Content-Range: %s\n" % (self.path, self.headers.get("Content-Range", "-"))); LOG.flush()
        if ch is None or verb != "files" or not self.authorised():
            if ch is None: self.answer(400, ["result: refused", "code: 20", "reason: no such call"])
            return
        if not allowed(rel):
            self.body()
            self.answer(200, ["result: refused", "class: usage", "code: 20", "reason: %s is not a channel file" % rel])
            return
        path = os.path.join(STAGE, ch, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        data = self.body()
        cr = self.headers.get("Content-Range")
        if cr:
            m = re.match(r"bytes (\d+)-(\d+)/(\d+)", cr)
            start, end, total = int(m.group(1)), int(m.group(2)), int(m.group(3))
            have = os.path.getsize(path) if os.path.exists(path) else 0
            if start != have:
                self.answer(200, ["result: partial", "received: %d" % have])
                return
            parts_seen[0] += 1
            global fail_part
            if fail_part and parts_seen[0] == fail_part:
                fail_part = 0
                self.answer(500, ["result: refused", "reason: a failure made on purpose"])
                return
            with open(path, "ab") as f:
                f.write(data)
            if end + 1 < total:
                self.answer(200, ["result: partial", "received: %d" % (end + 1)])
                return
        else:
            with open(path, "wb") as f:
                f.write(data)
        name = os.path.basename(rel)
        if re.match(r"^[0-9a-f]{64}\.(manifest|pkg)$", name) and sha(path) != name.split(".")[0]:
            os.unlink(path)
            self.answer(200, ["result: refused", "class: integrity", "code: 12",
                              "reason: %s does not hold what its name says" % rel])
            return
        self.answer(200, ["result: received", "received: %d" % os.path.getsize(path)])

os.makedirs(STAGE, exist_ok=True)
http.server.ThreadingHTTPServer.allow_reuse_address = True
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(PORTFILE, "w").write(str(s.server_address[1]))
s.serve_forever()
