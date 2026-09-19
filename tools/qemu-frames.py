#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Records what a QEMU guest shows, through its monitor socket, until QEMU
exits: a screenshot every INTERVAL seconds, kept only when the screen
changed, and a list of how long each frame stayed on screen.

    qemu-frames.py <monitor socket> <frames dir> [interval, default 0.5]

Frames are <frames dir>/NNNNNN.ppm, numbered on from what the directory
already holds, so the boots of one run follow each other. <frames dir>/
frames.txt is an ffmpeg concat list (file, duration), extended at every
frame; tools/qemu-video.sh makes the video from it."""
import hashlib, os, socket, sys, time

sock_path, out = sys.argv[1], sys.argv[2]
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
os.makedirs(out, exist_ok=True)
listing = os.path.join(out, "frames.txt")
n = len([f for f in os.listdir(out) if f.endswith(".ppm") and f[:6].isdigit()])

s = None
for _ in range(100):                         # QEMU makes the socket as it starts
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock_path)
        break
    except OSError:
        s = None
        time.sleep(0.1)
if s is None:
    sys.exit("qemu-frames: no monitor at %s" % sock_path)
s.settimeout(5)

def ask(line):
    s.sendall((line + "\n").encode())
    got = b""
    while not got.endswith(b"(qemu) "):
        chunk = s.recv(65536)
        if not chunk:
            raise EOFError
        got += chunk
    return got

last_digest, last_time, last_name = None, None, None
tmp = os.path.join(out, "grab.ppm")
try:
    ask("")                                  # the banner and first prompt
    while True:
        t0 = time.time()
        ask("screendump " + tmp)
        if os.path.exists(tmp):
            data = open(tmp, "rb").read()
            d = hashlib.sha1(data).digest()
            if d != last_digest:
                if last_name is not None:
                    with open(listing, "a") as f:
                        f.write("file '%s'\nduration %.2f\n" % (last_name, t0 - last_time))
                n += 1
                last_name = "%06d.ppm" % n
                os.replace(tmp, os.path.join(out, last_name))
                last_digest, last_time = d, t0
        time.sleep(max(0.0, interval - (time.time() - t0)))
except (EOFError, OSError, socket.timeout):
    pass
if last_name is not None:
    with open(listing, "a") as f:
        f.write("file '%s'\nduration %.2f\n" % (last_name, max(0.5, time.time() - last_time)))
