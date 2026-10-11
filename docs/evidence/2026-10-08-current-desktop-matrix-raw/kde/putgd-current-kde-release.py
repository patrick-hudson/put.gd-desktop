import sys,subprocess,pathlib,json,datetime
sys.path.insert(0,'/home/patrick/work/put.gd/tools/lab');import lab
out=pathlib.Path('/home/patrick/work/put.gd/work/cross-desktop-current/kde')
script='''set -eu
cd /home/lab/putgd-current-a3d70dc
cmake --build --preset release --target putgd putgd-cli putgd_s3_module putgd-desktop-fixture
cpack --config build/release/CPackConfig.cmake -G DEB -B build/packages
sha256sum build/release/putgd build/release/putgd-cli build/release/libputgd-s3.so build/packages/*.deb
'''
(out/'release-package-script.sh').write_text(script)
cmd=lab.ssh_args('/home/patrick/work/put.gd/work/lab-runs/kde-001')+['sh','-s']
start=datetime.datetime.now(datetime.timezone.utc).isoformat()
with (out/'release-package.log').open('w') as log:
 p=subprocess.run(cmd,input=script,text=True,stdout=log,stderr=subprocess.STDOUT)
(out/'release-package-result.json').write_text(json.dumps({'sourceCommit':'a3d70dc2a6c6866fc6e38dd5e001553fb5c157a2','startedUtc':start,'finishedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'exitCode':p.returncode,'command':cmd,'reason':'Resume Release package targets after requested interruption of duplicate Release test compilation. Full Dev28/28 already passed.'},indent=2)+'\n')
print('KDE Release package exit',p.returncode)
raise SystemExit(p.returncode)
