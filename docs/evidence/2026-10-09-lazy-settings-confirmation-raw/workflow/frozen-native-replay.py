#!/usr/bin/env python3
"""Incremental clean checkpoint replay. Each phase is explicit; help is inert."""
from pathlib import Path
from types import SimpleNamespace
import argparse, hashlib, json, re, shlex, subprocess, sys, tarfile

ROOT = Path('/home/patrick/work/put.gd')
sys.path.insert(0, str(ROOT / 'work/ui-refinement/native-replay'))
import replay

PREVIOUS = replay.HERE / 'lazy-exp001-native'
WORKTREE = ROOT / 'work/view-startup-latency/exp001'
PATHS = ('src/ui/SettingsWindow.cpp', 'src/ui/SettingsWindow.h',
         'tests/gui/test_settings.cpp', 'tests/gui/test_ui_application.cpp')
REMOTE_CACHE = '/home/lab/putgd-ui-refinement-lazy-exp001-native/source'
IDENTITY_TESTS = '^(version_contract|version_integration|gui_version|gui_tray|ui_split_services_integration)$'


def git(*args):
    return subprocess.check_output(['git', *args], cwd=WORKTREE, text=True).strip()


def verify_tree(src, files):
    for name, item in files.items():
        p = src / name
        if item['type'] == 'file':
            assert p.is_file() and not p.is_symlink(), name
            assert replay.sha(p) == item['sha256'], name
            assert p.stat().st_mode & 0o777 == item['mode'], name
        else:
            assert p.is_symlink() and str(p.readlink()) == item['target'], name


def prepare(r):
    assert not (r.out / 'source.json').exists(), 'Use a fresh cohort'
    previous = replay.read(PREVIOUS / 'source.json')
    assert previous['candidate']['dirty'] is True
    assert replay.sha(previous['candidate']['patch']) == previous['candidate']['patchSha256']
    assert git('rev-parse', 'HEAD') == r.args.commit
    assert re.fullmatch('[0-9a-f]{40}', r.args.commit)
    assert git('status', '--porcelain', '--untracked-files=normal') == '', 'Checkpoint must be clean'
    assert git('rev-parse', 'HEAD^') == previous['commit'], 'Expected one checkpoint after baseline45'
    assert sorted(git('diff', '--name-only', 'HEAD^', 'HEAD').splitlines()) == sorted(PATHS)
    for name in PATHS:
        assert replay.sha(WORKTREE / name) == previous['candidate']['files'][name], name
    tree = git('rev-parse', 'HEAD^{tree}')
    count = int(git('rev-list', '--count', 'HEAD'))
    assert count == previous['commitCount'] + 1
    archive = r.out / 'source.tar'
    bundle = r.out / 'source.bundle'
    r.command(['git', 'archive', '--format=tar', '-o', str(archive), r.args.commit], cwd=WORKTREE)
    r.command(['git', 'bundle', 'create', str(bundle), 'HEAD'], cwd=WORKTREE, timeout=120)
    files = {}
    with tarfile.open(archive) as t:
        for m in t.getmembers():
            assert not m.name.startswith(('work/', '.aws/', '.context/'))
            assert Path(m.name).name != 'credentials.txt'
            if m.isfile():
                files[m.name] = {'type': 'file', 'mode': m.mode,
                                'sha256': hashlib.sha256(t.extractfile(m).read()).hexdigest()}
            elif m.issym():
                files[m.name] = {'type': 'symlink', 'target': m.linkname}
    assert files == previous['files'], 'Checkpoint must preserve every reviewed candidate source byte/mode'
    verify_tree(WORKTREE, files)
    source = {'schemaVersion': 1, 'utc': replay.now(), 'commit': r.args.commit,
              'tree': tree, 'commitCount': count, 'archive': str(archive),
              'archiveSha256': replay.sha(archive), 'files': files,
              'frozenCandidate': {'worktree': str(WORKTREE), 'bundle': str(bundle),
                                  'bundleSha256': replay.sha(bundle), 'dirty': False,
                                  'previousCohort': str(PREVIOUS),
                                  'previousSourceSha256': replay.sha(PREVIOUS / 'source.json'),
                                  'reviewedPatchSha256': previous['candidate']['patchSha256'],
                                  'files': previous['candidate']['files'],
                                  'guestSourceCache': REMOTE_CACHE}}
    replay.save(r.out / 'source.json', source)
    # Deliberately read-only. Requires all helpers closed and operations finished.
    inventory = {'guests': {}}
    for name in replay.GUESTS:
        old = replay.Replay(SimpleNamespace(output=str(PREVIOUS), guest=name,
                                            phase='frozen-checkpoint-readonly-inventory'))
        resident, snapshot = old.assert_current()
        assert not snapshot['diagnostics']['ui'].get('pid')
        assert old.cli('diagnostics')['activeOperations'] == 0
        inventory['guests'][name] = {'guest': {'processes': [{'comm': 'putgd', **resident}],
                                             'manifest': snapshot['build']}}
    replay.save(r.out / 'preparation-inventory.json', inventory)
    replay.save(r.out / 'prepared.json', {'utc': replay.now(), 'sourceCommit': r.args.commit,
                                        'sourceFilesVerified': len(files), 'dirty': False,
                                        'nativeAcceptance': 'pending', 'existingOverlaysOnly': True})


def source(r):
    s = r.source()
    c = s['frozenCandidate']
    assert c['dirty'] is False and replay.sha(c['bundle']) == c['bundleSha256']
    assert replay.sha(PREVIOUS / 'source.json') == c['previousSourceSha256']
    return s


def manifest_ok(manifest, s):
    assert manifest['gitCommit'] == s['commit']
    assert manifest['gitTree'] == s['tree']
    assert manifest['commitCount'] == s['commitCount']
    assert manifest['dirty'] is False
    assert manifest['sourceProvenance'] == 'git'


def configure(preset, s, sdk, aws):
    # The existing runner's clean archive route supplies provenance. This route
    # keeps a real Git checkout, so clear any cached archive assertions instead.
    result = replay.configure(preset, {'candidate': True}, sdk, aws)
    return result + ['-DPUTGD_SOURCE_COMMIT=', '-DPUTGD_SOURCE_TREE=',
                     '-DPUTGD_SOURCE_COMMIT_COUNT=',
                     '-DBUILD_TESTING=' + ('ON' if preset == 'dev' else 'OFF')]


def host_build(r):
    r.all_stopped()
    s = source(r)
    assert git('rev-parse', 'HEAD') == s['commit']
    assert git('status', '--porcelain', '--untracked-files=normal') == ''
    verify_tree(WORKTREE, s['files'])
    out = r.out / 'host'
    out.mkdir()
    sdk = ROOT / 'build'
    for preset in ('dev', 'release'):
        r.command(configure(preset, s, sdk / 'sdk', sdk / 'dependencies/aws-sdk-cpp'),
                  cwd=WORKTREE, timeout=180)
        r.command(['cmake', '--build', '--preset', preset], cwd=WORKTREE, timeout=1800)
        if preset == 'dev':
            r.command(['ctest', '--preset', 'dev', '-R', IDENTITY_TESTS,
                       '--output-junit', str(out / 'dev.xml')],
                      cwd=WORKTREE, timeout=1800)
    r.command(['cpack', '--config', 'build/release/CPackConfig.cmake', '-G', 'DEB',
               '-B', str(out / 'packages')], cwd=WORKTREE, timeout=180)
    packages = list((out / 'packages').glob('*.deb'))
    assert len(packages) == 1
    manifest = replay.read(WORKTREE / 'build/release/build-identity/build-info.json')
    manifest_ok(manifest, s)
    replay.save(out / 'build.json', {'utc': replay.now(), 'sourceCommit': s['commit'],
                                   'manifest': manifest, 'package': str(packages[0]),
                                   'packageSha256': replay.sha(packages[0]),
                                   'sourceCache': str(WORKTREE), 'nativeAcceptance': 'pending'})


VERIFY_REMOTE = '''import pathlib,json,hashlib,subprocess
p=pathlib.Path.cwd();s=json.loads((p/'source.json').read_text());old=json.loads((p/'previous-source.json').read_text());src=pathlib.Path(s['frozenCandidate']['guestSourceCache'])
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def git(*args): return subprocess.check_output(['git','-C',str(src),*args],text=True).strip()
def run(*args): subprocess.run(['git','-C',str(src),*args],check=True,stdout=subprocess.DEVNULL)
def verify(files):
 for name,item in files.items():
  f=src/name
  if item['type']=='file': assert f.is_file() and not f.is_symlink() and digest(f)==item['sha256'] and f.stat().st_mode&0o777==item['mode'],name
  else: assert f.is_symlink() and str(f.readlink())==item['target'],name
assert src==pathlib.Path('/home/lab/putgd-ui-refinement-lazy-exp001-native/source') and src.is_dir()
assert digest(p/'source.bundle')==s['frozenCandidate']['bundleSha256']
assert digest(p/'source.tar')==s['archiveSha256']
assert digest(p/'candidate.patch')==old['candidate']['patchSha256']==s['frozenCandidate']['reviewedPatchSha256']
assert git('rev-parse','HEAD')==old['commit']
assert sorted(git('diff','--name-only','HEAD').splitlines())==sorted(s['frozenCandidate']['files'])
assert git('diff','--cached','--name-only')==''
verify(old['files'])
before={name:digest(src/name) for name in s['frozenCandidate']['files']}
assert before==s['frozenCandidate']['files']
assert hashlib.sha256(subprocess.check_output(['git','-C',str(src),'diff','--binary','HEAD','--',*before])).hexdigest()==old['candidate']['patchSha256']
run('apply','--reverse','--check',str(p/'candidate.patch'))
run('apply','--reverse',str(p/'candidate.patch'))
assert git('status','--porcelain','--untracked-files=normal')==''
run('fetch',str(p/'source.bundle'),'HEAD')
run('checkout','--detach',s['commit'])
assert git('rev-parse','HEAD')==s['commit'] and git('rev-parse','HEAD^')==old['commit']
assert git('rev-parse','HEAD^{tree}')==s['tree'] and int(git('rev-list','--count','HEAD'))==s['commitCount']
assert git('status','--porcelain','--untracked-files=normal')==''
verify(s['files'])
after={name:digest(src/name) for name in s['frozenCandidate']['files']}
assert before==after
print(json.dumps({'verified':True,'sourceCommit':s['commit'],'sourceFilesVerified':len(s['files']),'beforeOwnedHashes':before,'afterOwnedHashes':after,'clean':True,'cache':str(src)}))
'''


def guest_build(r):
    assert r.name in ('gnome', 'kde')
    r.all_stopped()
    s = source(r)
    guest = '/home/lab/putgd-ui-refinement-' + r.out.name
    r.shell('mkdir ' + shlex.quote(guest) + '\n')
    for local, name in ((s['archive'], 'source.tar'), (r.out / 'source.json', 'source.json'),
                        (s['frozenCandidate']['bundle'], 'source.bundle'),
                        (PREVIOUS / 'source.json', 'previous-source.json'),
                        (replay.read(PREVIOUS / 'source.json')['candidate']['patch'], 'candidate.patch')):
        r.put(local, guest + '/' + name)
    verified = json.loads(r.command(replay.lab.ssh_args(r.run) +
                                   ['cd ' + shlex.quote(guest) + ' && python3 -'],
                                   text=VERIFY_REMOTE, timeout=180))
    replay.save(r.dest / 'cache-update.json', verified)
    script = 'set -eu\ncd ' + shlex.quote(REMOTE_CACHE) + '\n'
    for preset in ('dev', 'release'):
        script += shlex.join(configure(preset, s, Path('/home/lab/putgd-memory-build/sdk'),
                                              Path('/home/lab/putgd-memory-build/dependencies/aws-sdk-cpp'))) + '\n'
        script += 'cmake --build --preset ' + preset + '\n'
        if preset == 'dev':
            script += 'ctest --preset dev -R ' + shlex.quote(IDENTITY_TESTS) + ' --output-junit ' + shlex.quote(guest + '/dev.xml') + '\n'
    script += 'cpack --config build/release/CPackConfig.cmake -G DEB -B ' + shlex.quote(guest + '/packages') + '\n'
    r.shell(script, timeout=7200)
    packages = r.shell('find ' + shlex.quote(guest + '/packages') + " -maxdepth 1 -name '*.deb' -print\n").splitlines()
    assert len(packages) == 1
    package = r.dest / Path(packages[0]).name
    r.get(packages[0], package)
    r.get(guest + '/dev.xml', r.dest / 'dev.xml')
    r.get(REMOTE_CACHE + '/build/release/build-identity/build-info.json', r.dest / 'build-info.json')
    manifest = replay.read(r.dest / 'build-info.json')
    manifest_ok(manifest, s)
    replay.save(r.dest / 'build.json', {'utc': replay.now(), 'sourceCommit': s['commit'],
                                      'manifest': manifest, 'guestDirectory': guest,
                                      'sourceCache': REMOTE_CACHE, 'package': str(package),
                                      'packageSha256': replay.sha(package), 'nativeAcceptance': 'pending'})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True)
    sub = p.add_subparsers(dest='phase', required=True)
    q = sub.add_parser('prepare'); q.add_argument('--commit', required=True)
    sub.add_parser('host-build')
    q = sub.add_parser('guest-build'); q.add_argument('--guest', choices=('gnome', 'kde'), required=True)
    args = p.parse_args()
    r = replay.Replay(args)
    try:
        {'prepare': prepare, 'host-build': host_build, 'guest-build': guest_build}[args.phase](r)
        replay.save(r.attempt / 'complete.json', {'utc': replay.now(), 'phase': args.phase,
                                                 'proceduralComplete': True, 'nativeAcceptance': 'pending'})
    except BaseException as e:
        replay.save(r.attempt / 'failed.json', {'utc': replay.now(), 'phase': args.phase,
                                               'errorType': type(e).__name__, 'error': str(e)})
        raise


if __name__ == '__main__':
    main()
