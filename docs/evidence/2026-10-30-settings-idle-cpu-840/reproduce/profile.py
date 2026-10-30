#!/usr/bin/env python3
import argparse,hashlib,json,os,pathlib,subprocess,time
p=argparse.ArgumentParser();p.add_argument('bin',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);p.add_argument('--seconds',type=int,default=5);p.add_argument('--platform',default='offscreen');p.add_argument('--views',default='history,settings,history,settings');p.add_argument('--profile',action='store_true');p.add_argument('--accessibility',action='store_true');a=p.parse_args();a.bin=a.bin.resolve();a.output.mkdir(mode=0o700,parents=True,exist_ok=False)
root=a.output.resolve();env=os.environ.copy()
if a.platform=='wayland' and env.get('WAYLAND_DISPLAY') and not env['WAYLAND_DISPLAY'].startswith('/'):
 env['WAYLAND_DISPLAY']=str(pathlib.Path(env['XDG_RUNTIME_DIR'])/env['WAYLAND_DISPLAY'])
for name,part in [('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_STATE_HOME','state'),('XDG_CACHE_HOME','cache'),('XDG_RUNTIME_DIR','runtime')]:
 path=root/part;path.mkdir(mode=0o700);env[name]=str(path)
env['QT_QPA_PLATFORM']=a.platform
if a.accessibility:env['QT_LINUX_ACCESSIBILITY_ALWAYS_ON']='1'
else:env.pop('QT_LINUX_ACCESSIBILITY_ALWAYS_ON',None)
socket=root/'runtime/putgd/session.sock';assert len(str(socket).encode())<=107
with (root/'socket-preflight.json').open('w') as output:
 subprocess.run(['python3',str(pathlib.Path(__file__).with_name('840-check_socket_paths.py')),'--socket',str(socket)],stdout=output,check=True)
def cli(method,params={}):
 r=subprocess.run([str(a.bin/'putgd-cli'),'--socket',str(socket),method,json.dumps(params)],env=env,text=True,capture_output=True,timeout=20)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 return json.loads(r.stdout)
def ticks(pid):
 fields=pathlib.Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split();return int(fields[11])+int(fields[12])
def ui_pid():
 for path in pathlib.Path('/proc').glob('[0-9]*/stat'):
  try:
   text=path.read_text(); fields=text.rsplit(')',1)[1].split()
   if int(fields[1])==process.pid and '(putgd-ui)' in text:return int(path.parent.name)
  except (OSError,ValueError):pass
 return None
log=(root/'app.log').open('w');process=subprocess.Popen([str(a.bin/'putgd'),'--background'],env=env,stdout=log,stderr=log)
receipt={'placement':'boat','platform':a.platform,'accessibilityForced':a.accessibility,'resident':process.pid,'seconds':a.seconds,'binaries':{name:hashlib.sha256((a.bin/name).read_bytes()).hexdigest() for name in ['putgd','putgd-cli','putgd-ui']},'samples':[]}
try:
 deadline=time.monotonic()+30
 while not socket.exists():
  if process.poll() is not None:raise RuntimeError('resident exited')
  if time.monotonic()>deadline:raise TimeoutError('session')
  time.sleep(.1)
 receipt['build']=json.loads(subprocess.check_output([str(a.bin/'putgd-cli'),'--version-json'],text=True))
 for view in a.views.split(','):
  cli('ui.show',{'view':view});pid=ui_pid();assert pid
  (root/'shown.json').write_text(json.dumps({'view':view,'uiPid':pid,'wallTime':time.time()}))
  # Settle product startup and view transition before the timed sample.
  time.sleep(3)
  before=ticks(pid);start=time.monotonic();time.sleep(a.seconds);elapsed=time.monotonic()-start;delta=ticks(pid)-before
  sample={'view':view,'uiPid':pid,'ticks':delta,'elapsed':elapsed,'cpuPercent':100*delta/os.sysconf('SC_CLK_TCK')/elapsed};receipt['samples'].append(sample);print(json.dumps(sample),flush=True)
 if a.profile:
  pid=ui_pid();perf='/usr/bin/perf'
  choices=sorted(pathlib.Path('/usr/lib/linux-tools').glob('*/perf'))
  if choices:perf=str(choices[-1])
  cmd=['sudo','-n',perf,'record','-F','99','-g','-p',str(pid),'-o',str(root/'perf.data'),'--','sleep','8']
  perf_result=subprocess.run(cmd,text=True,capture_output=True,timeout=20);(root/'perf-record.log').write_text(perf_result.stdout+perf_result.stderr)
  if perf_result.returncode==0:
   r=subprocess.run(['sudo','-n',perf,'report','--stdio','--no-children','-i',str(root/'perf.data')],text=True,capture_output=True,timeout=25);(root/'perf-report.txt').write_text(r.stdout+r.stderr)
  else:
   script=root/'sample.gdb';script.write_text('set pagination off\nset print thread-events off\npython\nimport time,gdb\nfor i in range(30):\n gdb.execute("thread 1")\n gdb.execute("bt 18")\n gdb.execute("continue &")\n time.sleep(0.05)\n gdb.execute("interrupt")\nend\ndetach\nquit\n')
   r=subprocess.run(['sudo','-n','gdb','-q','-batch','-p',str(pid),'-x',str(script)],text=True,capture_output=True,timeout=30);(root/'gdb-samples.txt').write_text(r.stdout+r.stderr)
finally:
 try:cli('ui.quit')
 except Exception:process.terminate()
 try:process.wait(timeout=20)
 except subprocess.TimeoutExpired:process.kill();process.wait()
 (root/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');log.close()
