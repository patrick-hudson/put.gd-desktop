"""Synthetic source inventory/bytes for collector tests; never desktop evidence."""
import hashlib
import io
import json
from pathlib import Path
import tarfile

import appimage_complete_sources as complete


def add_inventory(image):
    lock = json.loads((complete.ROOT / 'packaging/appimage/runtime-source.json').read_text())
    pins = []
    components = []
    for name in ('putgd', 'proton-helper', 'appimage-runtime', 'ffmpeg', 'icu', 'layershell', 'aws-sdk-cpp', 'qtbase', 'ubuntu:gtk'):
        source = {'repository': 'https://example.invalid/' + name.replace(':', '-'), 'commit': 'a' * 40}
        if name == 'putgd':
            source['commit'] = image['build']['gitCommit']
        if name == 'proton-helper':
            source = {'sourceCatalog': 'SOURCES.json', 'commit': 'b' * 40}
        if name == 'ubuntu:gtk':
            source = {'sourcePackage': 'gtk+3.0', 'sourceVersion': '3.24.41-4ubuntu1'}
        if name == 'qtbase':
            pins.append({**source, 'release': '6.12.0'})
        components.append({'id': name, 'source': source})
    image['notices'] = {'inventory': {'components': components}}
    image['toolLock'] = {'buildInputs': {'qtLicenseSources': pins}, 'tools': [{'name': 'runtime-x86_64', **lock['runtime']}]}
    return image


def write_source(directory, image):
    components = complete.inventory(image)
    files = {}
    records = []
    for item in components:
        if 'external' in item:
            records.append(item)
        else:
            name = item['id'].replace(':', '-')
            records.append({**item, 'directory': name})
            files[name + '/source.tar'] = b'synthetic source archive'
    runtime = complete.runtime_inputs(image, complete.ROOT / 'packaging/appimage/runtime-source.json')
    catalog = {'components': records, 'runtime': runtime, 'appImage': image['artifact'],
               'files': [{'file': name, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)} for name, data in files.items()]}
    files['SOURCES.json'] = json.dumps(catalog).encode()
    files['README.txt'] = b'synthetic source fixture'
    path = directory / complete.archive_name(image['build']['version'])
    with tarfile.open(path, 'w:gz') as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name); info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    receipt = {'format': 'appimage-complete-source', 'file': path.name, 'sha256': complete.sha256(path),
               'bytes': path.stat().st_size, 'appImage': image['artifact'], 'components': components, 'runtime': runtime}
    (directory / complete.RECEIPT_NAME).write_text(json.dumps(receipt))
    return receipt
