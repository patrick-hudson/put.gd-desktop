import sys,subprocess,pathlib,json,datetime
sys.path.insert(0,'/home/patrick/work/put.gd/tools/lab')
import lab
out=pathlib.Path('/home/patrick/work/put.gd/work/cross-desktop-current/kde'); out.mkdir(exist_ok=True)
script='''set -eu
cd /home/lab/putgd-current-a3d70dc
python3 tools/verify_dependencies.py /home/lab/putgd-native/build/dependencies/aws-sdk-cpp
cmake --preset dev -DCMAKE_PREFIX_PATH=/home/lab/putgd-native/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-native/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-native/build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build --preset dev
ctest --preset dev --output-junit /home/lab/putgd-current-a3d70dc/dev-tests.xml
cmake --preset release -DCMAKE_PREFIX_PATH=/home/lab/putgd-native/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-native/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-native/build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G DEB -B build/packages
sha256sum build/release/putgd build/release/putgd-cli build/release/libputgd-s3.so build/packages/*.deb
'''
(out/'build-script.sh').write_text(script)
cmd=lab.ssh_args('/home/patrick/work/put.gd/work/lab-runs/kde-001')+['sh','-s']
start=datetime.datetime.now(datetime.timezone.utc).isoformat()
with (out/'build-tests.log').open('w') as log:
 p=subprocess.run(cmd,input=script,text=True,stdout=log,stderr=subprocess.STDOUT)
(out/'build-result.json').write_text(json.dumps({'sourceCommit':'a3d70dc2a6c6866fc6e38dd5e001553fb5c157a2','archiveSha256':'ec179214a662010c7f6d21a2cc5fe29479423ff6a6e9292433ef35b0aebd3fab','startedUtc':start,'finishedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'exitCode':p.returncode,'command':cmd},indent=2)+'\n')
print('KDE build exit',p.returncode)
raise SystemExit(p.returncode)
