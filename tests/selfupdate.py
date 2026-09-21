#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Check signed update metadata, root immutability and refresh in one process."""
import functools
import hashlib
import http.server
import os
from pathlib import Path
import subprocess
import tempfile
import threading

pkg = str(Path(os.environ.get('PKG', './build/pkg')).resolve())
probe = str(Path(os.environ.get('UPDATE_PROBE', './build/test_update')).resolve())
example = str(Path(os.environ.get('UPDATE_EXAMPLE', './build/example-selfupdate')).resolve())
checks = 0

def run(*args):
    return subprocess.run([pkg, *map(str, args)], check=True, capture_output=True)

def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}

with tempfile.TemporaryDirectory(prefix='pkg-update-') as tmp:
    base = Path(tmp)
    root, channel, drawer, key = [base / s for s in ('root', 'channel', 'drawer', 'key')]
    drawer.mkdir()
    changes = base / 'changes.txt'
    run('KEYGEN', 'FILE', key)

    def publish(version, signing=key, ch=channel):
        (drawer / 'hello.txt').write_text('hello ' + version)
        changes.write_text('Changes in ' + version + '\n\nA signed improvement.\n')
        run('PUBLISH', drawer, 'NAME', 'hello', 'VERSION', version, 'KIND', 'data',
            'CHANNEL', ch, 'SIGN', signing, 'CHANGES', changes)

    publish('1.0')
    run('INSTALL', 'hello', 'ROOT', root, 'CHANNEL', channel)
    baseline = snapshot(root)

    def check(expected, expected_version=None, ch=channel, name='hello', installation=root):
        global checks
        result = subprocess.run([probe, name, str(installation), str(ch)],
                                input='check\n', text=True, capture_output=True, check=True)
        text = result.stdout
        assert f'state: {expected}\n' in text, text
        if expected_version:
            assert f'offered: {expected_version}\n' in text, text
        assert snapshot(root) == baseline, 'check changed installed files, database or trust'
        checks += 1
        return text

    check(0, '1.0')
    check(2, name='absent', ch=base / 'missing-channel')
    check(3, ch=base / 'missing-channel')
    empty = base / 'empty'; empty.mkdir()
    check(6, ch=empty)
    publish('2.0')
    text = check(1, '2.0')
    assert 'Changes in 2.0\n\nA signed improvement.' in text
    assert 'download-known: 0' in text and 'bytes: 9' in text
    # The check needs only metadata: remove every payload from the channel.
    for payload in channel.glob('objects/*.pkg'):
        payload.unlink()
    check(1, '2.0')
    manifest = next(p for p in channel.glob('objects/*.manifest') if b'Version: 2.0\n' in p.read_bytes())
    saved = manifest.read_bytes(); manifest.write_bytes(saved + b'Short: forged\n')
    assert 'code: 12' in check(7)
    manifest.write_bytes(saved)
    signature = manifest.with_suffix('.sig')
    saved_sig = signature.read_bytes(); signature.write_bytes(b'Signer: broken\n')
    assert 'code: 13' in check(7)
    signature.write_bytes(saved_sig)
    run('WITHDRAW', 'hello', 'VERSION', '2.0', 'CHANNEL', channel, 'SIGN', key)
    check(0, '1.0')
    run('WITHDRAW', 'hello', 'VERSION', '1.0', 'CHANNEL', channel, 'SIGN', key)
    check(4)
    publish('3.0')
    assert 'withdrawn: 1' in check(1, '3.0')
    other_key = base / 'other.key'; other = base / 'other'
    run('KEYGEN', 'FILE', other_key)
    publish('4.0', other_key, other)
    check(5, '4.0', ch=other)

    # Root-configured channels must also report changed keys, including
    # when every channel has rotated away from the installed key.
    second = base / 'second'
    publish('5.0', other_key, second)
    channel_list = root / '.pkg/channels'
    saved_channels = channel_list.read_bytes() if channel_list.exists() else None
    channel_list.write_text(str(other) + '\n' + str(second) + '\n')
    baseline = snapshot(root)
    check(5, '5.0', ch='-')
    channel_list.write_text(str(base / 'missing') + '\n')
    baseline = snapshot(root)
    check(3, ch='-')
    if saved_channels is None: channel_list.unlink()
    else: channel_list.write_bytes(saved_channels)
    baseline = snapshot(root)
    # A generic host installation with no recorded CPU cannot choose a
    # CPU-specific package merely because its version is higher.
    run('PUBLISH', drawer, 'NAME', 'hello', 'VERSION', '9.0', 'KIND', 'data',
        'ARCH', 'x86_64', 'CHANNEL', channel, 'SIGN', key)
    check(1, '3.0')
    saved_index = (channel / 'index').read_text()
    (channel / 'index').write_text(saved_index.replace('hello 9.0 x86_64 ', 'hello 9.0 generic '))
    assert 'code: 12' in check(7)  # Signed architecture must agree with index.
    (channel / 'index').write_text(saved_index)
    (channel / 'index').write_text('hello 3.0 generic ' + 'z' * 64 + '\n')
    assert 'code: 12' in check(7)
    (channel / 'index').write_text(saved_index)

    config = base / 'hello.pkgupdate'
    config.write_text('Format: pkg-update 1\nPackage: hello\nRoot: root\nChannel: channel\n')
    response = subprocess.run([example, '--config', str(config)], text=True, capture_output=True, check=True)
    assert 'Update available' in response.stdout and 'Changes in 3.0' in response.stdout
    checks += 1

    requested = []
    fail_index = False
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_GET(self):
            requested.append(self.path)
            if fail_index and self.path == '/index':
                self.send_error(503)
                return
            super().do_GET()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(channel)))
    worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
    url = f'http://127.0.0.1:{server.server_port}'
    env = dict(os.environ, PKG_CACHE=str(base / 'cache'))
    client = subprocess.Popen([probe, 'hello', str(root), url], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    def again(expected, version):
        global checks
        client.stdin.write('check\n'); client.stdin.flush()
        answer = ''
        while True:
            line = client.stdout.readline()
            assert line, 'persistent client exited'
            if line == 'END\n': break
            answer += line
        assert f'state: {expected}\n' in answer, answer
        if version: assert f'offered: {version}\n' in answer, answer
        assert snapshot(root) == baseline
        checks += 1
    try:
        again(1, '3.0')
        publish('3.1')
        again(1, '3.1')
        assert requested.count('/index') == 2, requested
        assert not any(p.endswith('.pkg') or '/archives/' in p for p in requested), requested
        fail_index = True
        again(3, None)  # A cached success must not mask a failed refresh.
        fail_index = False
        again(1, '3.1')
    finally:
        client.stdin.close()
        client.wait(timeout=10)
        client.stdout.close(); client.stderr.close()
        server.shutdown(); server.server_close(); worker.join()

print(f'selfupdate: {checks} checks passed')
