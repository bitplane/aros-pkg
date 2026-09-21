#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Observe activity while a real HTTP response is deliberately withheld."""
import http.server
import os
from pathlib import Path
import re
import selectors
import ssl
import subprocess
import tempfile
import threading
import time

pkg = str(Path(os.environ.get('PKG', './build/pkg')).resolve())
frames = re.compile(rb'\r  ( \. | o | O |\(O\)|\( \)) ')


def check(known, certificate=None):
    milestones = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if certificate and self.path.startswith('/ch/'):
                self.send_response(302)
                self.send_header('Location', f'https://127.0.0.1:{self.server.server_port}/final')
                self.send_header('Content-Length', '17')
                self.end_headers()
                self.wfile.flush()
                time.sleep(0.6)
                self.wfile.write(b'redirect response')
                return
            time.sleep(1.2)
            milestones['headers'] = time.monotonic()
            self.send_response(200)
            if known:
                self.send_header('Content-Length', str(1024 * 1024))
            else:
                self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            self.wfile.flush()
            time.sleep(1.2)
            milestones['body'] = time.monotonic()
            for _ in range(16):
                piece = b'x' * 65536
                if known:
                    self.wfile.write(piece)
                else:
                    self.wfile.write(b'10000\r\n' + piece + b'\r\n')
                self.wfile.flush()
                time.sleep(0.05)
            if not known:
                self.wfile.write(b'0\r\n\r\n')

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    scheme = 'https' if certificate else 'http'
    if certificate:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate + '/cert.pem', certificate + '/key.pem')
        server.socket = context.wrap_socket(server.socket, server_side=True)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix='pkg-activity-live-') as root:
            env = dict(os.environ, PKG_PROGRESS='1', PKG_COLOR='never',
                       PKG_PROGRESS_AFTER='500', PKG_CACHE=root + '/cache', LC_ALL='C')
            if certificate:
                env['CURL_CA_BUNDLE'] = certificate + '/cert.pem'
            proc = subprocess.Popen([pkg, 'INSTALL', 'missing', 'ROOT', root + '/root',
                                     'CHANNEL', f'{scheme}://127.0.0.1:{server.server_port}/ch'],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
            samples = []
            try:
                with selectors.DefaultSelector() as poll:
                    poll.register(proc.stdout, selectors.EVENT_READ)
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        if not poll.select(0.1):
                            continue
                        data = os.read(proc.stdout.fileno(), 65536)
                        if not data:
                            break
                        samples.append((time.monotonic(), data))
                    else:
                        raise AssertionError('command timed out')
                proc.wait(timeout=2)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                proc.stdout.close()
            assert 'body' in milestones, 'server did not send the body'
            before_headers = b''.join(s for t, s in samples if t < milestones['headers'])
            before_body = b''.join(s for t, s in samples if t < milestones['body'])
            assert b'/17 bytes' not in before_body, 'redirect length must not become the file size'
            assert len(set(frames.findall(before_headers))) >= 2, 'pulse must move before headers arrive'
            if known:
                assert b'downloading index, 0.0/1.0 MB' in before_body, 'zero must precede body bytes'
                after_zero = before_body.split(b'0.0/1.0 MB', 1)[1]
                assert not frames.search(after_zero), 'known progress must keep its figure during a read wait'
            else:
                header_wait = b''.join(s for t, s in samples if milestones['headers'] < t < milestones['body'])
                assert len(set(frames.findall(header_wait))) >= 2, 'unknown body wait must keep pulsing'
                assert b'/1.0 MB' not in before_body, 'unknown response must not invent a total'
            # The payload deliberately fails package parsing, exercising error cleanup.
            assert proc.returncode != 0, 'invalid channel must fail'
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


check(True)
check(False)
with tempfile.TemporaryDirectory(prefix='pkg-activity-tls-') as certificate:
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', certificate + '/key.pem', '-out', certificate + '/cert.pem',
                    '-subj', '/CN=localhost', '-days', '1',
                    '-addext', 'subjectAltName=IP:127.0.0.1'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    check(True, certificate)
    check(False, certificate)
print('activity network: pulse before headers, zero before body, unknown-body pulse, error cleanup passed')
