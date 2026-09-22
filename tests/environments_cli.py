# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Root selection and configuration through the public CLI."""
import os
import pty
from pathlib import Path
import subprocess
import tempfile

pkg = str(Path(os.environ.get('PKG', './build/pkg')).resolve())
with tempfile.TemporaryDirectory(prefix='pkg-environments-cli-') as tmp:
    base = Path(tmp).resolve()
    env = dict(os.environ, HOME=str(base), XDG_CONFIG_HOME=str(base / 'config'))
    config = base / 'config/aros-pkg/environments.conf'
    first, second = base / 'first root', base / 'second'
    first.mkdir()
    second.mkdir()
    def interactive(reply):
        master, slave = pty.openpty()
        proc = subprocess.Popen([pkg, 'LIST'], env=env, stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        os.write(master, (reply + '\n').encode())
        out = b''
        while True:
            try:
                chunk = os.read(master, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
        os.close(master)
        assert proc.wait(timeout=5) == 0, out
        return out.decode()
    def run(*args, ok=True):
        p = subprocess.run([pkg, *map(str, args)], env=env, capture_output=True, text=True)
        assert (p.returncode == 0) == ok, (args, p.returncode, p.stdout, p.stderr)
        return p.stdout + p.stderr
    assert 'no environment is registered' in run('ENV', 'LIST')
    assert not config.exists()
    assert 'command line (ROOT)' in run('LIST', 'ROOT', first)
    assert not config.exists()
    run('LIST', ok=False)
    assert 'interactive choice' in interactive(str(first))
    assert not config.exists()
    run('ENV', 'ADD', 'native', 'ROOT', first)
    assert str(config) in run('ENV', 'LIST')
    assert str(first) in run('LIST')
    run('ENV', 'ADD', 'other', 'ROOT', second)
    run('LIST', ok=False)
    before = config.read_bytes()
    assert 'Which root is this operation for' in interactive('2')
    assert before == config.read_bytes()
    assert str(second) in run('LIST', 'ENVIRONMENT', 'other')
    run('ENV', 'DEFAULT', 'native')
    assert str(first) in run('LIST')
    assert str(second) in run('LIST', 'ROOT', second, 'ENVIRONMENT', 'native')
    assert 'selected-root' in run('LIST', 'MACHINE')
    before = config.read_bytes()
    run('ENV', 'ADD', 'missing', 'ROOT', base / 'absent', ok=False)
    assert before == config.read_bytes()
    first.rmdir()
    run('LIST', ok=False)
    assert not first.exists()
    run('ENV', 'REMOVE', 'native')
    assert str(second) in run('LIST')
    config.write_text('invalid configuration\n')
    run('LIST', ok=False)
    assert 'command line (ROOT)' in run('LIST', 'ROOT', second)
print('environments CLI: PASS')
