"""Real update checks/install lifecycle; never retag or sign application bytes."""
import json
from pathlib import Path
import shlex

from ..api import RecipeBlocked
from .base import Fixture


def feed(scope, older=False):
    ctx=scope.ctx
    ctx.fixture('app-profile')
    root=ctx.target.root+'/state/feed-'+scope.token[:8]
    scope.python('from pathlib import Path\nPath('+repr(root)+').mkdir(mode=0o700)')
    scope.defer('update feed directory',lambda:scope.python('import shutil\nshutil.rmtree('+repr(root)+')'))
    scope.write(scope.root+'/update_server.py',Path(__file__).with_name('update_server.py').read_bytes())
    scope.launch(['python3',scope.root+'/update_server.py',root],name='feed')
    def port():
        result=ctx.shell('cat '+shlex.quote(root+'/port')+' 2>/dev/null')
        return int(result['stdout']) if result['stdout'].strip().isdigit() else None
    port=ctx.wait(port,'guest update feed',timeout=10)
    url='http://127.0.0.1:'+str(port)+'/'
    scope.restart({'PUTGD_UPDATE_FEED_URL':url,'PUTGD_UPDATE_RELEASE_URL':url+'release',
                   'PUTGD_UPDATE_MIRROR_URL':url+'mirror'})
    status=ctx.call('update.status')
    channel=status.get('channel') or 'stable'
    version=('26.101.1' if older else '99.1231.999')+('-nightly' if channel=='nightly' else '')
    def publish(document):
        if document.get('schema')!=1 or document.get('channel') not in ('nightly','stable'):
            raise ValueError('publish needs a schema-1 stable/nightly update feed')
        scope.python('''import json,os,tempfile
from pathlib import Path
p=Path(ROOT)/CHANNEL
fd,name=tempfile.mkstemp(dir=p.parent,prefix='.feed-')
with os.fdopen(fd,'w') as out:json.dump(DOCUMENT,out);out.flush();os.fsync(out.fileno())
os.replace(name,p)
'''.replace('ROOT',repr(root)).replace('CHANNEL',repr(document['channel']+'.json')).replace('DOCUMENT',repr(document)))
    publish({'schema':1,'channel':channel,'release':{'version':version,'tag':'v'+version,'assets':{}}})
    def ledger():
        return scope.json('import json,urllib.request\nprint(urllib.request.urlopen('+repr(url+'__ledger')+',timeout=5).read().decode())')
    def resident():
        return scope.json(r'''import json,os,socket,struct
from pathlib import Path
with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as peer:
 peer.connect(__SOCKET__)
 pid,uid,gid=struct.unpack('3i',peer.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
assert uid==os.getuid() and pid>1
p=Path('/proc')/str(pid)
env=dict(v.split(b'=',1) for v in (p/'environ').read_bytes().split(b'\0') if b'=' in v)
assert env[b'PUTGD_UPDATE_FEED_URL'].decode()==__URL__
print(json.dumps({'pid':pid,'start':(p/'stat').read_text().split(') ',1)[1].split()[19]}))
'''.replace('__SOCKET__',repr(ctx.target.socket)).replace('__URL__',repr(url)))
    identity=resident()
    return Fixture(url=url,root=root,port=port,residentPid=identity['pid'],resident=resident,version=version,
                   channel=channel,supported=status.get('supported',status.get('build',{}).get('supported',False)),
                   publish=publish,ledger=ledger,isolated=True,loopback=True)


def lifecycle(scope):
    """Use pre-staged authentic signed releases, never development substitutes.

    The profile's updates.bundle names a target-side manifest with `current`
    (path, sha256, build) and `releases` (channel, version, assets). Each asset
    has path, sha256 and name. build is the actual --version-json output.
    Release directories include the genuine SHA256SUMS and its minisig. The
    production updater validates its compiled release key during install.
    """
    ctx=scope.ctx
    bundle=ctx.profile.get('updates',{}).get('bundle')
    if not bundle:
        raise RecipeBlocked('signed update lifecycle requires updates.bundle with authentic tagged AppImage releases and SHA256SUMS.minisig; development bytes are not eligible',issue=682)
    ctx.fixture('app-profile')
    # Validate on target; never transfer package bytes or a signing secret to
    # the controller. All immutable originals remain untouched.
    manifest=scope.json('''import hashlib,json,os,re,stat,subprocess
from pathlib import Path
p=Path(BUNDLE);s=p.lstat()
assert p.resolve()==p and stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid()
doc=json.loads(p.read_text())
for item in [doc['current']]+[a for release in doc['releases'] for a in release['assets']]:
 f=Path(item['path']);s=f.lstat()
 assert f.resolve()==f and stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid()
 with f.open('rb') as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==item['sha256']
current=doc['current'];build=json.loads(subprocess.check_output([current['path'],'--version-json'],text=True,timeout=30))
assert build==current['build'] and not build['dirty'] and current['tag']=='v'+build['version']
assert re.fullmatch(r'[2-9][0-9]\\.[1-9][0-9]{2,3}\\.[1-9][0-9]{0,8}(-nightly)?',build['version'])
assert current['path'].endswith('.AppImage')
print(json.dumps(doc))
'''.replace('BUNDLE',repr(bundle)),timeout=90)
    image=scope.root+'/current.AppImage'
    scope.python('import shutil,os\nshutil.copyfile('+repr(manifest['current']['path'])+','+repr(image)+');os.chmod('+repr(image)+',0o700)')
    original=ctx.target.app_executable
    def restore():
        ctx.target.app_executable=original
        scope.restart({'APPIMAGE':''})
    scope.defer('original resident executable',restore,priority=-450)
    ctx.target.app_executable=image
    fixture=ctx.fixture('events.update-feed')
    releases=[]
    for release in manifest['releases']:
        channel,version=release['channel'],release['version']
        # No filenames from the manifest may escape the owned release tree.
        if channel not in ('stable','nightly') or version.startswith('v') or not version or '/' in version or '..' in version:
            raise ValueError('invalid release identity')
        assets={}
        names={asset['name'] for asset in release['assets']}
        if not {'SHA256SUMS','SHA256SUMS.minisig'}.issubset(names):
            raise RecipeBlocked('signed release bundle is missing checksum/signature files',issue=682)
        for asset in release['assets']:
            name=asset['name']
            if Path(name).name!=name or name in ('.','..'):
                raise ValueError('invalid release asset name')
            target=fixture.root+'/release/v'+version+'/'+name
            mirror=fixture.root+'/mirror/v'+version+'/'+name
            scope.python('''import shutil
from pathlib import Path
for target in TARGETS:
 p=Path(target);p.parent.mkdir(mode=0o700,parents=True,exist_ok=True);shutil.copyfile(SOURCE,p)
'''.replace('TARGETS',repr([target,mirror])).replace('SOURCE',repr(asset['path'])))
            if name.endswith('.AppImage'):
                size=scope.json('import json,os\nprint(json.dumps(os.stat('+repr(target)+').st_size))')
                assets['appimage']={'name':name,'sha256':asset['sha256'],'size':size,
                    'url':'https://github.com/patrick-hudson/put.gd-desktop/releases/download/v'+version+'/'+name}
        document={'schema':1,'channel':channel,'release':{'version':version,'tag':'v'+version,'assets':assets}}
        fixture.publish(document);releases.append(document)
    return Fixture(fixture,image=image,releases=releases,build=manifest['current']['build'],
                   install=lambda **params:ctx.call('update.install',params),
                   restart=lambda **params:ctx.call('update.restart',params))
