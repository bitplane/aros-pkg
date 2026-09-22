#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Signed local bootstrap: replacement, DRYRUN, tampering and rollback refusal."""
import functools
import hashlib
import http.server
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import threading

repo = Path(__file__).resolve().parent.parent
pkg = str(Path(os.environ.get('PKG', repo / 'build/pkg')).resolve())

def run(*args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, **kw)

with tempfile.TemporaryDirectory(prefix='pkg-selfupdate-test-') as d:
    root = Path(d)
    key = root / 'key'
    run(pkg, 'KEYGEN', 'FILE', key, check=True)
    public = next(x.split(': ', 1)[1] for x in key.read_text().splitlines() if x.startswith('Public:'))
    cpu = 'arm64' if platform.machine() in ('arm64', 'aarch64') else 'x86_64'
    system = 'macos' if platform.system() == 'Darwin' else 'linux'
    directory = root / 'Bootstrap' / f'{system}-{cpu}'
    directory.mkdir(parents=True)
    sums = root / 'Bootstrap/SHA256SUMS'
    sig = root / 'Bootstrap/SHA256SUMS.sig'
    binary = directory / 'pkg'
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args): pass
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=str(root)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        probe = root / 'probe.c'
        probe.write_text('''#include "pkg_selfupdate.h"
#include <stdio.h>
#include <string.h>
const char cookie[]="$VER: pkg " PKG_VERSION_STRING " (22.09.2026)";
static void record(void *u,const char *k,const char *v) {(void)u;printf("%s: %s\\n",k,v);}
int main(int argc,char **argv) {struct pkg_sink s;memset(&s,0,sizeof s);s.structured=1;s.record=record;
if(argc>1&&!strcmp(argv[1],"VERSION")){puts(cookie);return 0;}
if(argc>1&&!strcmp(argv[1],"REMOVE"))return pkg_selfremove(&s,argc>2&&!strcmp(argv[2],"DRYRUN"));
return pkg_selfupdate(&s,argc>1&&!strcmp(argv[1],"DRYRUN"));}
''')
        base = ['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I'+str(repo/'include'),
                '-DPKG_BUILD="20260922"', '-DPKG_SELFUPDATE_KEY="'+public+'"',
                f'-DPKG_SELFUPDATE_CHANNEL="http://127.0.0.1:{server.server_port}"',
                str(probe), str(repo/'src/pkg_selfupdate.c'), str(repo/'build/libpkg.a')]
        old = root/'old'; new = root/'new'; older = root/'older'
        run(*base, '-DPKG_VERSION_PATCH="0"', '-o', old, check=True)
        run(*base, '-DPKG_VERSION_PATCH="5"', '-o', new, check=True)
        olderbase=[x if x != '-DPKG_BUILD="20260922"' else '-DPKG_BUILD="20260921"' for x in base]
        run(*olderbase, '-DPKG_VERSION_PATCH="0"', '-o', older, check=True)
        def offer(data, namespace='aros-pkg-bootstrap', signing=key):
            binary.write_bytes(data)
            sums.write_text(hashlib.sha256(data).hexdigest()+'  '+str(binary.relative_to(root))+'\n')
            run(pkg, 'SIGN', sums, 'KEY', signing, 'OUT', sig, 'SSH', 'NAMESPACE', namespace, check=True)
        target = root/'running pkg'
        alias = root/'alias'
        alias.symlink_to(target)
        baseline=old.read_bytes(); newer=new.read_bytes()
        checks=0
        def check(expected, args=(), unchanged=True):
            global checks
            shutil.copy2(old,target)
            result=run(alias,*args)
            assert result.returncode==expected,(result.returncode,result.stdout,result.stderr)
            assert target.read_bytes()==(baseline if unchanged else newer),result.stdout
            assert not list(root.glob('.pkg-selfupdate.*'))
            checks+=1
            return result.stdout
        offer(newer)
        assert 'DRYRUN' in check(0,('DRYRUN',))
        check(0,unchanged=False)
        # Exact running path through a symlink, with a space, was replaced.
        offer(baseline)
        assert 'up to date' in check(0)
        offer(older.read_bytes())
        assert 'up to date' in check(0)
        offer(newer)
        binary.write_bytes(newer+b'bad')
        check(13)
        offer(newer)
        sums.write_text(sums.read_text()+'altered\n')
        check(13)
        offer(newer,namespace='another-purpose')
        check(13)
        other=root/'other-key';run(pkg,'KEYGEN','FILE',other,check=True)
        offer(newer,signing=other)
        check(13)
        offer(b'not a program')
        check(13)
        offer(newer)
        sums.write_text(sums.read_text()*2)
        run(pkg,'SIGN',sums,'KEY',key,'OUT',sig,'SSH','NAMESPACE','aros-pkg-bootstrap',check=True)
        check(13)
        sig.unlink()
        check(16)
        # A managed executable must update its owning record through libpkg.
        drawer=root/'drawer';(drawer/'bin').mkdir(parents=True)
        shutil.copy2(old,drawer/'bin/pkg')
        managed=root/'managed'
        run(pkg,'PUBLISH',drawer,'NAME','pkg','VERSION','1.7.0+20260922','KIND','application',
            'ARCH','any','CHANNEL',root,'SIGN',key,check=True)
        run(pkg,'INSTALL','pkg','ROOT',managed,'CHANNEL',root,check=True)
        shutil.copy2(new,drawer/'bin/pkg')
        run(pkg,'PUBLISH',drawer,'NAME','pkg','VERSION','1.7.5+20260922','KIND','application',
            'ARCH','any','CHANNEL',root,'SIGN',key,check=True)
        result=run(managed/'bin/pkg')
        assert result.returncode==0,(result.stdout,result.stderr)
        assert 'root-source: installed package record' in result.stdout,result.stdout
        assert (managed/'bin/pkg').read_bytes()==newer
        run(pkg,'VERIFY','pkg','ROOT',managed,check=True)
        assert '1.7.5+20260922' in (managed/'.pkg/db/pkg').read_text()
        checks+=1
        # Standalone removal resolves the running symlink, preserving its link,
        # unrelated files and configuration.
        shutil.copy2(old,target)
        unrelated=root/'unrelated';unrelated.write_text('keep me')
        config=root/'environments.conf';config.write_text('keep configuration')
        result=run(alias,'REMOVE','DRYRUN')
        assert result.returncode==0 and target.read_bytes()==baseline,(result.stdout,result.stderr)
        checks+=1
        result=run(alias,'REMOVE')
        assert result.returncode==0 and not target.exists(),(result.stdout,result.stderr)
        assert alias.is_symlink() and unrelated.read_text()=='keep me'
        assert config.read_text()=='keep configuration'
        checks+=1
        # Removing managed pkg keeps a separately installed program and database.
        extra=root/'extra';extra.mkdir();(extra/'other.txt').write_text('other application')
        run(pkg,'PUBLISH',extra,'NAME','other','VERSION','1.0','KIND','data',
            'CHANNEL',root,'SIGN',key,check=True)
        run(pkg,'INSTALL','other','ROOT',managed,'CHANNEL',root,check=True)
        result=run(managed/'bin/pkg','REMOVE','DRYRUN')
        assert result.returncode==0 and (managed/'bin/pkg').exists(),(result.stdout,result.stderr)
        assert (managed/'.pkg/db/pkg').exists()
        checks+=1
        result=run(managed/'bin/pkg','REMOVE')
        assert result.returncode==0,(result.stdout,result.stderr)
        assert not (managed/'bin/pkg').exists() and not (managed/'.pkg/db/pkg').exists()
        assert (managed/'.pkg/db/other').exists() and (managed/'other.txt').read_text()=='other application'
        run(pkg,'VERIFY','other','ROOT',managed,check=True)
        checks+=1
        print(f'selfupdate-bootstrap: {checks} checks passed')
    finally:
        server.shutdown();server.server_close()
