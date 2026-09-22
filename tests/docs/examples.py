#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Runs every example command in the documentation and checks its exit code.

    examples.py <pkg binary> [--write] [file.md ...]

An example is a ```console block: a line starting with "$ " is a command,
the lines under it its output. A command expected to be refused ends with
"# exits N". The commands of one file run in order, in one shell, in a
fresh directory holding the fixtures (tests/docs/fixtures.sh), with the
portal's address pointing at a local server. ```amigados and ```sh blocks
are shown, not run: AmigaDOS, and services that keep running.

--write puts each command's actual output under it in docs/*.md, except
where the old text differs only in hexadecimal (keys, digests)."""
import http.server, os, re, shlex, shutil, subprocess, sys, tempfile, threading

PORTAL = "https://aros-pkg.azurewebsites.net"
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(here))
pkg = os.path.abspath(sys.argv[1])
write = "--write" in sys.argv
def md_files():
    out = [os.path.join(repo, "README.md")]
    for d, _, fs in sorted(os.walk(os.path.join(repo, "docs"))):
        out += sorted(os.path.join(d, f) for f in fs if f.endswith(".md"))
    return out
files = [a for a in sys.argv[2:] if a != "--write"] or md_files()

def blocks(text):
    """(start line, end line, lang) of each fenced block."""
    lines, out, i = text.split("\n"), [], 0
    while i < len(lines):
        m = re.match(r"^```(\w*)\s*$", lines[i])
        if m:
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            out.append((i + 1, j, m.group(1)))
            i = j
        i += 1
    return lines, out

def expected(cmd):
    m = re.search(r"#\s*exits\s+(\d+)\s*$", cmd)
    return int(m.group(1)) if m else 0

tmp = tempfile.mkdtemp(prefix="pkg-docs.")
fix = os.path.join(tmp, "fixtures")
subprocess.run(["sh", os.path.join(here, "fixtures.sh"), fix], check=True,
               env=dict(os.environ, PKG=pkg))

class Quiet(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=os.path.join(fix, "portal"), **k)
    def log_message(self, *a):
        pass
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
threading.Thread(target=srv.serve_forever, daemon=True).start()
local = "http://127.0.0.1:%d" % srv.server_address[1]

checks = fails = shown = 0
words_cmd = []                       # (file, line index, words) judged, not run
for path in files:
    text = open(path).read()
    lines, bl = blocks(text)
    cmds = []                        # (line index, command)
    for s, e, lang in bl:
        if lang == "console":
            cmds += [(k, lines[k][2:]) for k in range(s, e) if lines[k].startswith("$ ")]
        elif lang in ("amigados", "sh", "text"):
            shown += sum(1 for k in range(s, e) if lines[k].strip() and not lines[k].lstrip().startswith(("#", ";")))
            # Not run here, but a pkg command among them is held to the
            # parser: its words are judged by pkg itself (PKG_CHECK_WORDS)
            # and nothing is done. A syntax line, with [optional] parts or a
            # choice a|b, is the grammar test's, not this one's.
            for k in range(s, e):
                m = re.match(r"^\s*(?:\S*[/:])?[Pp]kg\s+(.*)$", lines[k])
                if not m or re.search(r"\[|\||\.\.\.", m.group(1)):
                    continue
                words_cmd.append((path, k, m.group(1).split(";")[0].split(" #")[0].strip()))
    if not cmds:
        continue
    work = os.path.join(tmp, os.path.relpath(path, repo).replace(os.sep, "_"))
    os.makedirs(work)
    shutil.copytree(os.path.join(fix, "drawers"), work, dirs_exist_ok=True)
    shutil.copytree(os.path.join(fix, "channel"), os.path.join(work, "channel"))
    script = ["set +e"]
    for n, (_, c) in enumerate(cmds):
        script.append("{ %s\n} > .out%d 2>&1 < /dev/null; echo $? > .rc%d" % (c.replace(PORTAL, local), n, n))
    env = dict(os.environ, PATH=os.path.dirname(pkg) + os.pathsep + os.environ["PATH"],
               HOME=work, PKG_CACHE=os.path.join(work, ".cache"),
               # the examples register environments: never in the person's own
               # configuration, whatever XDG_CONFIG_HOME their shell sets
               XDG_CONFIG_HOME=os.path.join(work, ".config"))
    for k in ("PKG_SIGNKEY", "PKG_OUTPUT", "PKG_TRACE", "PKG_PUSHKEY", "PKG_PROGRESS"):
        env.pop(k, None)
    subprocess.run(["bash", "-c", "\n".join(script)], cwd=work, env=env, timeout=600)
    outputs = {}
    for n, (k, c) in enumerate(cmds):
        checks += 1
        rc = int(open(os.path.join(work, ".rc%d" % n)).read().strip() or "-1")
        out = open(os.path.join(work, ".out%d" % n)).read().replace(local, PORTAL).replace(work + "/", "")
        outputs[k] = out.rstrip("\n").split("\n") if out.strip() else []
        if rc != expected(c):
            fails += 1
            print("FAIL %s:%d: exit %d, expected %d: %s" % (os.path.relpath(path, repo), k + 1, rc, expected(c), c))
            for l in outputs[k][:6]:
                print("       " + l)
    if write and path.startswith(os.path.join(repo, "docs") + os.sep):
        mask = lambda ls: [re.sub(r"[0-9a-f]{12,}", "#", l) for l in ls]
        new, k = [], 0
        while k < len(lines):
            new.append(lines[k])
            if k in outputs:
                j = k + 1
                old = []
                while j < len(lines) and not lines[j].startswith("$ ") and not lines[j].startswith("```"):
                    old.append(lines[j]); j += 1
                new += old if mask(old) == mask(outputs[k]) else outputs[k]
                k = j
                continue
            k += 1
        if new != lines:
            open(path, "w").write("\n".join(new))
            print("wrote the outputs of %s" % os.path.relpath(path, repo))

words = 0
for path, k, c in words_cmd:
    try:
        argv = shlex.split(c)
    except ValueError:
        argv = c.split()
    if not argv:
        continue
    words += 1
    r = subprocess.run([pkg] + argv, capture_output=True, text=True, timeout=60,
                       env=dict(os.environ, PKG_CHECK_WORDS="1", HOME=tmp,
                                XDG_CONFIG_HOME=os.path.join(tmp, ".config")))
    if r.returncode != 0:
        fails += 1
        print("FAIL %s:%d: pkg does not take these words: pkg %s" % (os.path.relpath(path, repo), k + 1, c))
        for l in (r.stdout + r.stderr).strip().split("\n")[:3]:
            print("       " + l)
srv.shutdown()
shutil.rmtree(tmp, ignore_errors=True)
print("docs-examples: %d commands run, %d more judged by pkg's own parser without running, %d failures"
      % (checks, words, fails))
sys.exit(1 if fails else 0)
