#!/usr/bin/env python3
import json
from common import ROOT,OUT,RUN,now,save,sha,source,load_lab,assert_resident
lab=load_lab();m=source();expected=assert_resident(lab)
original_command=lab.command
def capture_command(args,timeout=60,check=True,input=None):
 result=original_command(args,timeout=timeout,check=False,input=input)
 if result.returncode:
  (OUT/'bridge-identity-command-error.log').write_text(result.stderr)
  raise RuntimeError('Read-only bridge metadata command failed; owned diagnostic saved')
 return result
lab.command=capture_command
script="""python3 - <<'GUEST'
import json,subprocess,pathlib,hashlib,os
def call(*args):return json.loads(subprocess.check_output(['busctl','--user','--json=short',*args],text=True))
out={'services':{}}
for name in ['org.kde.KWin','org.kde.KWin.ScreenShot2','org.kde.kglobalaccel']:
 owner=call('call','org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetNameOwner','s',name)['data'][0]
 pid=call('call','org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetConnectionUnixProcessID','s',owner)['data'][0]
 p=pathlib.Path('/proc')/str(pid);a=(p/'stat').read_text().rsplit(')',1)[1].split();item={'owner':owner,'pid':pid,'startTicks':int(a[19]),'processName':(p/'comm').read_text().strip()}
 try:item.update(executable=str((p/'exe').readlink()),sha256=hashlib.sha256((p/'exe').read_bytes()).hexdigest(),liveExecutableReadable=True)
 except PermissionError:
  item.update(liveExecutableReadable=False,limitation='Kernel denies /proc compositor executable inspection to this user; no live compositor ELF hash claimed')
  if item['processName']=='kwin_wayland':item.update(installedExecutable='/usr/bin/kwin_wayland',installedExecutableSha256=hashlib.sha256(pathlib.Path('/usr/bin/kwin_wayland').read_bytes()).hexdigest())
 out['services'][name]=item
out['screenShot2Version']=call('get-property','org.kde.KWin.ScreenShot2','/org/kde/KWin/ScreenShot2','org.kde.KWin.ScreenShot2','Version')
p=pathlib.Path('/usr/share/applications/putgd.desktop');out['restrictedDesktopEntry']={'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
out['kwinPackage']=subprocess.check_output(['dpkg-query','-W','-f=${Package} ${Version}','kwin-wayland'],text=True).strip()
print(json.dumps(out))
GUEST
"""
observed=json.loads(lab.guest_session(RUN,script));cap=lab.cli(RUN,'capabilities')['source'];assert cap['adapter']=='kde-wayland';assert cap['capture']['window']['available'] and cap['capture']['window']['permission_entry_matches_executable'] and not cap['capture']['window']['permission_rejected'];assert assert_resident(lab)==expected
paths=['src/platform/kde/KdeSourceAdapter.cpp','src/platform/kde/KWinCapture.cpp','src/platform/kde/KdeShortcuts.cpp','src/platform/kde/DataControlClipboard.cpp','src/platform/kde/KdeRegionOverlay.cpp','packaging/linux/app.desktop.in']
for path in paths:assert sha(ROOT/path)==m['files'][path]['sha256']
package=json.loads((OUT/'package-files.json').read_text());assert observed['restrictedDesktopEntry']['sha256']==package['installedFiles']['/usr/share/applications/putgd.desktop']
save(OUT/'bridge-identity.json',{'schemaVersion':1,'timestampUtc':now(),'sourceCommit':m['commit'],'resident':expected,'integration':'Compiled resident adapter; no put.gd KWin extension/script is installed or loaded','adapterSourceSha256':{path:sha(ROOT/path) for path in paths},'actualServices':observed,'capabilities':cap,'activationEvidence':'Current source compiled into verified running resident; actual native operation IDs are in matrix.json and tray-matrix.json','script':script,'ok':True})
print(json.dumps({'ok':True,'resident':expected,'kwin':observed['services']['org.kde.KWin'],'interfaceVersion':observed['screenShot2Version']}))
