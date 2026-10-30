"""Synthetic guest clipboard owners with tool and ownership preflight."""
import shlex

from ..api import RecipeBlocked


def tool(scope, name):
    found=scope.ctx.shell('command -v '+shlex.quote(name))
    if found['returncode']==0 and found['stdout'].strip():
        return found['stdout'].strip()
    package='wl-clipboard' if name.startswith('wl-') else 'xclip'
    root=scope.root+'/probe-'+package
    # Extract into the owned overlay directory, without installing packages or
    # changing the sealed golden. Existing system shared libraries are reused.
    result=scope.ctx.shell('mkdir -m 700 -p '+shlex.quote(root)+' && cd '+shlex.quote(root)+
        ' && apt-get download '+package+' >download.log 2>&1 && '+
        'for deb in ./*.deb; do dpkg-deb --extract "$deb" .; done',timeout=60)
    executable=root+'/usr/bin/'+name
    if result['returncode'] or scope.ctx.shell('test -x '+shlex.quote(executable))['returncode']:
        raise RecipeBlocked('clipboard requires '+name+'; overlay probe-tool staging unavailable',issue=682)
    return executable


def owner(scope, path, mime, *, name):
    ctx=scope.ctx
    if ctx.placement not in ('guest','boat') or not ctx.profile.get('guest'):
        raise RecipeBlocked('clipboard owner requires a disposable nested guest',issue=682)
    scope.initialize()
    wayland=ctx.profile['session']=='wayland'
    executable=tool(scope,'wl-copy' if wayland else 'xclip')
    reader=tool(scope,'wl-paste') if wayland else executable
    args=[executable,'--foreground','--type',mime] if wayland else [executable,'-quiet','-selection','clipboard','-target',mime]
    code='import os\nf=os.open('+repr(path)+',os.O_RDONLY);os.dup2(f,0)\nos.execv('+repr(executable)+','+repr(args)+')'
    process=scope.launch(['python3','-c',code],name=name)
    read=[reader,'--no-newline','--type',mime] if wayland else [reader,'-selection','clipboard','-out','-target',mime]
    def ready():
        result=ctx.shell("python3 - <<'SD_CLIPBOARD'\n"+'''import subprocess
from pathlib import Path
p=Path('/proc')/str(PROCESS['pid'])/'stat'
fields=p.read_text().split(') ',1)[1].split()
assert fields[19]==PROCESS['start'] and fields[0]!='Z'
reply=subprocess.run(READ,capture_output=True,timeout=1)
assert reply.returncode==0 and reply.stdout==Path(PATH).read_bytes()
'''.replace('PROCESS',repr(process)).replace('READ',repr(read)).replace('PATH',repr(path))+"\nSD_CLIPBOARD\n",timeout=3)
        return result['returncode']==0
    try:
        ctx.wait(ready,'native synthetic clipboard owner ready',timeout=5)
    except Exception as error:
        raise RecipeBlocked('clipboard '+executable.rsplit('/',1)[-1]+' owner failed to start or acquire selection',issue=682) from error
    return process
