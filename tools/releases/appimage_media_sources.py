#!/usr/bin/env python3
"""Build the corresponding source for the AppImage's GPL recording components.

The AppImage bundles FFmpeg built with --enable-gpl, the distro's libx264 and
libx265 and the GStreamer plugin that links x264, which makes the image a
GPL-2.0-or-later combined work. This archive carries their exact sources: the
pinned FFmpeg commit as `git archive` output with its configure options, and
the Ubuntu source packages (as downloaded by apt) for the bundled distro
binaries recorded in the AppImage receipt. put.gd's own source is the release's
`-source.tar.gz`. Output: `putgd-VERSION-appimage-media-source.tar.gz` and the
`appimage-media-source.json` receipt the collector checks against the image.
"""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

from audit_native import sha256 as digest

RECEIPT_NAME = 'appimage-media-source.json'


def archive_name(version):
    return 'putgd-' + version + '-appimage-media-source.tar.gz'


def published_components(receipt):
    components = receipt['recordingRuntime']['gplComponents']
    return sorted({(item['sourcePackage'], item['sourceVersion']) for item in components})


def _member(name, data, timestamp):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mtime = timestamp
    info.mode = 0o644
    info.uid = info.gid = 0
    info.uname = info.gname = 'root'
    return info, io.BytesIO(data)


def build(receipt_path, ffmpeg_source, output_dir, timestamp, download=None):
    receipt = json.loads(receipt_path.read_text())
    version = receipt['build']['version']
    runtime = receipt['recordingRuntime']
    ffmpeg = runtime['ffmpeg']
    head = subprocess.run(['git', '-C', str(ffmpeg_source), 'rev-parse', 'HEAD'], check=True,
                          text=True, capture_output=True).stdout.strip()
    if head != ffmpeg['commit']:
        raise ValueError('FFmpeg checkout differs from the pinned commit in the AppImage receipt')
    prefix = 'ffmpeg-' + ffmpeg['release'] + '/'
    ffmpeg_tar = subprocess.run(['git', '-C', str(ffmpeg_source), 'archive', '--format=tar', '--prefix=' + prefix,
                                 ffmpeg['commit']], check=True, capture_output=True).stdout
    members = [('ffmpeg/' + prefix.rstrip('/') + '.tar', ffmpeg_tar)]
    sources = []
    with tempfile.TemporaryDirectory(prefix='pgmsrc-') as scratch:
        for package, package_version in published_components(receipt):
            target = Path(scratch) / package
            target.mkdir()
            # apt checks every file against the signed Sources index it fetched.
            (download or _apt_source)(package, package_version, target)
            files = sorted(path for path in target.iterdir() if path.is_file())
            if not any(path.suffix == '.dsc' for path in files):
                raise ValueError('No source package downloaded for ' + package + ' ' + package_version)
            entries = []
            for path in files:
                data = path.read_bytes()
                members.append(('ubuntu/' + package + '/' + path.name, data))
                entries.append({'file': path.name, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)})
            binaries = sorted(item['binaryPackage'] + ' ' + item['binaryVersion']
                              for item in runtime['gplComponents'] if item['sourcePackage'] == package)
            sources.append({'sourcePackage': package, 'sourceVersion': package_version,
                            'bundledBinaries': binaries, 'files': entries})
    catalog = {'schemaVersion': 1, 'appImage': {'version': version, 'sha256': receipt['artifact']['sha256']},
               'license': 'GPL-2.0-or-later combined work; each component keeps its own license',
               'ffmpeg': {'repository': ffmpeg['repository'], 'release': ffmpeg['release'], 'commit': ffmpeg['commit'],
                          'configure': runtime['media']['ffmpeg']['configure'],
                          'file': members[0][0], 'sha256': hashlib.sha256(ffmpeg_tar).hexdigest()},
               'ubuntuSources': sources}
    readme = ('Corresponding source for the GPL recording components of put.gd AppImage ' + version + '.\n\n'
              'ffmpeg/: FFmpeg ' + ffmpeg['release'] + ' at ' + ffmpeg['commit'] + ' (git archive). Build it with\n'
              '  ./configure --prefix=PREFIX --libdir=PREFIX/lib ' + ' '.join(runtime['media']['ffmpeg']['configure']) + '\n'
              'against Ubuntu 24.04 development packages, as tools/releases/provision-appimage.py does.\n\n'
              'ubuntu/: the Ubuntu 24.04 source packages of the bundled distro binaries listed in SOURCES.json\n'
              '(unpack with dpkg-source -x NAME.dsc). put.gd itself is the release asset putgd-' + version + '-source.tar.gz.\n')
    members += [('SOURCES.json', (json.dumps(catalog, indent=2) + '\n').encode()), ('README.txt', readme.encode())]
    output_dir.mkdir(parents=True, exist_ok=True)
    archive = output_dir / archive_name(version)
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.PAX_FORMAT) as tar:
        for name, data in sorted(members):
            tar.addfile(*_member(name, data, timestamp))
    with archive.open('wb') as stream, gzip.GzipFile(filename='', mode='wb', fileobj=stream, mtime=0, compresslevel=9) as packed:
        packed.write(raw.getvalue())
    result = {'schemaVersion': 1, 'format': 'appimage-media-source',
              'appImage': catalog['appImage'], 'file': archive.name, 'sha256': digest(archive),
              'bytes': archive.stat().st_size, 'ffmpegCommit': ffmpeg['commit'],
              'ubuntuSources': [{'sourcePackage': item['sourcePackage'], 'sourceVersion': item['sourceVersion']}
                                for item in sources]}
    (output_dir / RECEIPT_NAME).write_text(json.dumps(result, indent=2) + '\n')
    return result


def _apt_source(package, version, target):
    subprocess.run(['apt-get', 'source', '--download-only', '-qq', package + '=' + version], check=True, cwd=target)


def verify(directory, receipt, appimage_receipt):
    """The collector's check: the archive's bytes and contents name this exact image."""
    path = directory / receipt['file']
    if path.is_symlink() or not path.is_file() or digest(path) != receipt['sha256'] or path.stat().st_size != receipt['bytes']:
        raise ValueError('AppImage media source bytes differ from their receipt')
    if receipt['appImage']['sha256'] != appimage_receipt['artifact']['sha256']:
        raise ValueError('AppImage media source belongs to another image')
    runtime = appimage_receipt.get('recordingRuntime', {})
    if receipt['ffmpegCommit'] != runtime.get('ffmpeg', {}).get('commit'):
        raise ValueError('AppImage media source carries another FFmpeg commit')
    expected = [{'sourcePackage': name, 'sourceVersion': version} for name, version in published_components(appimage_receipt)]
    if receipt['ubuntuSources'] != expected:
        raise ValueError('AppImage media source packages differ from the bundled binaries')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', type=Path, required=True, help='appimage-build.json')
    parser.add_argument('--ffmpeg-source', type=Path, required=True, help='Pinned FFmpeg git checkout')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--timestamp', type=int, required=True, help='Release commit time for member mtimes')
    args = parser.parse_args()
    print(json.dumps(build(args.receipt, args.ffmpeg_source, args.output_dir, args.timestamp)))


if __name__ == '__main__':
    main()
