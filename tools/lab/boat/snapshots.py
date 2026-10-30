#!/usr/bin/env python3
"""Repeatable additive Boat snapshot preparation. Run heavy subcommands on Boat only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import tempfile
import xml.etree.ElementTree as ET

OLD_NAMES = frozenset(('putgd-build-base', 'putgd-lab-cinnamon',
                       'putgd-lab-ubuntu-gnome', 'putgd-lab-ubuntu-gnome-v2',
                       'putgd-lab-aurora'))
PROBE_PACKAGES = ('python3', 'python3-pyatspi', 'python3-dbus', 'python3-pyqt6',
                  'dbus-bin', 'libglib2.0-bin', 'libx11-6', 'xdotool', 'x11-utils',
                  'x11-xserver-utils', 'xclip', 'wl-clipboard', 'tesseract-ocr')
PROBE_TOOLS = ('dbus-monitor', 'gdbus', 'xdotool', 'xprop', 'xwininfo', 'xrandr',
               'xclip', 'wl-paste', 'wl-copy', 'tesseract')
PROBE_CONTRACT = {
    'commit': '4b5e185ff9df2ae7dd578ae535da1202c58deb68',
    'path': 'tools/lab/surface_driver/profiles/provision-probes.sh',
    'sha256': 'bbbc802c14512ae80d34791e4632ffe557b34bfdc5edb273aaf7ffdb2e6c2816',
    'additionalPackages': ['tesseract-ocr'],
}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sha(value, size=40):
    if not re.fullmatch('[0-9a-f]{' + str(size) + '}', value):
        raise argparse.ArgumentTypeError(f'expected {size} lowercase hexadecimal characters')
    return value


def snapshot_name(value):
    if value in OLD_NAMES or not re.fullmatch(r'putgd-[a-z0-9]+(?:-[a-z0-9]+)*-v(?:[2-9]|[1-9][0-9]+)', value):
        raise argparse.ArgumentTypeError('use a NEW versioned putgd snapshot name')
    return value



def package_metadata(name, target=None):
    role = re.sub(r'-v[0-9]+$', '', name)
    supported = {'putgd-build-base': 'ubuntu24.04', 'putgd-lab-cinnamon': 'ubuntu24.04',
                 'putgd-lab-ubuntu-gnome': 'ubuntu26.04'}
    expected = supported.get(role)
    if expected is None or (target is not None and target != expected):
        raise ValueError('package target does not match the snapshot profile')
    return {'packageTarget': expected, 'hostPackageTarget': 'ubuntu24.04',
            'packageBuild': {'method': 'candidate-native-target', 'artifact': expected,
                'command': f'python3 tools/lab/boat/candidate.py COMMIT --artifact {expected} --output NEW_PRIVATE_OUTPUT',
                'environment': 'the target distro container from the exact commit package.yml'}}


def deb_identity(path):
    """Read packaged identity without executing a foreign-distro binary."""
    path = Path(path).resolve()
    with tempfile.TemporaryDirectory(prefix='pgdeb-') as temporary:
        run(['dpkg-deb', '-x', path, temporary])
        root = Path(temporary) / 'usr/share/putgd'
        identity = json.loads((root / 'build-info.json').read_text())
    audit_path = path.parent / 'native-audit.json'
    audit = json.loads(audit_path.read_text())
    package_hash = digest(path)
    if audit.get('package', {}).get('sha256') != package_hash or audit.get('manifest') != identity:
        raise ValueError('native audit does not match the DEB bytes and packaged build identity')
    return {'filename': path.name, 'path': str(path), 'sha256': package_hash,
            'packageTarget': audit['updateCheck']['packageTarget'], 'nativeAuditSha256': digest(audit_path),
            'version': run(['dpkg-deb', '-f', path, 'Version']).strip(),
            'depends': run(['dpkg-deb', '-f', path, 'Depends']).strip(), 'buildInfo': identity}


def validate_package(package, target, commit):
    if (package.get('packageTarget') != target or package['buildInfo'].get('gitCommit') != commit
            or package['buildInfo'].get('dirty') is not False):
        raise ValueError('package must match the guest target and expected clean nightly commit')



def validate_guest_platform(platform, target):
    compatible = ((platform.get('id') == 'ubuntu' and platform.get('versionId') == target.removeprefix('ubuntu'))
                  or (target == 'ubuntu24.04' and platform.get('id') == 'linuxmint'
                      and platform.get('ubuntuCodename') == 'noble'))
    if not compatible:
        raise ValueError('guest distribution does not match the requested native package target')


def run_name(value):
    if not re.fullmatch('[a-z][a-z0-9]{1,10}', value):
        raise argparse.ArgumentTypeError('short alphanumeric run name required')
    return value


def jobs(value):
    number = int(value)
    if not 1 <= number <= 8:
        raise argparse.ArgumentTypeError('jobs must be between 1 and 8')
    return number



def tcp_port(value):
    number = int(value)
    if not 1 <= number <= 65535:
        raise argparse.ArgumentTypeError('TCP port must be between 1 and 65535')
    return number


def run(argv, *, cwd=None, timeout=1800):
    result = subprocess.run([str(x) for x in argv], cwd=cwd, text=True,
                            capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{Path(str(argv[0])).name} exited {result.returncode}: {result.stdout[-4000:]}{result.stderr[-4000:]}')
    return result.stdout


def exclusive_json(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')


def ci_packages(checkout):
    """Fail closed if CI no longer has the single supported Ubuntu dependency line."""
    workflow = (checkout / '.github/workflows/ci.yml').read_text()
    lines = [line.strip() for line in workflow.splitlines()
             if 'apt-get install -y' in line and 'build-essential ccache git' in line]
    if len(lines) != 1:
        raise ValueError('CI dependency recipe changed; review before provisioning')
    packages = shlex.split(lines[0].split('"${apt[@]}" ', 1)[1])
    if not packages or any(not re.fullmatch('[a-z0-9+.-]+', p) for p in packages):
        raise ValueError('unsupported CI package expression')
    return packages


def probe_script(install=False):
    script = '#!/bin/sh\nset -eu\nexport PATH=/usr/bin:/bin:$PATH\n'
    if install:
        script += 'sudo -n apt-get update\nsudo -n apt-get install -y --no-install-recommends ' + shlex.join(PROBE_PACKAGES) + '\n'
    script += "python3 -c 'import pyatspi, dbus; from PyQt6.QtGui import QGuiApplication'\n"
    for tool in PROBE_TOOLS:
        script += 'command -v ' + tool + '\n'
    script += "dpkg-query -W -f='${binary:Package}=${Version}\\n' " + shlex.join(PROBE_PACKAGES) + '\n'
    return script


def host_script(checkout, commit, count, prepare):
    q = shlex.quote
    packages = ci_packages(checkout)
    script = '#!/bin/bash\nset -euo pipefail\nexport PATH=/usr/bin:/bin:$PATH LANG=C.UTF-8 LC_ALL=C.UTF-8\n'
    script += 'cd ' + q(str(checkout.resolve())) + '\n'
    script += 'test "$(git rev-parse HEAD)" = ' + q(commit) + '\ntest -z "$(git status --porcelain --untracked-files=no)"\n'
    script += 'uptime\ndf -h .\ndf -i .\n'
    capacity_code = "import os,json; s=os.statvfs(" + repr(str(checkout.resolve())) + "); print(json.dumps(dict(availableBytes=s.f_bavail*s.f_frsize,availableInodes=s.f_favail))); assert s.f_bavail*s.f_frsize >= 14*1024**3 and s.f_favail > 1024"
    script += "python3 -c " + q(capacity_code) + "\n"
    if prepare:
        script += 'sudo -n apt-get update\nsudo -n env DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=l apt-get install -y --allow-downgrades ' + shlex.join([*(p + '/noble' if p in ('nodejs', 'npm') else p for p in packages), 'zstd', 'qemu-system-x86', 'acl', 'x11vnc', 'tesseract-ocr']) + '\n'
        script += 'id -u putgd-ci >/dev/null 2>&1 || sudo -n useradd --create-home --shell /bin/bash putgd-ci\n'
        sdk = (checkout / 'tools/ci/provision-linux-sdk.sh').read_text()
        if sdk.count('bash tools/ci/sdk-source.sh\n') != 1:
            raise ValueError('CI SDK provisioning recipe changed')
        sdk = sdk.replace('bash tools/ci/sdk-source.sh\n', 'if test -d build/dependencies/aws-sdk-cpp; then\n  python3 tools/verify_dependencies.py build/dependencies/aws-sdk-cpp\nelse\n  bash tools/ci/sdk-source.sh\nfi\n')
        script += f'export PUTGD_SDK_JOBS={count}\n# CI tools/ci/provision-linux-sdk.sh with verified warm-source reuse:\n' + sdk + '\npython3 tools/proton/provision_runtime.py\n'
    script += "dpkg-query -W -f='${binary:Package}=${Version}\\n' " + shlex.join(packages) + '\n'
    script += 'cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON -DPUTGD_TEST_TIMEOUT_SCALE=3 -DPUTGD_AWS_SOURCE_DIR="$PWD/build/sdk/share/putgd-ci/aws-sdk-source-licenses" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache\n'
    script += f'nice -n 10 cmake --build --preset dev --parallel {count}\n'
    script += 'build/dev/putgd-cli --version-json\n'
    # Grant the unprivileged CI user traversal, without opening private homes globally.
    ancestors = [p for p in checkout.resolve().parents if str(p).startswith('/home/user')]
    if ancestors:
        script += 'sudo -n setfacl -m u:putgd-ci:--x ' + shlex.join(str(p) for p in ancestors) + '\n'
    script += 'scratch_base=${PUTGD_CI_TMPDIR:-/dev/shm}\n'
    script += 'test "$(stat -f -c %T:%t "$scratch_base")" = tmpfs:1021994\n'
    script += 'df -h "$scratch_base"\n'
    script += 'test_tmp=$(mktemp -d "$scratch_base/p.XXXXXX")\n'
    script += 'sudo -n python3 tools/ci/ctest_preflight.py --build build/dev --tmp "$test_tmp" --user putgd-ci\n'
    script += 'sudo -n chown -R putgd-ci:putgd-ci build/dev "$test_tmp"\n'
    script += 'sudo -n install -d -m 700 -o putgd-ci -g putgd-ci "$test_tmp"/{config,data,state,cache,runtime}\n'
    script += "trap 'sudo -n chown -R user:user build/dev' EXIT\n"
    script += "ctest_status=0\n"
    script += "sudo -n runuser -u putgd-ci -- env -u DISPLAY -u XAUTHORITY -u WAYLAND_DISPLAY PUTGD_TEST_TIME_SCALE=3 LANG=C.UTF-8 LC_ALL=C.UTF-8 TMPDIR=\"$test_tmp\" XDG_CONFIG_HOME=\"$test_tmp/config\" XDG_DATA_HOME=\"$test_tmp/data\" XDG_STATE_HOME=\"$test_tmp/state\" XDG_CACHE_HOME=\"$test_tmp/cache\" XDG_RUNTIME_DIR=\"$test_tmp/runtime\" ctest --preset dev --parallel 12 --resource-spec-file \"$PWD/tools/ci/ctest-resources.json\" --output-junit \"$PWD/build/dev/snapshot-tests.xml\" || ctest_status=$?\n"
    script += 'sudo -n chown -R user:user build/dev\n'
    script += 'python3 ' + q(str(Path(__file__).resolve())) + ' ctest-result --checkout "$PWD" --exit-code "$ctest_status"\n'
    return script



def record_ctest(checkout, code):
    build = checkout.resolve() / 'build/dev'
    cases = list(ET.parse(build / 'snapshot-tests.xml').iter('testcase'))
    expected = {test['name'] for test in json.loads((build / 'registered-tests.json').read_text())['tests']}
    names = {case.attrib['name'] for case in cases}
    failed = [case.attrib['name'] for case in cases if case.find('failure') is not None
              or case.find('error') is not None or case.find('skipped') is not None
              or case.attrib.get('status') in ('fail', 'notrun')]
    ok = code == 0 and bool(expected) and names == expected and len(cases) == len(expected) and not failed
    summary = {'tests': len(cases), 'failedFirst': failed, 'failed': failed, 'retried': [],
               'passedOnRetry': [], 'blocking': failed, 'unexplainedCtestFailure': code != 0 and not failed,
               'ok': ok}
    (build / 'snapshot-flakes.json').write_text(json.dumps({
        'schemaVersion': 1, 'source': {'kind': 'boat-direct-ctest', 'commit': run(['git', 'rev-parse', 'HEAD'], cwd=checkout).strip()},
        'command': ['ctest', '--preset', 'dev', '--parallel', '12', '--resource-spec-file', str(checkout / 'tools/ci/ctest-resources.json')],
        'exitCode': code, 'retryEnabled': False, 'summary': summary}, indent=2) + '\n')
    return summary


def canonical_image(path):
    path = Path(path).absolute()
    if path.resolve() != path or path.parent.parent.name != 'lab-images' or path.parent.parent.parent.name != 'work':
        raise ValueError('image must be under a real canonical work/lab-images/NAME root')
    return path


def seal(source, overlay, destination, expected, commit, name, host_packages, target=None, compress=False):
    target_metadata = package_metadata(name, target)
    source, overlay = Path(source).resolve(), Path(overlay).resolve()
    destination = canonical_image(destination)
    if destination.exists() or destination.parent.exists():
        raise ValueError('new golden directory must not exist')
    if digest(source) != expected:
        raise ValueError('source golden hash mismatch')
    info = json.loads(run(['qemu-img', 'info', '--output=json', overlay]))
    if Path(info.get('full-backing-filename', '')).resolve() != source:
        raise ValueError('overlay must directly reference the specified source golden')
    # qemu-img convert takes an image lock and refuses a writable running source.
    destination.parent.mkdir(mode=0o700, parents=True)
    available = os.statvfs(destination.parent)
    limit = available.f_bavail * available.f_frsize - 10 * 1024**3 - 256 * 1024**2
    if limit <= 0:
        raise ValueError('host reserve unavailable before flattening')
    def output_limit():
        import resource
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    converted = subprocess.run(['nice', '-n', '10', 'qemu-img', 'convert', *(['-c'] if compress else []),
        '-f', 'qcow2', '-O', 'qcow2', overlay, destination], text=True, capture_output=True, timeout=1800, preexec_fn=output_limit)
    (destination.parent / 'conversion.log').write_text(converted.stdout + converted.stderr)
    if converted.returncode:
        raise RuntimeError('flattening failed or reached its hard file-size reserve; partial image preserved')
    result = json.loads(run(['qemu-img', 'info', '--output=json', destination]))
    if result['format'] != 'qcow2' or result.get('backing-filename'):
        raise ValueError('new image must be standalone qcow2')
    run(['qemu-img', 'check', destination])
    if digest(source) != expected:
        raise ValueError('source golden changed during sealing')
    destination.chmod(0o444)
    manifest = make_manifest(name, commit, source, expected, destination, digest(destination),
                             host_packages, result['virtual-size'])
    manifest.update(target_metadata)
    if name.startswith('putgd-lab-cinnamon-'):
        manifest['guestSessionPolicy'] = {'mintUpdate': {'autostartOverride': '~/.config/autostart/mintupdate.desktop', 'hidden': True, 'enabled': False, 'scope': 'disposable guest user; original golden preserved'}}
    manifest['golden']['compressedClusters'] = compress
    manifest['conversion'] = {'command': 'qemu-img convert ' + ('-c ' if compress else '') + '-f qcow2 -O qcow2',
                              'hardOutputLimitBytes': limit, 'reserveBytes': 10 * 1024**3}
    parent_manifest = source.parent / 'manifest.json'
    if parent_manifest.is_file():
        parent = json.loads(parent_manifest.read_text())
        if parent.get('golden', {}).get('sha256') == expected:
            manifest['sourceChain'] = [parent['sourceGolden'], *parent.get('sourceChain', [])]
    exclusive_json(destination.parent / 'manifest.json', manifest)
    return manifest


def make_manifest(name, commit, source, source_hash, image, image_hash, packages, virtual_bytes):
    snapshot_name(name)
    sha(commit)
    sha(source_hash, 64)
    sha(image_hash, 64)
    if source_hash == image_hash:
        raise ValueError('new provisioned golden must have a new digest')
    if not packages or len(packages) != len(set(packages)):
        raise ValueError('host package list must be nonempty and unique')
    return {'schemaVersion': 1, 'placement': 'boat', 'snapshotName': name,
            'builtTreeCommit': commit, 'hostPackages': list(packages),
            'probePackages': list(PROBE_PACKAGES), 'probeTools': list(PROBE_TOOLS),
            'probeContract': PROBE_CONTRACT,
            'sourceGolden': {'path': str(source), 'sha256': source_hash},
            'golden': {'path': str(canonical_image(image)), 'sha256': image_hash,
                       'standalone': True, 'virtualBytes': virtual_bytes}, **package_metadata(name)}



def build_manifest(checkout, name, golden, source, expected, output, target=None, deb=None, controller=None, controller_commit=None):
    snapshot_name(name)
    target_metadata = package_metadata(name, target)
    checkout = checkout.resolve()
    identity_path = checkout / 'build/dev/build-identity/build-info.json'
    identity = json.loads(identity_path.read_text())
    commit = run(['git', 'rev-parse', 'HEAD'], cwd=checkout).strip()
    if identity.get('gitCommit') != commit or identity.get('dirty') is not False:
        raise ValueError('built identity must match the clean checkout')
    source_digest = digest(source)
    if source_digest != expected:
        raise ValueError('source golden changed')
    ledger_path = checkout / 'build/dev/snapshot-flakes.json'
    ledger = json.loads(ledger_path.read_text())
    if ledger.get('summary', {}).get('ok') is not True or ledger['summary'].get('tests', 0) <= 0:
        raise ValueError('full CTest run must pass before publication')
    if ledger.get('retryEnabled') is not False:
        raise ValueError('publication requires one full CTest run with retries disabled')
    settings = Path('/etc/environment').read_text().splitlines()
    if not all(line in settings for line in ('PUTGD_CI_TMPDIR=/dev/shm', 'TMPDIR=/dev/shm')):
        raise ValueError('persist the documented fast scratch settings before publication')
    if run(['stat', '-f', '-c', '%T:%t', '/dev/shm']).strip() != 'tmpfs:1021994':
        raise ValueError('documented scratch must be tmpfs')
    golden = canonical_image(golden)
    packages = ci_packages(checkout)
    info = json.loads(run(['qemu-img', 'info', '--output=json', golden]))
    if info.get('backing-filename') or info['format'] != 'qcow2':
        raise ValueError('golden must be standalone qcow2')
    data = {'schemaVersion': 1, 'placement': 'boat', 'snapshotName': name, **target_metadata,
            'builtTreeCommit': commit, 'builtTree': str(checkout), 'buildInfo': identity,
            'fastScratch': {'base': '/dev/shm', 'filesystem': 'tmpfs', 'environment': 'PUTGD_CI_TMPDIR',
                            'privatePerRun': True, 'testParallel': 12, 'timeScale': 3, 'retryEnabled': False},
            'hostPackages': packages, 'hostPackageVersions': run(['dpkg-query', '-W',
                '-f=${binary:Package}=${Version}\n', *packages]).splitlines(),
            'sourceGolden': {'path': str(source), 'sha256': expected},
            'golden': {'path': str(golden), 'sha256': source_digest if golden == source.resolve() else digest(golden), 'standalone': True,
                       'virtualBytes': info['virtual-size']},
            'buildInfoSha256': digest(identity_path),
            'ctestSummary': ledger['summary'], 'ctestLedgerSha256': digest(ledger_path),
            'ctestJunitSha256': digest(checkout / 'build/dev/snapshot-tests.xml')}
    if golden != source.resolve():
        provisioned = json.loads((golden.parent / 'manifest.json').read_text())
        if provisioned['golden']['sha256'] != data['golden']['sha256']:
            raise ValueError('provisioned golden digest changed')
        data.update(probePackages=provisioned['probePackages'], probeTools=provisioned['probeTools'],
                    probeContract=provisioned.get('probeContract', PROBE_CONTRACT),
                    sourceChain=provisioned.get('sourceChain', []))
        if 'guestSessionPolicy' in provisioned:
            data['guestSessionPolicy'] = provisioned['guestSessionPolicy']
        data['golden']['manifestSha256'] = digest(golden.parent / 'manifest.json')
    else:
        data.update(probePackages=[], probeTools=[], goldenUnchanged=True)
    bootstrap = Path('/home/user/snapshot-bootstrap')
    if bootstrap.is_dir():
        data['bootstrap'] = {
            'publicKeySha256': digest(bootstrap / 'identity.pub'),
            'knownHostsSha256': digest(bootstrap / 'known_hosts'),
            'hostKeyFingerprints': run(['ssh-keygen', '-lf', bootstrap / 'known_hosts']).splitlines()}
    data['binaries'] = {name: digest(checkout / 'build/dev' / name)
                        for name in ('putgd', 'putgd-cli', 'putgd-ui')}
    runtime = checkout / 'build/dependencies/proton-runtime'
    data['protonRuntime'] = {'sha256': digest(runtime / 'putgd-proton-runtime'),
                             'manifestSha256': digest(runtime / 'manifest.json')}
    workflow = (checkout / '.github/workflows/package.yml').read_text()
    images = re.findall(r'- name: ' + re.escape(data['packageTarget']) + r'\n\s+format: DEB\n\s+image: ([^\s]+)', workflow)
    if len(images) != 1 or not re.fullmatch(r'ubuntu:[0-9.]+@sha256:[0-9a-f]{64}', images[0]):
        raise ValueError('package target container must be pinned in the exact source workflow')
    data['packageBuild']['containerImage'] = images[0]
    if Path('/home/user/putgd-gnome-golden/image.qcow2').exists():
        raise ValueError('legacy GNOME golden remains outside canonical work/lab-images; preserve its bytes and resolve references before relocation')
    data['imagesRoot'] = str(golden.parent.parent)
    data['goldenInventory'] = []
    for image in sorted(golden.parent.parent.rglob('*.qcow2')):
        image_info = json.loads(run(['qemu-img', 'info', '--output=json', image]))
        if not image_info.get('backing-filename'):
            canonical_image(image)
            data['goldenInventory'].append({'path': str(image), 'format': image_info['format'],
                                           'standalone': True, 'virtualBytes': image_info['virtual-size']})
    if controller is not None:
        actual = run(['git', 'rev-parse', 'HEAD'], cwd=controller).strip()
        if controller_commit is not None and actual != controller_commit:
            raise ValueError('lab controller commit mismatch')
        data['labController'] = {'path': str(controller.resolve()), 'commit': actual,
            'entrypoint': str(controller.resolve() / 'tools/lab/lab.py')}
    if deb is not None:
        package = deb_identity(deb)
        validate_package(package, data['packageTarget'], commit)
        data['packageArtifact'] = package
    exclusive_json(output, data)
    return data


def publication_receipt(worker, name, manifest, output, request=None, timeout=900):
    """Read back a binding, including after an interrupted CLI; never save it again."""
    snapshot_name(name)
    data = json.loads(Path(manifest).read_text())
    if data['snapshotName'] != name or data.get('placement') != 'boat':
        raise ValueError('manifest does not describe this snapshot')
    if Path(output).exists():
        raise ValueError('output receipt already exists')
    deadline = time.monotonic() + timeout
    while True:
        listing = json.loads(run(['boat', '--json', 'snapshots', '--limit', '1'], timeout=120))
        matches = [item for item in listing['named'] if item['name'] == name]
        if matches:
            saved = matches[0]
            if saved.get('sourceSandboxId') != worker:
                raise ValueError('snapshot belongs to a different source worker')
            if saved['status'] == 'ready' and saved.get('snapshotId'):
                exclusive_json(output, {'manifest': data, 'snapshot': saved,
                                         'sourceSandboxId': worker, 'request': request,
                                         'verification': 'boat snapshots readback'})
                return saved
            if saved['status'] not in ('saving', 'pending'):
                raise RuntimeError('snapshot publication failed: ' + saved['status'])
        if time.monotonic() >= deadline:
            raise TimeoutError('snapshot did not become ready; use receipt for read-only recovery')
        time.sleep(2)


def publish(worker, name, manifest, output):
    # No replacing a name, including a previous run's new-version name.
    snapshot_name(name)
    listing = json.loads(run(['boat', '--json', 'snapshots', '--limit', '1'], timeout=120))
    if any(item['name'] == name for item in listing['named']):
        raise ValueError('snapshot name already exists; choose a new name or use receipt')
    data = json.loads(Path(manifest).read_text())
    if data['snapshotName'] != name or data.get('placement') != 'boat':
        raise ValueError('manifest does not describe this snapshot')
    if Path(output).exists():
        raise ValueError('output receipt already exists')
    # The CLI may emit multiple JSON events, or lose its reply after the save.
    # Either case is resolved by readback, never by repeating the save operation.
    try:
        raw = run(['boat', '--json', 'snapshot', worker, name], timeout=600)
        decoder, events = json.JSONDecoder(), []
        while raw.strip():
            raw = raw.lstrip()
            event, consumed = decoder.raw_decode(raw)
            events.append(event)
            raw = raw[consumed:]
        request = {'events': events}
    except (RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        request = {'errorType': type(error).__name__, 'detail': str(error)[:2000]}
    return publication_receipt(worker, name, manifest, output, request)


def cinnamon_policy_script(install):
    """Disable only Mint Update's XDG autostart in a disposable guest user profile."""
    setup = r'''
source = Path('/etc/xdg/autostart/mintupdate.desktop')
if not source.is_file():
    raise RuntimeError('expected Mint Update autostart entry is missing')
override.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
override.write_text('[Desktop Entry]\nType=Application\nName=Update Manager\nHidden=true\nX-GNOME-Autostart-enabled=false\n')
override.chmod(0o600)
for pid, start in running:
    if (Path('/proc') / str(pid) / 'stat').read_text().split()[21] != start:
        raise RuntimeError('Mint Update PID identity changed')
    os.kill(pid, signal.SIGTERM)
deadline = time.monotonic() + 10
while updates():
    if time.monotonic() >= deadline:
        raise RuntimeError('owned overlay Mint Update did not stop')
    time.sleep(.2)
''' if install else ''
    return "python3 - <<'POLICY'\n" + r'''import os,json,signal,time
from pathlib import Path
override = Path.home() / '.config/autostart/mintupdate.desktop'
def updates():
    found=[]
    for p in Path('/proc').iterdir():
        if not p.name.isdecimal(): continue
        try:
            if p.stat().st_uid != os.geteuid() or (p/'comm').read_text().strip() != 'mintUpdate': continue
            args=(p/'cmdline').read_bytes().split(b'\0')
            if not any(Path(os.fsdecode(arg)).name.lower() in ('mintupdate', 'mintupdate.py') for arg in args if arg):
                raise RuntimeError('unexpected Mint Update command identity')
            found.append((int(p.name), (p/'stat').read_text().split()[21]))
        except FileNotFoundError: continue
    return found
running=updates()
''' + setup + r'''
text=override.read_text()
assert 'Hidden=true' in text and 'X-GNOME-Autostart-enabled=false' in text
assert not updates(), 'Mint Update must be absent in a fresh verification overlay'
print(json.dumps(dict(autostartOverride=str(override),hidden=True,enabled=False,priorRunningPids=[pid for pid,start in running],remainingPids=[])))
POLICY
'''


def guest(args):
    """Operate only fresh lab overlays; the caller supplies an authenticated pin."""
    target = 'ubuntu26.04' if args.profile == 'gnome-wayland' else 'ubuntu24.04'
    if args.package_target is not None and args.package_target != target:
        raise ValueError('DEB target does not match the guest profile')
    if args.install:
        if args.name is None or args.destination is None:
            raise ValueError('--install requires a new snapshot name and destination image')
        prefix = 'putgd-lab-ubuntu-gnome-' if args.profile == 'gnome-wayland' else 'putgd-lab-cinnamon-'
        if not args.name.startswith(prefix):
            raise ValueError('snapshot name does not match the guest profile')
        canonical_image(args.destination)
        package_metadata(args.name, target)
    checkout = args.checkout.resolve()
    controller = (args.controller_checkout or checkout).resolve()
    controller_commit = run(['git', 'rev-parse', 'HEAD'], cwd=controller).strip()
    if args.controller_commit is not None and controller_commit != args.controller_commit:
        raise ValueError('lab controller does not match its requested commit')
    sys.path.insert(0, str(controller / 'tools/lab'))
    import lab
    if run(['git', 'rev-parse', 'HEAD'], cwd=checkout).strip() != args.commit:
        raise ValueError('checkout does not match requested commit')
    if digest(args.source) != args.source_sha256:
        raise ValueError('source golden hash mismatch')
    evidence = args.evidence.absolute()
    evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    exclusive_json(evidence / 'controller.json', {'path': str(controller), 'commit': controller_commit,
                                                'applicationCommit': args.commit})
    run_dir = controller / 'work/lab-runs' / args.run_name
    socket_check = run(['python3', checkout / 'tools/diagnostics/check_socket_paths.py', '--socket',
                        run_dir / 'qmp.sock', run_dir / 'vnc.sock'])
    (evidence / 'socket-preflight.json').write_text(socket_check)
    # Boat ascii-lazyfs exposes positive statvfs inode counts; the builder-only
    # preflight deliberately does not classify this filesystem. Record raw values.
    capacity = os.statvfs(checkout)
    exclusive_json(evidence / 'capacity.json', {
        'placement': 'boat', 'filesystem': run(['stat', '-f', '-c', '%T:%t', checkout]).strip(),
        'availableBytes': capacity.f_bavail * capacity.f_frsize,
        'availableInodes': capacity.f_favail, 'requiredReserveBytes': 10 * 1024**3})
    image_estimate = args.source.stat().st_size // (2 if args.compress_golden else 1)
    expected_growth = image_estimate + 2 * 1024**3 if args.install else 0
    if capacity.f_bavail * capacity.f_frsize < 10 * 1024**3 + expected_growth or capacity.f_favail <= 1024:
        raise ValueError('Boat host capacity below reserve')
    if not os.access('/dev/kvm', os.R_OK | os.W_OK):
        raise ValueError('KVM must be accessible before guest provisioning')
    package = deb_identity(args.deb) if args.deb else None
    if package:
        validate_package(package, target, args.package_commit or args.commit)
    launched = False
    try:
        result = run(['python3', controller / 'tools/lab/lab.py', 'launch', '--profile', args.profile,
                      '--golden', args.source, '--run', run_dir, '--bootstrap-key', args.bootstrap_key,
                      '--known-hosts', args.known_hosts, '--acceleration', 'kvm', '--timeout', '240'], timeout=300)
        launched = True
        (evidence / 'launch.json').write_text(result)
        if args.profile == 'cinnamon-x11':
            exclusive_json(evidence / 'session-policy.json', json.loads(lab.ssh(run_dir, cinnamon_policy_script(args.install))))
        capacity_script = "python3 - <<'CAPACITY'\nimport os,json\nr={}\nfor p in (os.path.expanduser('~'), '/tmp'):\n s=os.statvfs(p); r[p]=dict(availableBytes=s.f_bavail*s.f_frsize,availableInodes=s.f_favail); assert s.f_bavail*s.f_frsize >= 2*1024**3 and s.f_favail > 1024\nprint(json.dumps(r))\nCAPACITY\n"
        (evidence / 'guest-capacity.json').write_text(lab.ssh(run_dir, capacity_script))
        with lab.QMP(run_dir / 'qmp.sock') as qmp:
            qmp.execute('screendump', {'filename': str(evidence / 'desktop.ppm')})
        checked = lab.command(lab.ssh_args(run_dir) + ['sh', '-s'],
                              input=probe_script(args.install), timeout=900, check=False)
        (evidence / 'probes.log').write_text(checked.stdout + checked.stderr)
        if checked.returncode:
            raise ValueError('probe installation/check failed; evidence preserved')
        exclusive_json(evidence / 'idle-policy.json', lab.guest_preflight(run_dir, apply=False, stage='snapshot-check'))
        platform = json.loads(lab.ssh(run_dir, "/usr/bin/python3 - <<'PLATFORM'\nimport json\nfrom pathlib import Path\nfrom PyQt6.QtCore import QT_VERSION_STR\nd=dict(line.split('=',1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)\nd={k:v.strip(chr(34)) for k,v in d.items()}\nprint(json.dumps(dict(id=d.get('ID'),versionId=d.get('VERSION_ID'),ubuntuCodename=d.get('UBUNTU_CODENAME'),qtVersion=QT_VERSION_STR)))\nPLATFORM\n"))
        validate_guest_platform(platform, target)
        exclusive_json(evidence / 'platform.json', {**platform, 'packageTarget': target})
        if package:
            exclusive_json(evidence / 'package-artifact.json', {**package, 'packageTarget': target})
            exclusive_json(evidence / 'install.json', lab.install(run_dir, deb=args.deb))
            socket_script = "python3 - <<'SOCKET'\nimport os,json\nfrom pathlib import Path\np=str(Path(os.environ['XDG_RUNTIME_DIR']).resolve()/'putgd/session.sock')\nn=len(p.encode('utf-8'))\nprint(json.dumps(dict(socket=p,utf8Bytes=n,limit=107,ok=n<=107)))\nassert n<=107\nSOCKET\n"
            exclusive_json(evidence / 'app-socket-preflight.json', json.loads(lab.guest_session(run_dir, socket_script)))
            started = lab.start(run_dir, timeout=60)
            exclusive_json(evidence / 'start.json', started)
            observed = json.loads(lab.guest_session(run_dir, 'putgd-cli --version-json\n'))
            if observed.get('version') != package['buildInfo']['version'] or observed.get('gitCommit') != package['buildInfo']['gitCommit']:
                raise ValueError('installed version does not match the package build')
            exclusive_json(evidence / 'installed-version.json', observed)
            exclusive_json(evidence / 'show.json', lab.cli(run_dir, 'ui.show', {'view': 'settings'}))
            time.sleep(3)
            exclusive_json(evidence / 'view-state.json', lab.cli(run_dir, 'ui.viewState'))
            if args.profile == 'cinnamon-x11':
                exclusive_json(evidence / 'session-policy-after-start.json', json.loads(lab.ssh(run_dir, cinnamon_policy_script(False))))
            # The new golden must not autostart a private smoke profile on its next boot.
            with lab.QMP(run_dir / 'qmp.sock') as qmp:
                qmp.execute('screendump', {'filename': str(evidence / 'app-started.ppm')})
            lab.cli(run_dir, 'ui.quit')
            deadline = time.monotonic() + 15
            while lab.ssh(run_dir, f"if test -d /proc/{started['resident']['pid']}; then echo running; else echo stopped; fi\n").strip() != 'stopped':
                if time.monotonic() >= deadline:
                    raise TimeoutError('installed app did not finish its graceful shutdown')
                time.sleep(.25)

        with lab.QMP(run_dir / 'qmp.sock') as qmp:
            qmp.execute('screendump', {'filename': str(evidence / 'desktop-after.ppm')})
        lab.ssh(run_dir, 'sync\n')
        if args.install:
            # Do not persist a disposable run's authorized key in the new golden.
            key = (run_dir / 'identity.pub').read_text().strip().split()[1]
            lab.ssh(run_dir, "sed -i '/" + key.replace('/', r'\/') + "/d' ~/.ssh/authorized_keys\nsync\n")
    finally:
        if launched or (run_dir / 'state.json').exists():
            (evidence / 'reset.json').write_text(run(['python3', controller / 'tools/lab/lab.py',
                                                     'reset', '--run', run_dir]))
    if args.install:
        result = seal(args.source, run_dir / 'disk.qcow2', args.destination, args.source_sha256,
                      args.commit, args.name, ci_packages(checkout), target, args.compress_golden)
        exclusive_json(evidence / 'golden.json', result)
    state = json.loads((run_dir / 'state.json').read_text())
    completion = {'run': str(run_dir), 'runId': state['id'], 'completed': True, 'outcome': 'passed'}
    exclusive_json(evidence / 'completion.json', completion)
    exclusive_json(evidence / 'cleanup-evidence.json', {
        'schemaVersion': 1, 'kind': 'vm-overlay-cleanup-evidence', **completion,
        'files': {p.name: digest(p) for p in evidence.iterdir() if p.is_file()}})
    cleanup_args = ['python3', controller / 'tools/lab/lab.py', 'cleanup', '--run', run_dir,
                    '--images-root', args.source.parent.parent, '--evidence-manifest', evidence / 'cleanup-evidence.json']
    if args.test_held_overlay:
        holder = subprocess.Popen([sys.executable, '-c',
            'import os,sys,time,json; f=open(sys.argv[1],"rb"); s=os.fstat(f.fileno()); print(json.dumps(dict(device=s.st_dev,inode=s.st_ino,fd=f.fileno())),flush=True); time.sleep(60)',
            str(run_dir / 'disk.qcow2')], stdout=subprocess.PIPE, text=True)
        try:
            held_identity = json.loads(holder.stdout.readline())
            actual = (run_dir / 'disk.qcow2').stat()
            if held_identity.get('device') != actual.st_dev or held_identity.get('inode') != actual.st_ino:
                raise RuntimeError('held-overlay witness did not open the exact overlay')
            refused = subprocess.run([str(a) for a in cleanup_args], text=True, capture_output=True, timeout=90)
            report = {'holderPid': holder.pid, 'heldIdentity': held_identity, 'exitCode': refused.returncode, 'stdout': refused.stdout,
                      'stderr': refused.stderr, 'overlayStillExists': (run_dir / 'disk.qcow2').exists()}
            exclusive_json(evidence / 'held-overlay-refusal.json', report)
            error = json.loads(refused.stdout).get('error', '')
            if refused.returncode == 0 or not report['overlayStillExists'] or not any(message in error for message in
                    ('still has the overlay open', 'Privileged host process inventory blocked or incomplete',
                     'Privileged host process inventory found a process using the overlay')):
                raise ValueError('guard did not explicitly refuse the still-held overlay')
        finally:
            if holder.poll() is None:
                holder.terminate()
            holder.wait(timeout=15)
            holder.stdout.close()
        final_manifest = evidence / 'cleanup-evidence-final.json'
        exclusive_json(final_manifest, {'schemaVersion': 1, 'kind': 'vm-overlay-cleanup-evidence', **completion,
            'files': {p.name: digest(p) for p in evidence.iterdir() if p.is_file()}})
        cleanup_args[-1] = final_manifest
    (evidence / 'cleanup-preview.json').write_text(run([*cleanup_args, '--dry-run']))
    (evidence / 'cleanup.json').write_text(run(cleanup_args))
    return {'run': str(run_dir), 'evidence': str(evidence), 'placement': 'boat', 'ok': True}



def host_package(args):
    """Install and start the matching package on an owned Ubuntu 24.04 Boat head."""
    release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    if release.get('ID', '').strip('"') != 'ubuntu' or release.get('VERSION_ID', '').strip('"') != '24.04':
        raise ValueError('host smoke requires the Ubuntu 24.04 Boat host')
    package = deb_identity(args.deb)
    validate_package(package, 'ubuntu24.04', args.commit)
    output = args.evidence.absolute()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    exclusive_json(output / 'package.json', package)
    with (output / 'install.log').open('w') as log:
        subprocess.run(['sudo', '-n', 'env', 'DEBIAN_FRONTEND=noninteractive', 'NEEDRESTART_MODE=l',
                        'apt-get', 'install', '--reinstall', '-y', str(args.deb.resolve())],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
    sys.path.insert(0, str(args.checkout.resolve() / 'tools/lab'))
    from rfb import RFB
    app, vnc = None, None
    with tempfile.TemporaryDirectory(prefix='pgh-', dir='/tmp') as temporary:
        runtime = Path(temporary)
        env = dict(os.environ, DISPLAY=args.display, XAUTHORITY='/home/user/.Xauthority', QT_QPA_PLATFORM='xcb')
        for name in ('CONFIG', 'DATA', 'STATE', 'CACHE', 'RUNTIME'):
            directory = runtime / name.lower()
            directory.mkdir(mode=0o700)
            env['XDG_' + name + ('_DIR' if name == 'RUNTIME' else '_HOME')] = str(directory)
        socket_check = run(['python3', args.checkout / 'tools/diagnostics/check_socket_paths.py',
                            '--socket', runtime / 'runtime/putgd/session.sock'])
        (output / 'socket-preflight.json').write_text(socket_check)
        try:
            with (output / 'vnc.log').open('w') as vnc_log, (output / 'app.log').open('w') as app_log:
                vnc = subprocess.Popen(['x11vnc', '-display', args.display, '-auth', '/home/user/.Xauthority',
                                        '-localhost', '-nopw', '-rfbport', str(args.vnc_port), '-forever', '-shared'],
                                       stdout=vnc_log, stderr=subprocess.STDOUT)
                app = subprocess.Popen(['/usr/bin/putgd-launcher', '--background'], env=env,
                                       stdout=app_log, stderr=subprocess.STDOUT)
                exclusive_json(output / 'owned-processes.json', {'appPid': app.pid, 'vncPid': vnc.pid})
                deadline = time.monotonic() + 60
                while True:
                    if app.poll() is not None:
                        raise RuntimeError('installed app exited before CLI readiness')
                    result = subprocess.run(['/usr/bin/putgd-cli', 'capabilities'], env=env,
                                            capture_output=True, text=True, timeout=10)
                    if result.returncode == 0:
                        capabilities = json.loads(result.stdout)
                        if capabilities.get('ok'):
                            break
                    if time.monotonic() >= deadline:
                        raise TimeoutError('installed app did not answer capabilities')
                    time.sleep(.5)
                version = json.loads(subprocess.check_output(['/usr/bin/putgd-cli', '--version-json'], env=env, text=True, timeout=30))
                if version != package['buildInfo']:
                    raise ValueError('installed version does not match the build')
                exclusive_json(output / 'installed-version.json', version)
                exclusive_json(output / 'capabilities.json', capabilities)
                shown = json.loads(subprocess.check_output(['/usr/bin/putgd-cli', 'ui.show', json.dumps({'view': 'settings'})], env=env, text=True, timeout=30))
                if shown.get('ok') is not True:
                    raise ValueError('installed app did not open its Settings window')
                exclusive_json(output / 'show.json', shown)
                # Give the wall its normal three-second discovery interval.
                time.sleep(3)
                client = RFB(args.vnc_port)
                try:
                    client.screenshot(output / 'app-started.png')
                finally:
                    client.close()
                subprocess.run(['/usr/bin/putgd-cli', 'ui.quit'], env=env, check=True,
                               stdout=subprocess.DEVNULL, timeout=15)
                app.wait(timeout=15)
                exclusive_json(output / 'result.json', {'ok': True, 'placement': 'boat',
                    'packageTarget': 'ubuntu24.04', 'packageSha256': package['sha256'], 'version': version})
        except BaseException as error:
            exclusive_json(output / 'failure.json', {'ok': False, 'errorType': type(error).__name__, 'detail': str(error)[:2000]})
            try:
                client = RFB(args.vnc_port)
                try:
                    client.screenshot(output / 'failure.png')
                finally:
                    client.close()
            except (OSError, RuntimeError):
                pass
            raise
        finally:
            for process in (app, vnc):
                if process is not None and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
    return {'ok': True, 'evidence': str(output)}



def prepare_scratch():
    if run(['stat', '-f', '-c', '%T:%t', '/dev/shm']).strip() != 'tmpfs:1021994':
        raise ValueError('/dev/shm is not a supported tmpfs scratch')
    code = r"""from pathlib import Path
p=Path('/etc/environment')
lines=p.read_text().splitlines()
lines=[line for line in lines if not line.startswith(('PUTGD_CI_TMPDIR=', 'TMPDIR='))]
lines += ['PUTGD_CI_TMPDIR=/dev/shm', 'TMPDIR=/dev/shm']
p.write_text('\n'.join(lines)+'\n')
"""
    subprocess.run(['sudo', '-n', '/usr/bin/python3', '-'], input=code, text=True, check=True, timeout=30)
    return {'base': '/dev/shm', 'filesystem': 'tmpfs', 'environmentFile': '/etc/environment'}


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('prepare-scratch', help='persist the Boat tmpfs CTest default without changing /tmp mounts')
    result_p = sub.add_parser('ctest-result', help='record one direct full CTest pass; never retry')
    result_p.add_argument('--checkout', type=Path, required=True)
    result_p.add_argument('--exit-code', type=int, required=True)
    host = sub.add_parser('host-script', help='emit a reviewed script to run on a Boat worker')
    host.add_argument('--checkout', type=Path, required=True)
    host.add_argument('--commit', type=sha, required=True)
    host.add_argument('--jobs', type=jobs, default=4)
    host.add_argument('--prepare', action='store_true', help='install/provision; omit on verification forks')
    probe = sub.add_parser('probe-script', help='emit guest installation or read-only check script')
    probe.add_argument('--install', action='store_true')
    seal_p = sub.add_parser('seal', help='flatten a STOPPED fresh provisioning overlay; never boot a golden')
    for flag in ('source', 'overlay', 'destination', 'checkout'):
        seal_p.add_argument('--' + flag, type=Path, required=True)
    seal_p.add_argument('--source-sha256', type=lambda x: sha(x, 64), required=True)
    seal_p.add_argument('--commit', type=sha, required=True)
    seal_p.add_argument('--name', type=snapshot_name, required=True)
    seal_p.add_argument('--package-target', choices=('ubuntu24.04', 'ubuntu26.04'))
    seal_p.add_argument('--compress', action='store_true')
    guest_p = sub.add_parser('guest', help='launch a fresh overlay, install/check probes, stop and retire it')
    for flag in ('checkout', 'source', 'bootstrap-key', 'known-hosts', 'evidence'):
        guest_p.add_argument('--' + flag, type=Path, required=True)
    guest_p.add_argument('--profile', choices=('cinnamon-x11', 'gnome-wayland'), required=True)
    guest_p.add_argument('--run-name', type=run_name, required=True)
    guest_p.add_argument('--commit', type=sha, required=True)
    guest_p.add_argument('--source-sha256', type=lambda x: sha(x, 64), required=True)
    guest_p.add_argument('--install', action='store_true')
    guest_p.add_argument('--name', type=snapshot_name)
    guest_p.add_argument('--destination', type=Path)
    guest_p.add_argument('--package-target', choices=('ubuntu24.04', 'ubuntu26.04'))
    guest_p.add_argument('--deb', type=Path)
    guest_p.add_argument('--package-commit', type=sha)
    guest_p.add_argument('--controller-checkout', type=Path)
    guest_p.add_argument('--controller-commit', type=sha)
    guest_p.add_argument('--test-held-overlay', action='store_true')
    guest_p.add_argument('--compress-golden', action='store_true')
    manifest_p = sub.add_parser('manifest', help='bind built identity, package versions and golden chain')
    for flag in ('checkout', 'golden', 'source', 'output'):
        manifest_p.add_argument('--' + flag, type=Path, required=True)
    manifest_p.add_argument('--name', type=snapshot_name, required=True)
    manifest_p.add_argument('--package-target', choices=('ubuntu24.04', 'ubuntu26.04'))
    manifest_p.add_argument('--deb', type=Path)
    manifest_p.add_argument('--controller-checkout', type=Path)
    manifest_p.add_argument('--controller-commit', type=sha)
    manifest_p.add_argument('--source-sha256', type=lambda x: sha(x, 64), required=True)
    save = sub.add_parser('publish', help='save only a new name and retain its API receipt')
    host_smoke = sub.add_parser('host-package', help='install/start a matching DEB on an owned Boat Ubuntu 24.04 head')
    for flag in ('checkout', 'deb', 'evidence'):
        host_smoke.add_argument('--' + flag, type=Path, required=True)
    host_smoke.add_argument('--commit', type=sha, required=True)
    host_smoke.add_argument('--display', default=':0')
    host_smoke.add_argument('--vnc-port', type=tcp_port, default=5901)
    receipt = sub.add_parser('receipt', help='read back an existing owned snapshot without saving again')
    for command in (save, receipt):
        command.add_argument('--worker', required=True)
        command.add_argument('--name', type=snapshot_name, required=True)
        command.add_argument('--manifest', type=Path, required=True)
        command.add_argument('--output', type=Path, required=True)
    return p


def main():
    a = parser().parse_args()
    if a.command == 'prepare-scratch':
        print(json.dumps(prepare_scratch(), indent=2))
    elif a.command == 'ctest-result':
        summary = record_ctest(a.checkout, a.exit_code)
        print(json.dumps(summary, indent=2))
        if not summary['ok']:
            raise SystemExit(1)
    elif a.command == 'host-script':
        print(host_script(a.checkout, a.commit, a.jobs, a.prepare), end='')
    elif a.command == 'probe-script':
        print(probe_script(a.install), end='')
    elif a.command == 'seal':
        print(json.dumps(seal(a.source, a.overlay, a.destination, a.source_sha256,
                              a.commit, a.name, ci_packages(a.checkout), a.package_target, a.compress), indent=2))
    elif a.command == 'guest':
        if a.install and (not a.destination or not a.name):
            raise ValueError('--install requires --destination and --name')
        print(json.dumps(guest(a), indent=2))
    elif a.command == 'manifest':
        print(json.dumps(build_manifest(a.checkout, a.name, a.golden, a.source, a.source_sha256, a.output, a.package_target, a.deb, a.controller_checkout, a.controller_commit), indent=2))
    elif a.command == 'host-package':
        print(json.dumps(host_package(a), indent=2))
    elif a.command in ('publish', 'receipt'):
        function = publish if a.command == 'publish' else publication_receipt
        print(json.dumps(function(a.worker, a.name, a.manifest, a.output), indent=2))


if __name__ == '__main__':
    main()
