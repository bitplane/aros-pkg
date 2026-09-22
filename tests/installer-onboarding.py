# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import os, pathlib, subprocess, tempfile
repo=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='pkg-installer-check-') as d:
 p=pathlib.Path(d); (p/'bin').mkdir(); (p/'root with spaces').mkdir()
 fake=p/'binary';fake.write_text('''#!/bin/sh
if [ "$1" = HELP ]; then echo 'pkg 1.7.5+test'; exit 0; fi
printf '%s\\n' "$@" >> "$CALL_LOG"
''');fake.chmod(0o755)
 curl=p/'bin/curl';curl.write_text('#!/bin/sh\n[ "$1" = -fsSL ] && [ "$2" = -o ] || exit 1\ncp "$FAKE_PKG" "$3"\n');curl.chmod(0o755)
 base={**os.environ,'HOME':str(p),'PATH':str(p/'bin')+':'+os.environ['PATH'],'PKG_INSTALL_DIR':str(p/'install'),'PKG_SKIP_VERIFY':'1','PKG_NO_MODIFY_PATH':'1','PKG_NO_AROS':'1','FAKE_PKG':str(fake),'CALL_LOG':str(p/'calls')}
 for k in ('PKG_ENV_NAME','PKG_ENV_ROOT','PKG_ENV_DEFAULT','PKG_NO_ENV'):base.pop(k,None)
 script=str(repo/'portal/src/Portal/Install/install.sh')
 r=subprocess.run(['sh',script],env=base,capture_output=True,text=True);assert r.returncode==0,r.stderr;assert not (p/'calls').exists();print('PASS unattended no registration')
 r=subprocess.run(['sh',script],env={**base,'PKG_ENV_NAME':'native','PKG_ENV_ROOT':str(p/'root with spaces'),'PKG_ENV_DEFAULT':'1'},capture_output=True,text=True);assert r.returncode==0,r.stderr;assert (p/'calls').read_text().splitlines()==['ENV','ADD','native','ROOT',str(p/'root with spaces'),'ENV','DEFAULT','native'];assert 'environments.conf' in r.stdout;print('PASS explicit registration preserves spaces and default')
 (p/'calls').unlink()
 r=subprocess.run(['sh',script],env={**base,'PKG_ENV_NAME':'native'},capture_output=True,text=True);assert r.returncode!=0;assert not (p/'calls').exists();print('PASS partial configuration rejected')

with tempfile.TemporaryDirectory(prefix='pkg-existing-install-check-') as d:
 p=pathlib.Path(d);(p/'bin').mkdir();(p/'root').mkdir()
 existing=p/'bin/pkg';existing.write_text('#!/bin/sh\necho "pkg 1.7.4+test"\n');existing.chmod(0o755)
 curl=p/'bin/curl';curl.write_text('#!/bin/sh\ncp "$FAKE_PKG" "$3"\n');curl.chmod(0o755)
 fake=p/'binary';fake.write_text('#!/bin/sh\necho "pkg 1.7.5+test"\n');fake.chmod(0o755)
 env={**os.environ,'HOME':str(p),'PATH':str(p/'bin')+':'+os.environ['PATH'],'PKG_SKIP_VERIFY':'1','PKG_NO_MODIFY_PATH':'1','PKG_NO_AROS':'1','PKG_NO_ENV':'1','FAKE_PKG':str(fake)}
 env.pop('PKG_INSTALL_DIR',None)
 for k in ('PKG_ENV_NAME','PKG_ENV_ROOT','PKG_ENV_DEFAULT'):env.pop(k,None)
 r=subprocess.run(['sh',str(repo/'portal/src/Portal/Install/install.sh')],env=env,capture_output=True,text=True);assert r.returncode==0,r.stderr;assert '1.7.5' in existing.read_text();print('PASS lowercase pkg existing installation reused')

# The installer is normally piped to sh; prompts read the controlling terminal.
import pty, select, time
with tempfile.TemporaryDirectory(prefix='pkg-installer-tty-') as d:
 p=pathlib.Path(d); (p/'bin').mkdir()
 fake=p/'binary';fake.write_text('''#!/bin/sh
if [ "$1" = HELP ]; then echo 'pkg 1.7.5+test'; exit 0; fi
printf '%s\\n' "$@" >> "$CALL_LOG"
[ "$3" != taken ]
''');fake.chmod(0o755)
 curl=p/'bin/curl';curl.write_text('#!/bin/sh\ncp "$FAKE_PKG" "$3"\n');curl.chmod(0o755)
 env={**os.environ,'HOME':str(p),'PATH':str(p/'bin')+':'+os.environ['PATH'],'PKG_INSTALL_DIR':str(p/'install'),'PKG_SKIP_VERIFY':'1','PKG_NO_MODIFY_PATH':'1','PKG_NO_AROS':'1','FAKE_PKG':str(fake),'CALL_LOG':str(p/'calls')}
 for k in ('PKG_ENV_NAME','PKG_ENV_ROOT','PKG_ENV_DEFAULT','PKG_ENV_REUSE','PKG_NO_ENV'):env.pop(k,None)
 def interact(answers):
  pid,fd=pty.fork()
  if pid==0: os.execve('/bin/sh',['sh',str(repo/'portal/src/Portal/Install/install.sh')],env)
  output=b''; pending=b''; index=0; deadline=time.monotonic()+30
  try:
   while time.monotonic()<deadline:
    if not select.select([fd],[],[],0.2)[0]: continue
    try: chunk=os.read(fd,8192)
    except OSError: break
    if not chunk: break
    output+=chunk;pending+=chunk
    if index<len(answers) and answers[index][0].encode() in pending:
     os.write(fd,(answers[index][1]+'\n').encode());index+=1;pending=b''
   else: raise AssertionError(output.decode())
   _,status=os.waitpid(pid,0)
   assert os.waitstatus_to_exitcode(status)==0,output.decode()
   assert index==len(answers),(index,output.decode())
   return output.decode()
  finally: os.close(fd)
 interact([('Configure an environment?', 'y'),('Environment name [',''),('Package root [',''),('Use this environment by default?','')])
 assert (p/'AROS/System').is_dir()
 assert (p/'calls').read_text().splitlines()==['ENV','ADD','aros','ROOT',str(p/'AROS/System'),'ENV','DEFAULT','aros']
 print('PASS Return accepts defaults and creates root')
 occupied=p/'occupied';occupied.mkdir();(occupied/'.hidden').write_text('keep')
 out=interact([('Configure an environment?','maybe'),('Configure an environment?','y'),('Environment name [','bad name'),('Environment name [','test'),('Package root [','relative'),('Environment name [',''),('Package root [',str(occupied)),('Use this existing directory?',''),('Environment name [',''),('Package root [',str(p/'new root')),('Use this environment by default?','n')])
 assert (p/'new root').is_dir() and (occupied/'.hidden').read_text()=='keep'
 assert 'Enter an absolute path' in out
 print('PASS invalid entries retry, hidden contents require confirmation, decline chooses another root')
 interact([('Configure an environment?','y'),('Environment name [','taken'),('Package root [',str(p/'retry')),('Environment name [','good'),('Package root [',''),('Use this environment by default?','n')])
 print('PASS failed registration allows retry')
 interact([('Configure an environment?','y'),('Environment name [','reuse'),('Package root [',str(occupied)),('Use this existing directory?','y'),('Use this environment by default?','n')])
 assert (occupied/'.hidden').read_text()=='keep'
 print('PASS confirmed reuse preserves existing files')
 for reuse,code in [('0',1),('1',0)]:
  r=subprocess.run(['sh',str(repo/'portal/src/Portal/Install/install.sh')],env={**env,'PKG_ENV_NAME':'auto','PKG_ENV_ROOT':str(occupied),'PKG_ENV_REUSE':reuse},capture_output=True,text=True)
  assert r.returncode==code,(r.stdout,r.stderr)
 print('PASS unattended reuse needs explicit opt-in')

# A protected installation asks visibly before escalating; answering no leaves it intact.
if os.geteuid() != 0:
 import shutil
 with tempfile.TemporaryDirectory(prefix='pkg-prompt-contrast-') as d:
  directory=pathlib.Path(d)/'protected';directory.mkdir()
  binary=directory/'pkg';shutil.copy2(os.environ.get('PKG',repo/'build/pkg'),binary)
  directory.chmod(0o500)
  pid,fd=pty.fork()
  if pid==0: os.execve(str(binary),[str(binary),'UPGRADE'],{**os.environ,'PKG_COLOR':'always'})
  output=b'';sent=False;deadline=time.monotonic()+20
  try:
   while time.monotonic()<deadline:
    if not select.select([fd],[],[],0.2)[0]:continue
    try:chunk=os.read(fd,8192)
    except OSError:break
    if not chunk:break
    output+=chunk
    if b'[y/N]' in output and not sent:os.write(fd,b'n\n');sent=True
   else:raise AssertionError(output)
   _,status=os.waitpid(pid,0)
   assert os.waitstatus_to_exitcode(status)==17,output
   # The question is a question now, not a record: it carries the question
   # role, which is the brightest thing on the screen. What this check is
   # for is that it is never the dim ink of a figure, which is how it read
   # when it went out as "permissions: ..." under the detail role.
   question=next(line for line in output.splitlines() if b'[y/N]' in line)
   assert b'\x1b[2m' not in question,question
   assert b'administrator access is required' in question,question
   assert sent and binary.exists()
   print('PASS administrator question has normal contrast and declining preserves pkg')
  finally:
   directory.chmod(0o700);os.close(fd)
