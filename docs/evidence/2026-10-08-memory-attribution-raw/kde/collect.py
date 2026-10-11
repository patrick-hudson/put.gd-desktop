import pathlib,sys,json,subprocess,datetime,hashlib
root=pathlib.Path('/home/patrick/work/put.gd');sys.path.insert(0,str(root/'tools/lab'));import lab
o=root/'work/memory-next/kde';script=o/'guest-attribution.py'
command=lab.ssh_args(root/'work/lab-runs/current-kde-a3d70dc')+['python3','-']
r=subprocess.run(command,input=script.read_text(),text=True,capture_output=True,timeout=35)
out={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'command':command,'scriptSha256':hashlib.sha256(script.read_bytes()).hexdigest(),'exitCode':r.returncode,'scope':'Read-only current KDE app mapping attribution. No GUI/build/restart, credentials/config/environment/payloads, or renderer assumption. VNC unchanged.'}
if r.returncode==0:out['guest']=json.loads(r.stdout)
else:out['error']=r.stderr[:2000]
(o/'attribution.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({'exitCode':r.returncode,'processes':[{'pid':x['pid'],'rollup':x['smapsRollup'],'families':x['loadedFamilies']} for x in out.get('guest',{}).get('processes',[])]},indent=2))
raise SystemExit(r.returncode)
