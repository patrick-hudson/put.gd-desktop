#!/usr/bin/env python3
"""CycloneDX SBOMs of the release packages and their Grype vulnerability report (#420).

tools:    download the pinned Syft and Grype (packaging/sbom/tools.lock.json).
database: the current Grype database archive, verified, for `grype db import`.
generate: Syft's catalog of one extracted package, completed with the components
          the package's own inventories name. Syft finds little by itself: the
          AppImage has no dpkg database, and nothing records the built AWS SDK,
          FFmpeg or Proton helper versions in a form it reads.
scan:     Grype over each SBOM against the imported database.
report:   classify the findings with packaging/sbom/policy.json, write the job
          summary, and fail only where the policy blocks this channel.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
from urllib.parse import quote
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[2]
TOOLS_LOCK = ROOT / 'packaging/sbom/tools.lock.json'
POLICY = ROOT / 'packaging/sbom/policy.json'
LABELS = ('appimage', 'ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44')
DISTROS = {'appimage': 'ubuntu:24.04', 'ubuntu24.04': 'ubuntu:24.04', 'ubuntu26.04': 'ubuntu:26.04',
           'debian13': 'debian:13', 'fedora44': 'fedora:44'}
SEVERITIES = ('Critical', 'High', 'Medium', 'Low', 'Negligible', 'Unknown')
# Origins: code put.gd builds or ships itself, and the Ubuntu packages the AppImage
# freezes, are put.gd's to fix; components Syft found on its own are reported with them.
BUNDLED = ('bundled', 'bundled-distro', 'detected')
# NVD product names for components that have CVEs under them; checked with `grype db search`.
CPES = {
    'qt': ('qt:qt',), 'ffmpeg': ('ffmpeg:ffmpeg',),
    'icu': ('icu-project:international_components_for_unicode', 'unicode:international_components_for_unicode'),
    'harfbuzz': ('harfbuzz_project:harfbuzz',), 'libpng': ('libpng:libpng',), 'pcre2': ('pcre:pcre2',),
    'sqlite': ('sqlite:sqlite',), 'libjpeg-turbo': ('libjpeg-turbo:libjpeg-turbo',), 'md4c': ('md4c_project:md4c',),
    'libtiff': ('libtiff:libtiff',), 'libwebp': ('webmproject:libwebp',), 'freetype': ('freetype:freetype',),
    # Proton's fork of OpenPGP.js keeps upstream's versions.
    'openpgp': ('openpgpjs:openpgpjs',),
}
# Qt's third-party attributions by name: (component key, CPE key).
QT_EMBEDDED = (('harfbuzz', 'harfbuzz'), ('libjpeg-turbo', 'libjpeg-turbo'), ('libpng', 'libpng'), ('md4c', 'md4c'),
               ('pcre2', 'pcre2'), ('sqlite', 'sqlite'), ('libtiff', 'libtiff'), ('libwebp', 'libwebp'),
               ('tinycbor', None), ('double-conversion', None))
# AWS CRT submodules the SDK build links (aws-lc is not built: provision-sdk.sh uses
# the system OpenSSL; the CBMC model is test-only).
AWS_REPOSITORIES = {'s2n': 'https://github.com/aws/s2n-tls'}
AWS_UNSHIPPED = ('aws-lc', 'aws-verification-model-for-libcrypto')
PROTON_BUNDLED = ('bundled-js', 'embedded-runtime')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def download(url, path):
    request = urllib.request.Request(url, headers={'User-Agent': 'putgd-sbom'})
    with urllib.request.urlopen(request, timeout=300) as response, open(path, 'wb') as output:
        shutil.copyfileobj(response, output)


def fetch_tools(output, lock=TOOLS_LOCK):
    output.mkdir(parents=True, exist_ok=True)
    found = {}
    for tool in json.loads(lock.read_text())['tools']:
        archive = output / Path(tool['url']).name
        download(tool['url'], archive)
        if sha256(archive) != tool['sha256']:
            raise ValueError(tool['name'] + ' archive differs from the locked SHA-256')
        with tarfile.open(archive) as bundle:
            member = bundle.getmember(tool['name'])
            if not member.isfile():
                raise ValueError(tool['name'] + ' is not a regular file in its archive')
            bundle.extract(member, output, filter='data')
        archive.unlink()
        found[tool['name']] = {'release': tool['release'], 'sha256': tool['sha256'], 'path': str(output / tool['name'])}
    return found


def database_listing(lock=TOOLS_LOCK):
    """The current database archive: its file name, URL and SHA-256."""
    listing_url = json.loads(lock.read_text())['grypeDatabase']['listing']
    with tempfile.TemporaryDirectory() as work:
        listing = Path(work) / 'latest.json'
        download(listing_url, listing)
        value = json.loads(listing.read_text())
    name, checksum = value['path'], value['checksum']
    if Path(name).name != name or not re.fullmatch(r'sha256:[0-9a-f]{64}', checksum):
        raise ValueError('Unexpected Grype database listing')
    return {'name': name, 'url': listing_url.rsplit('/', 1)[0] + '/' + name, 'sha256': checksum.split(':', 1)[1],
            'built': value.get('built', ''), 'schemaVersion': value.get('schemaVersion', '')}


def extract(package, label, work):
    """Unpack one package into a new directory; returns its root."""
    root = work / 'root'
    root.mkdir(parents=True)
    if label == 'appimage':
        package = package.resolve()
        package.chmod(0o755)
        subprocess.run([str(package), '--appimage-extract'], check=True, cwd=work, stdout=subprocess.DEVNULL)
        root.rmdir()
        (work / 'squashfs-root').rename(root)
    elif package.suffix == '.deb':
        subprocess.run(['dpkg-deb', '-x', str(package), str(root)], check=True)
    else:
        rpm = subprocess.Popen(['rpm2cpio', str(package)], stdout=subprocess.PIPE)
        subprocess.run(['cpio', '-idm', '--quiet', '--no-absolute-filenames'], stdin=rpm.stdout, check=True, cwd=root)
        if rpm.wait() != 0:
            raise ValueError('rpm2cpio failed for ' + package.name)
    return root


def component(name, version, purl, origin, *, cpes=(), license=None, repository=None, commit=None, properties=()):
    item = {'type': 'library', 'bom-ref': purl, 'name': name}
    if version:
        item['version'] = version
    item['purl'] = purl
    if cpes:
        item['cpe'] = cpes[0]
    if license:
        item['licenses'] = [{'expression': license}]
    if repository:
        item['externalReferences'] = [{'type': 'vcs', 'url': repository + ('#' + commit if commit else '')}]
    values = [('putgd:origin', origin), *(('syft:cpe23', cpe) for cpe in cpes[1:]), *properties]
    if commit:
        values.append(('putgd:commit', commit))
    item['properties'] = [{'name': key, 'value': value} for key, value in values]
    return item


def cpes_for(key, version):
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', version or ''):
        return ()
    return tuple(f'cpe:2.3:a:{product}:{version}:*:*:*:*:*:*:*' for product in CPES.get(key, ()))


def generic_purl(name, version):
    return f'pkg:generic/{quote(name, safe="")}' + (f'@{quote(version, safe="")}' if version else '')


def deb_purl(name, version, distro, source, source_version):
    purl = f'pkg:deb/{distro[0]}/{quote(name, safe="")}@{quote(version, safe="")}?arch=amd64&distro={distro[0]}-{distro[1]}'
    if source and (source, source_version) != (name, version):
        purl += '&upstream=' + quote(source, safe='') + ('%40' + quote(source_version, safe='') if source_version != version else '')
    return purl


def npm_purl(name, version):
    return 'pkg:npm/' + quote(name, safe='/').replace('@', '%40', 1) + '@' + quote(version, safe='')


def qt_embedded(module, entry):
    """Qt's own third-party code with a usable version (qt_attribution.json via bundled-components.json)."""
    lowered = re.sub(r'[^a-z0-9]+', '-', entry['name'].lower())
    version = entry.get('version') or ''
    version = version[1:] if re.fullmatch(r'v[0-9.]+', version) else version
    for key, cpe in QT_EMBEDDED:
        if key in lowered or key.replace('-', '') in lowered.replace('-', ''):
            if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', version):
                return None
            # PCRE2's JIT is a second attribution of the same library.
            if key == 'pcre2' and 'just-in-time' in lowered:
                return None
            return component(key, version, generic_purl(key, version), 'bundled', cpes=cpes_for(cpe, version) if cpe else (),
                             license=entry.get('license'), properties=[('putgd:embeddedIn', module)])
    return None


def aws_components(lock):
    sdk = lock['awsSdk']
    found = [component('aws-sdk-cpp', sdk['release'], generic_purl('aws-sdk-cpp', sdk['release']), 'bundled',
                       license='Apache-2.0', repository=sdk['repository'].removesuffix('.git'), commit=sdk['commit'])]
    for item in sdk['submodules']:
        name = item['path'].rsplit('/', 1)[1]
        if name in AWS_UNSHIPPED:
            continue
        repository = AWS_REPOSITORIES.get(name, 'https://github.com/awslabs/' + name)
        owner_repo = repository.removeprefix('https://github.com/')
        found.append(component(name, item['commit'], f'pkg:github/{owner_repo}@{item["commit"]}', 'bundled',
                               license='Apache-2.0', repository=repository, commit=item['commit'],
                               properties=[('putgd:embeddedIn', 'aws-sdk-cpp')]))
    return found


def proton_components(notices):
    """The packages the shipped helper's notices classify as bundled or embedded."""
    found = []
    blocks = re.findall(r'^Package: (\S+) (\S+)\nClassification: (\S+)\nDeclared license: (.*)$', notices.read_text(errors='replace'), re.M)
    if not blocks:
        raise ValueError('Proton notices list no packages: ' + str(notices))
    for name, version, classification, license in blocks:
        if classification not in PROTON_BUNDLED:
            continue
        properties = [('putgd:embeddedIn', 'proton-helper'), ('putgd:protonClassification', classification)]
        if classification == 'embedded-runtime':
            # @oven/bun-<platform> is the Bun runtime itself: advisories name the `bun` package.
            properties.append(('putgd:npmPackage', name))
            name = 'bun'
        key = name.rsplit('/', 1)[-1]
        license = None if license == 'not declared' else license
        found.append(component(name, version, npm_purl(name, version), 'bundled', cpes=cpes_for(key, version),
                               license=license, properties=properties))
    return found


def appimage_components(root):
    inventory = json.loads((root / 'usr/share/doc/putgd/bundled-components.json').read_text())
    found = []
    for item in inventory['components']:
        identifier, version, source = item['id'], item.get('version', ''), item.get('source', {})
        if identifier == 'putgd':
            continue
        if identifier.startswith('ubuntu:'):
            distribution = source['distribution'].lower().split()
            found.append(component(item['name'], version, deb_purl(item['name'], version, distribution,
                                                                  source.get('sourcePackage'), source.get('sourceVersion')),
                                   'bundled-distro', license=item.get('license'),
                                   properties=[('putgd:sourcePackage', source.get('sourcePackage', ''))]))
            continue
        if identifier in ('aws-sdk-cpp', 'proton-helper'):
            continue  # Itemized below from their own locks.
        key = 'qt' if identifier.startswith('qt') and identifier != 'qtkeychain' else identifier
        found.append(component(identifier, version, generic_purl(identifier, version), 'bundled', cpes=cpes_for(key, version),
                               license=item.get('license'), repository=(source.get('repository') or '').removesuffix('.git') or None,
                               commit=source.get('commit')))
        if key == 'qt':
            found += [entry for entry in (qt_embedded(identifier, embedded) for embedded in item.get('embedded', [])) if entry]
    found += aws_components(json.loads((root / 'usr/share/putgd/dependency-lock.json').read_text()))
    found += proton_components(root / 'usr/libexec/putgd/proton/THIRD-PARTY-NOTICES.txt')
    return found


def native_components(root):
    found = aws_components(json.loads((root / 'usr/share/putgd/dependency-lock.json').read_text()))
    found += proton_components(root / 'usr/libexec/putgd/proton/THIRD-PARTY-NOTICES.txt')
    return found


def dedupe(components):
    seen, unique = set(), []
    for item in components:
        if item['bom-ref'] not in seen:
            seen.add(item['bom-ref'])
            unique.append(item)
    return unique


def merge_syft(syft, declared):
    """Syft's components, without those the inventories already declare (its binary
    classifiers read FFmpeg's library versions as FFmpeg's, for example)."""
    names = {item['name'].lower() for item in declared}
    names |= {prop['value'].lower() for item in declared for prop in item['properties'] if prop['name'] == 'putgd:sourcePackage' and prop['value']}
    names |= {'qtbase', 'qt', 'ffmpeg', 'putgd'}
    kept = []
    for item in syft.get('components', []):
        if item.get('type') == 'file' or item.get('name', '').lower() in names:
            continue
        item = dict(item)
        item.setdefault('properties', []).append({'name': 'putgd:origin', 'value': 'detected'})
        kept.append(item)
    return kept


def generate(package, label, syft, output, timestamp, version):
    if label not in LABELS:
        raise ValueError('Unknown package label: ' + label)
    with tempfile.TemporaryDirectory(prefix='putgd-sbom-') as work:
        work = Path(work)
        root = extract(package, label, work)
        catalog = work / 'syft.cdx.json'
        subprocess.run([str(syft), 'scan', 'dir:' + str(root), '--source-name', 'putgd-' + label, '--source-version', version,
                        '-o', 'cyclonedx-json=' + str(catalog), '-q'], check=True,
                       env={**os.environ, 'SYFT_CHECK_FOR_APP_UPDATE': 'false'})
        syft_bom = json.loads(catalog.read_text())
        declared = appimage_components(root) if label == 'appimage' else native_components(root)
    digest = sha256(package)
    components = dedupe(declared) + merge_syft(syft_bom, declared)
    tools = syft_bom.get('metadata', {}).get('tools', {})
    tools.setdefault('components', []).append({'type': 'application', 'name': 'putgd sbom.py', 'version': '1'})
    bom = {'bomFormat': 'CycloneDX', 'specVersion': syft_bom.get('specVersion', '1.6'),
           'serialNumber': 'urn:uuid:' + str(uuid.uuid5(uuid.NAMESPACE_URL, 'https://put.gd/sbom/' + digest)), 'version': 1,
           'metadata': {'timestamp': datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
                        'tools': tools,
                        'component': {'type': 'application', 'bom-ref': 'putgd-package', 'name': package.name, 'version': version,
                                      'hashes': [{'alg': 'SHA-256', 'content': digest}],
                                      'properties': [{'name': 'putgd:label', 'value': label},
                                                     {'name': 'putgd:distro', 'value': DISTROS[label]}]}},
           'components': components}
    output.write_text(json.dumps(bom, indent=1, sort_keys=False) + '\n')
    origins = {}
    for item in components:
        origin = next(prop['value'] for prop in item['properties'] if prop['name'] == 'putgd:origin')
        origins[origin] = origins.get(origin, 0) + 1
    return {'label': label, 'file': output.name, 'sha256': sha256(output), 'bytes': output.stat().st_size,
            'package': package.name, 'packageSha256': digest, 'components': len(components), 'origins': origins}


def scan(grype, sboms, output):
    results = {}
    for path in sboms:
        bom = json.loads(path.read_text())
        properties = {prop['name']: prop['value'] for prop in bom['metadata']['component']['properties']}
        target = output / (path.name.removesuffix('.cdx.json') + '.grype.json')
        subprocess.run([str(grype), 'sbom:' + str(path), '--distro', properties['putgd:distro'], '-o', 'json', '--file', str(target), '-q'],
                       check=True, env={**os.environ, 'GRYPE_CHECK_FOR_APP_UPDATE': 'false', 'GRYPE_DB_AUTO_UPDATE': 'false'})
        results[properties['putgd:label']] = target
    return results


def load_policy(path=POLICY):
    policy = json.loads(path.read_text())
    if policy.get('schemaVersion') != 1 or not set(policy['severities']) <= set(SEVERITIES):
        raise ValueError('Unsupported vulnerability policy')
    for entry in policy['ignore']:
        missing = {'id', 'component', 'reason', 'expires'} - set(entry)
        if missing or not entry['reason'].strip():
            raise ValueError('Ignore entries need id, component, a reason and an expiry: ' + json.dumps(entry))
        datetime.date.fromisoformat(entry['expires'])
    return policy


def channel(release_tag):
    if not release_tag:
        return 'development'
    return 'nightly' if release_tag.endswith('-nightly') else 'stable'


def findings(bom_path, grype_path):
    bom = json.loads(bom_path.read_text())
    label = next(prop['value'] for prop in bom['metadata']['component']['properties'] if prop['name'] == 'putgd:label')
    origin = {}
    for item in bom['components']:
        value = next((prop['value'] for prop in item.get('properties', []) if prop['name'] == 'putgd:origin'), 'detected')
        origin[item.get('purl')] = value
        origin[(item['name'], item.get('version'))] = value
    found = []
    for match in json.loads(grype_path.read_text()).get('matches', []):
        artifact, vulnerability = match['artifact'], match['vulnerability']
        fix = vulnerability.get('fix') or {}
        found.append({'id': vulnerability['id'], 'severity': vulnerability.get('severity') or 'Unknown',
                      'aliases': sorted({item['id'] for item in match.get('relatedVulnerabilities', []) if item['id'] != vulnerability['id']}),
                      'component': artifact['name'], 'version': artifact.get('version', ''),
                      'origin': origin.get(artifact.get('purl')) or origin.get((artifact['name'], artifact.get('version'))) or 'detected',
                      'fixState': fix.get('state', 'unknown'), 'fixedIn': fix.get('versions', []), 'package': label})
    return found


def classify(items, policy, mode, today):
    """Mark each finding blocking, ignored (until a date) or reported."""
    blocks = policy['block'].get(mode, False)
    for item in items:
        names = {item['id'], *item['aliases']}
        rule = next((entry for entry in policy['ignore'] if entry['id'] in names and entry['component'] in (item['component'], '*')), None)
        candidate = (item['origin'] in BUNDLED and item['severity'] in policy['severities']
                     and (item['fixState'] == 'fixed' or not policy.get('requireFix', True)))
        if rule and datetime.date.fromisoformat(rule['expires']) >= today:
            item['status'] = 'accepted until ' + rule['expires']
        elif candidate:
            item['status'] = ('blocking' if blocks else 'flagged') + (' (acceptance expired ' + rule['expires'] + ')' if rule else '')
        else:
            item['status'] = 'reported'
    return blocks


def summary_markdown(receipts, items, database, mode, blocks, limit=150):
    lines = [f'### Vulnerability scan ({mode})', '',
             f'Grype database `{database.get("schemaVersion", "?")}` built {database.get("built", "?")}. '
             f'Policy: `packaging/sbom/policy.json`; findings in bundled code at {", ".join(SEVERITIES[:2])} with a fix are '
             + ('**blocking** on this channel.' if blocks else 'flagged, not blocking, on this channel.'), '',
             '| Package | Components | ' + ' | '.join(SEVERITIES) + ' | Flagged |', '| --- | ---: |' + ' ---: |' * (len(SEVERITIES) + 1)]
    for label, receipt in receipts.items():
        mine = [item for item in items if item['package'] == label]
        counts = [str(sum(1 for item in mine if item['severity'] == severity)) for severity in SEVERITIES]
        flagged = sum(1 for item in mine if item['status'].startswith(('blocking', 'flagged')))
        lines.append(f'| {label} | {receipt["components"]} | ' + ' | '.join(counts) + f' | {flagged} |')
    serious = {}
    for item in items:
        if item['severity'] in ('Critical', 'High'):
            key = (item['id'], item['component'], item['version'])
            serious.setdefault(key, {**item, 'packages': []})['packages'].append(item['package'])
    order = {severity: index for index, severity in enumerate(SEVERITIES)}
    rows = sorted(serious.values(), key=lambda item: (not item['status'].startswith(('blocking', 'flagged')), order[item['severity']], item['component'], item['id']))
    lines += ['', f'#### Critical and high ({len(rows)})', '']
    if rows:
        lines += ['| ID | Severity | Component | Version | Fixed in | Origin | Packages | Status |', '| --- | --- | --- | --- | --- | --- | --- | --- |']
        for item in rows[:limit]:
            lines.append(f'| {item["id"]} | {item["severity"]} | {item["component"]} | {item["version"]} | '
                         f'{", ".join(item["fixedIn"]) or item["fixState"]} | {item["origin"]} | {", ".join(sorted(set(item["packages"])))} | {item["status"]} |')
        if len(rows) > limit:
            lines.append(f'| … {len(rows) - limit} more in the report artifact | | | | | | | |')
    else:
        lines.append('None.')
    return '\n'.join(lines) + '\n'


def report(sbom_dir, grype_dir, receipts, policy, mode, database, today):
    items = []
    for label in receipts:
        items += findings(sbom_dir / receipts[label]['file'], grype_dir / (receipts[label]['file'].removesuffix('.cdx.json') + '.grype.json'))
    blocks = classify(items, policy, mode, today)
    flagged = [item for item in items if item['status'].startswith(('blocking', 'flagged'))]
    return items, flagged, blocks


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    tools = commands.add_parser('tools', help='Download the pinned Syft and Grype')
    tools.add_argument('--output', type=Path, required=True)
    database = commands.add_parser('database', help='Download the current Grype database archive')
    database.add_argument('--output', type=Path, required=True, help='Directory; reuses a verified archive already there')
    database.add_argument('--listing', type=Path, help='Write the listing here (for cache keys)')
    database.add_argument('--listing-only', action='store_true')
    generate_command = commands.add_parser('generate', help='SBOMs of a staged package set')
    generate_command.add_argument('--inputs', type=Path, required=True, help='Downloaded release-* artifacts')
    generate_command.add_argument('--syft', type=Path, required=True)
    generate_command.add_argument('--output', type=Path, required=True)
    generate_command.add_argument('--timestamp', type=int, required=True)
    scan_command = commands.add_parser('scan', help='Grype and the policy over generated SBOMs')
    scan_command.add_argument('--sboms', type=Path, required=True)
    scan_command.add_argument('--grype', type=Path, required=True)
    scan_command.add_argument('--output', type=Path, required=True)
    scan_command.add_argument('--database', type=Path, required=True, help='Listing of the imported database')
    scan_command.add_argument('--release-tag', default='')
    scan_command.add_argument('--policy', type=Path, default=POLICY)
    scan_command.add_argument('--summary', type=Path)
    args = parser.parse_args()
    if args.command == 'tools':
        print(json.dumps(fetch_tools(args.output), indent=2))
    elif args.command == 'database':
        listing = database_listing()
        if args.listing:
            args.listing.write_text(json.dumps(listing, indent=2) + '\n')
        if not args.listing_only:
            args.output.mkdir(parents=True, exist_ok=True)
            archive = args.output / listing['name']
            if not archive.is_file() or sha256(archive) != listing['sha256']:
                download(listing['url'], archive)
                if sha256(archive) != listing['sha256']:
                    raise ValueError('Grype database archive differs from its listed SHA-256')
            for stale in args.output.glob('vulnerability-db_*'):
                if stale != archive:
                    stale.unlink()
        print(json.dumps(listing))
    elif args.command == 'generate':
        args.output.mkdir(parents=True, exist_ok=False)
        receipts = {}
        for label in LABELS:
            directory = args.inputs / ('release-appimage' if label == 'appimage' else 'release-native-' + label)
            if label == 'appimage':
                receipt = json.loads((directory / 'appimage-build.json').read_text())
                package, version = directory / receipt['artifact']['file'], receipt['build']['version']
            else:
                receipt = json.loads((directory / 'native-audit.json').read_text())
                package, version = directory / receipt['package']['file'], receipt['manifest']['version']
                if sha256(package) != receipt['package']['sha256']:
                    raise ValueError('Package differs from its audit receipt: ' + package.name)
            receipts[label] = generate(package, label, args.syft, args.output / ('putgd-' + version + '-' + label + '.cdx.json'),
                                       args.timestamp, version)
        (args.output / 'sbom-receipts.json').write_text(json.dumps({'schemaVersion': 1, 'sboms': receipts}, indent=2) + '\n')
        print(json.dumps(receipts, indent=2))
    else:
        receipts = json.loads((args.sboms / 'sbom-receipts.json').read_text())['sboms']
        args.output.mkdir(parents=True, exist_ok=True)
        scan(args.grype, [args.sboms / receipt['file'] for receipt in receipts.values()], args.output)
        policy, mode = load_policy(args.policy), channel(args.release_tag)
        database = json.loads(args.database.read_text())
        today = datetime.datetime.now(datetime.timezone.utc).date()
        items, flagged, blocks = report(args.sboms, args.output, receipts, policy, mode, database, today)
        (args.output / 'vulnerability-report.json').write_text(json.dumps(
            {'schemaVersion': 1, 'channel': mode, 'releaseTag': args.release_tag, 'database': database,
             'policy': policy, 'findings': items}, indent=1) + '\n')
        markdown = summary_markdown(receipts, items, database, mode, blocks)
        (args.output / 'vulnerability-summary.md').write_text(markdown)
        if args.summary:
            with open(args.summary, 'a') as stream:
                stream.write(markdown)
        level = 'error' if blocks else 'warning'
        for item in flagged[:20]:
            print(f'::{level}::{item["id"]} ({item["severity"]}) in {item["component"]} {item["version"]} '
                  f'({item["package"]}), fixed in {", ".join(item["fixedIn"]) or "?"}')
        print(f'{len(items)} findings, {len(flagged)} flagged; channel {mode} ' + ('blocks' if blocks else 'does not block'))
        if blocks and flagged:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
