import json, os, pathlib, signal, subprocess, sys, time
source, output = map(pathlib.Path, sys.argv[1:])
output.mkdir()
private = pathlib.Path('/tmp/pg393b')
private.mkdir(mode=0o700)
env = dict(os.environ, TMPDIR=str(private), XDG_CONFIG_HOME=str(private/'config'),
           XDG_DATA_HOME=str(private/'data'), XDG_STATE_HOME=str(private/'state'),
           XDG_CACHE_HOME=str(private/'cache'), XDG_RUNTIME_DIR=str(private/'runtime'),
           QT_QPA_PLATFORM='offscreen', PUTGD_TEST_TIME_SCALE='3')
for k in ('DISPLAY','WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS'): env.pop(k,None)
for d in ('config','data','state','cache','runtime'): (private/d).mkdir(mode=0o700)
preflight = subprocess.run(['python3', str(source/'tools/diagnostics/check_socket_paths.py'),
    '--socket', str(private/'test_operations-XXXXXX/runtime/putgd/session.sock')],capture_output=True,text=True,check=True)
(output/'socket-preflight.json').write_text(preflight.stdout)
hog = subprocess.Popen(['taskset','-c','0','python3','-c','while True: pass'],stdout=subprocess.DEVNULL)
started=time.monotonic(); runs=[]
try:
    for index in range(1,101):
        result=subprocess.run(['taskset','-c','0',str(source/'build/dev/test_operations'),
            'resumableUploadJournalSurvivesForRetry'],env=env,capture_output=True,text=True,timeout=40)
        (output/f'{index:03d}.log').write_text(result.stdout+result.stderr)
        runs.append({'iteration':index,'exitCode':result.returncode})
        if result.returncode or time.monotonic()-started>180: break
finally:
    hog.terminate();hog.wait()
    receipt={'runs':runs,'seconds':round(time.monotonic()-started,2),'hogs':1,'hogCpus':'0','testCpus':'0',
      'timeScale':3,'source':'50053b01f1bbd7f3776a53513bb8c43151ce0de8',
      'testHash':subprocess.check_output(['sha256sum',str(source/'build/dev/test_operations')],text=True).split()[0]}
    (output/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))
