#!/usr/bin/env python3
"""Audit downloaded installers-v1 release bytes, sources and stable CVE policy.

Run on a builder: python3 tools/releases/audit_assets.py --tag TAG --download
--directory /short/private/assets --output /short/private/audit. Downloads use gh,
including linked corresponding sources. The output directory must be new. Raw
findings are retained even on failure. This is no legal or desktop acceptance.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import xml.etree.ElementTree as ET
from urllib.parse import unquote, urlsplit

import appimage_complete_sources
import appimage_notices
from audit_native import sha256, validate_inventory
from audit_proton import audit_proton
from collect import identity_matches, verified_source
from native import package_metadata
import proton_sources
from release_identity import parse_tag, calendar_base
import sbom
import source_archive
import support_assets

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = 'patrick-hudson/put.gd-desktop'


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def run(*args, **kwargs):
    return subprocess.check_output([str(a) for a in args], text=True, **kwargs).strip()


def checksums(directory, expected):
    """Require the positive inventory and all checksum lines, including sources."""
    if {p.name for p in directory.iterdir()} != set(expected):
        raise ValueError('Downloaded inventory differs from expected assets')
    records = {}
    for line in (directory / 'SHA256SUMS').read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]*)', line)
        if not match or match[2] in records:
            raise ValueError('Invalid or duplicate checksum entry')
        records[match[2]] = match[1]
    if set(records) != set(expected) - {'SHA256SUMS'}:
        raise ValueError('Incomplete checksum inventory')
    for name, digest in records.items():
        path = directory / name
        if path.is_symlink() or not path.is_file() or sha256(path) != digest:
            raise ValueError('Checksum mismatch: ' + name)
    run('sha256sum', '--check', '--strict', 'SHA256SUMS', cwd=directory)
    return {name: {'sha256': sha256(directory / name), 'bytes': (directory / name).stat().st_size}
            for name in sorted(expected)}


def remote_snapshot(repository, tag, directory, expected):
    # Release-by-tag omits drafts. The authenticated list includes them; bind
    # the selected ID before reading assets so the stable prepublication gate works.
    page = 1
    while True:
        releases = json.loads(run('gh', 'api', f'repos/{repository}/releases?per_page=100&page={page}'))
        matches = [item for item in releases if item['tag_name'] == tag]
        if len(matches) > 1:
            raise ValueError('Ambiguous release tag')
        if matches:
            metadata = json.loads(run('gh', 'api', f'repos/{repository}/releases/{matches[0]["id"]}'))
            break
        if len(releases) < 100:
            raise ValueError('Release tag not found in authenticated inventory')
        page += 1
    assets = metadata['assets']
    if metadata['tag_name'] != tag or {a['name'] for a in assets} != set(expected) or len(assets) != len(expected):
        raise ValueError('Remote release asset inventory differs from downloads')
    for asset in assets:
        path = directory / asset['name']
        if (asset['state'] != 'uploaded' or asset['size'] != path.stat().st_size
                or (asset.get('digest') is not None and asset['digest'] != 'sha256:' + sha256(path))):
            raise ValueError('Remote asset differs from downloaded bytes: ' + asset['name'])
    return support_assets.snapshot(metadata)


def release_identity(root, tag, identity):
    parsed = parse_tag(tag)
    commit = run('git', '-C', root, 'rev-parse', 'refs/tags/' + tag + '^{commit}')
    tree = run('git', '-C', root, 'rev-parse', commit + '^{tree}')
    count = int(run('git', '-C', root, 'rev-list', '--count', commit))
    stamp = int(run('git', '-C', root, 'show', '-s', '--format=%ct', commit))
    if (identity['tag'] != tag or identity['version'] != parsed.version
            or identity['gitCommit'] != commit or identity['gitTree'] != tree
            or identity['commitCount'] != count or parsed.base_version != calendar_base(stamp, count)):
        raise ValueError('Release identity differs from tag commit/tree/count/date')
    return stamp


def source_export(path, receipt):
    """Verify every curated source member and reject non-production paths."""
    prefix = receipt['archiveRoot'] + '/'
    with tarfile.open(path) as archive:
        info = json.load(archive.extractfile(prefix + 'source-info.json'))
        if info['identity'] != receipt['identity'] or info['files'] != [item for item in receipt.get('members', info['files']) if item['path'] != 'source-info.json']:
            raise ValueError('Source export identity mismatch')
        expected = {prefix + item['path']: item for item in info['files']}
        seen = set()
        for member in archive:
            if member.isdir():
                continue
            if not member.isfile() or member.name in seen:
                raise ValueError('Unsafe/duplicate source export member')
            seen.add(member.name)
            if member.name == prefix + 'source-info.json':
                continue
            item = expected.get(member.name)
            if item is not None:
                source_archive._safe_path(item['path'])
            if (item is None or not (source_archive._included(item['path']) or item['path'] == 'README_SOURCE.md')
                    or member.size != item['bytes']
                    or hashlib.file_digest(archive.extractfile(member), 'sha256').hexdigest() != item['sha256']):
                raise ValueError('Source export file mismatch: ' + member.name)
        if seen != set(expected) | {prefix + 'source-info.json'}:
            raise ValueError('Missing source export member')
    return {'files': len(expected), 'identity': info['identity'], 'sha256': sha256(path)}


def download_part(item, repository, directory):
    url = urlsplit(item['url'])
    prefix = '/' + repository + '/releases/download/'
    tail = url.path.removeprefix(prefix).split('/')
    if (url.scheme != 'https' or url.netloc != 'github.com' or url.query or url.fragment
            or not url.path.startswith(prefix) or len(tail) != 2 or unquote(tail[1]) != item['file']):
        raise ValueError('Invalid Proton source delivery URL')
    run('gh', 'release', 'download', unquote(tail[0]), '--repo', repository,
        '--pattern', item['file'], '--dir', directory)


def verify_rebuilt_proton(built, catalog):
    if built is not None and (built.get('rebuiltProton', {}).get('sha256') != catalog['helperSha256']
            or built.get('rebuiltProton', {}).get('bundledSha256') != catalog['helperSha256']):
        raise ValueError('Source rebuild Proton helper differs from release catalog')


def version_in_range(version, expression):
    """The numeric comparator ranges in Bun's published advisories; fail closed otherwise."""
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('Unsupported embedded Bun version')
    selected = tuple(map(int, version.split('.')))
    for alternative in expression.split('||'):
        clauses = alternative.replace(',', ' ').split()
        if not clauses:
            raise ValueError('Empty upstream advisory version range')
        matches = []
        for clause in clauses:
            match = re.fullmatch(r'(>=|<=|>|<|=)?(\d+\.\d+\.\d+)', clause)
            if not match:
                raise ValueError('Upstream advisory version range needs manual review: ' + expression)
            other = tuple(map(int, match[2].split('.')))
            matches.append({'>=': selected >= other, '<=': selected <= other,
                            '>': selected > other, '<': selected < other, '=': selected == other}[match[1] or '='])
        if all(matches):
            return True
    return False


def bun_advisories(versions):
    # Compiled Bun includes its runtime and trust store. The official advisories
    # can precede Grype's database; a scanner miss is not an acceptance.
    # Distro gh 2.46 supports --paginate but not --slurp. Decode each page
    # directly so the audit uses the standard system package on d69 too.
    raw = run('gh', 'api', '--paginate', 'repos/oven-sh/bun/security-advisories?per_page=100')
    pages = []
    while raw.strip():
        page, end = json.JSONDecoder().raw_decode(raw.lstrip())
        pages.append(page)
        raw = raw.lstrip()[end:]
    findings = []
    for advisory in (item for page in pages for item in page):
        if advisory.get('withdrawn_at'):
            continue
        for vulnerability in advisory['vulnerabilities']:
            if vulnerability['package']['name'] != 'bun':
                continue
            for version, packages in versions.items():
                if version_in_range(version, vulnerability['vulnerable_version_range']):
                    findings.append({key: advisory.get(key) for key in
                        ('ghsa_id', 'cve_id', 'severity', 'summary', 'html_url', 'published_at')} |
                        {'component': 'bun', 'version': version, 'packages': packages,
                         'vulnerableRange': vulnerability['vulnerable_version_range'],
                         'fixedIn': vulnerability.get('patched_versions')})
    return findings


def audit(args):
    directory, output = args.directory.resolve(), args.output.resolve()
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    result = {'schemaVersion': 1, 'tag': args.tag, 'passed': False,
              'date': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'host': run('hostname'), 'os': Path('/etc/os-release').read_text(),
              'auditorCommit': run('git', '-C', ROOT, 'rev-parse', 'HEAD'),
              'auditorScriptSha256': sha256(Path(__file__)),
              'scope': 'Exact downloaded bytes, notices, provenance, corresponding source and stable CVE policy; no legal, live-service or desktop acceptance',
              'checks': {}, 'errors': []}
    checks = result['checks']
    try:
        if args.download:
            directory.mkdir(parents=True, mode=0o700, exist_ok=False)
            run('gh', 'release', 'download', args.tag, '--repo', args.repository, '--dir', directory)
        print('Verifying installer inventory and identity', flush=True)
        builds = json.loads((directory / 'release-builds.json').read_text())
        if builds.get('assetLayout') != support_assets.LAYOUT:
            raise ValueError('Expected installers-v1 release layout')
        release, receipts = builds['release'], builds['receipts']
        checks['assets'] = checksums(directory, support_assets.inventory(release['version']))
        checks['remoteAssets'] = remote_snapshot(args.repository, args.tag, directory, checks['assets'])
        stamp = release_identity(args.source_root, args.tag, release)
        checks['identity'] = release
        checks['buildInputs'] = {name: json.loads(run('git', '-C', args.source_root, 'show',
            release['gitCommit'] + ':' + name)) for name in (
                'cmake/dependency-lock.json', 'packaging/appimage/tools.lock.json',
                'packaging/appimage/media.json', 'tools/proton/runtime-lock.json')}

        delivery = json.loads((directory / 'source-delivery.json').read_text())
        support_assets.validate(delivery, release, args.repository)
        if delivery != builds['sources']:
            raise ValueError('Source delivery manifest copies differ')
        sources = output / 'sources'
        sources.mkdir()
        if args.reuse_downloads:
            for name in delivery['assets']:
                cached = args.reuse_downloads / 'sources' / name
                if cached.is_symlink() or not cached.is_file():
                    raise ValueError('Invalid cached source download')
                os.link(cached, sources / name)
        else:
            run('gh', 'release', 'download', delivery['tag'], '--repo', args.repository, '--dir', sources)
        checks['sourceAssets'] = checksums(sources, delivery['assets'])
        if checks['sourceAssets'] != delivery['assets']:
            raise ValueError('Linked source assets differ from installer binding')
        checks['remoteSources'] = remote_snapshot(args.repository, delivery['tag'], sources, delivery['assets'])
        source_receipt = receipts['source']
        write(sources / 'source-archive.json', source_receipt)
        built = (json.loads(args.source_build_result.read_text()) if args.source_build_result
                 else receipts['sourceBuild'])
        if built is not None:
            write(sources / 'source-build-results.json', built)
        archive, _, built = verified_source(sources, release, require_build=built is not None)
        checks['sourceExport'] = source_export(archive, source_receipt)
        checks['sourceRebuild'] = {'kind': 'separate exact-source rebuild' if args.source_build_result else 'CI receipt', 'receipt': built}
        if built is None:
            result['errors'].append('No exact-source production rebuild receipt')
        print('Verifying complete corresponding source', flush=True)
        appimage_complete_sources.verify(sources, receipts['appimageCompleteSource'], receipts['appimage'])
        checks['completeSource'] = receipts['appimageCompleteSource']
        catalog = proton_sources.catalog_value(directory / proton_sources.CATALOG_NAME)
        verify_rebuilt_proton(built, catalog)
        print('Verifying Proton source transport and packets', flush=True)
        parts = output / 'proton-sources'
        parts.mkdir()
        for item in catalog['delivery']['parts']:
            if args.reuse_downloads:
                cached = args.reuse_downloads / 'proton-sources' / item['file']
                if cached.is_symlink() or not cached.is_file():
                    raise ValueError('Invalid cached Proton download')
                os.link(cached, parts / item['file'])
            else:
                download_part(item, args.repository, parts)
        proton_sources.verify(parts, catalog)
        checks['protonSource'] = catalog
        regenerated = output / 'sboms'
        regenerated.mkdir()
        tools_dir = output / 'tools'
        checks['tools'] = sbom.fetch_tools(tools_dir)
        checks['packages'] = {}
        checks['dependencies'] = {}
        bun_versions = {}
        generated = {}
        for label in sbom.LABELS:
            print('Auditing package and regenerating SBOM: ' + label, flush=True)
            receipt = receipts[label]
            appimage = label == 'appimage'
            artifact = receipt['artifact'] if appimage else receipt['package']
            filename = (artifact['file'] if appimage else
                        f'putgd-{release["version"]}-{label}-x86_64' + ('.rpm' if label == 'fedora44' else '.deb'))
            package = directory / filename
            if sha256(package) != artifact['sha256']:
                raise ValueError('Package differs from CI audit: ' + label)
            expected = receipt['build'] if appimage else receipt['manifest']
            if not identity_matches(expected, release):
                raise ValueError('Package receipt identity differs from release')
            work = output / label
            work.mkdir()
            root = sbom.extract(package, label, work)
            proton = audit_proton(root, required=True)
            if (proton['runtimeSha256'] != catalog['helperSha256']
                    or proton['sourceCatalogSha256'] != sha256(directory / proton_sources.CATALOG_NAME)):
                raise ValueError('Packaged Proton runtime/catalog differs from supplied source')
            if 'Package: @openpgp/web-stream-tools ' not in (root / 'usr/libexec/putgd/proton/THIRD-PARTY-NOTICES.txt').read_text():
                raise ValueError('Direct OpenPGP web-stream-tools dependency missing from notices')
            bun_versions.setdefault(proton['manifest']['bunVersion'], []).append(label)
            if appimage:
                observed = appimage_notices.audit(root)
                actual_files = {p.relative_to(root).as_posix(): sha256(p) for p in root.rglob('*') if p.is_file()}
                if actual_files != {i['path']: i['sha256'] for i in receipt['inventory']}:
                    raise ValueError('AppImage file hashes differ from deployed CI audit')
                if json.loads((root / 'usr/share/putgd/build-info.json').read_text()) != expected:
                    raise ValueError('AppImage installed manifest differs from release')
                environment = {**os.environ, 'LD_LIBRARY_PATH': str(root / 'usr/lib') + ':' + str(root / 'usr/lib/putgd')}
                observed['compiledIdentities'] = {}
                for program in receipt['postDeploymentAudit']['identityMatchedPrograms']:
                    identity = json.loads(run(root / 'usr/bin' / program, '--version-json', env=environment, timeout=30))
                    if identity != expected:
                        raise ValueError('AppImage compiled identity differs from release: ' + program)
                    observed['compiledIdentities'][program] = identity
                if not (root / 'usr/share/doc/putgd/APPIMAGE-GPL-NOTICE.txt').is_file():
                    raise ValueError('Missing combined-work GPL notice')
            else:
                observed = validate_inventory(root, expected, require_proton=True)
                observed['metadata'] = package_metadata(package, 'DEB' if package.suffix == '.deb' else 'RPM')
                found_license = ET.parse(root / appimage_notices.METAINFO).getroot().findtext('project_license')
                if set(re.findall(r'[A-Za-z0-9.+-]+', found_license or '')) != {'MIT', 'AND', 'Apache-2.0', 'GPL-3.0-or-later'}:
                    raise ValueError('Native AppStream license differs from bundled components')
                if label == 'fedora44':
                    observed['metadata']['license'] = run('rpm', '-qp', '--qf', '%{LICENSE}', package)
                    if observed['metadata']['license'] != 'MIT AND Apache-2.0 AND GPL-3.0-or-later':
                        raise ValueError('RPM license metadata differs from bundled components')
                # Recheck CI's compiled identities against the extracted file hashes.
                current = {i['path']: i['sha256'] for i in observed['inventory']}
                if current != {i['path']: i['sha256'] for i in receipt['inventory']}:
                    raise ValueError('Native inventory differs from compiled-identity CI audit')
            checks['packages'][label] = {'sha256': sha256(package), 'audit': observed, 'proton': proton}
            name = f'putgd-{release["version"]}-{label}.cdx.json'
            published = json.loads((directory / name).read_text())
            if (published['metadata']['component']['hashes'] != [{'alg': 'SHA-256', 'content': sha256(package)}]
                    or sha256(directory / name) != receipts['sbom'][label]['sha256']):
                raise ValueError('Published SBOM binding mismatch')
            generated[label] = sbom.generate(package, label, tools_dir / 'syft', regenerated / name, stamp, release['version'])
            fresh = json.loads((regenerated / name).read_text())
            # Compare declarations including actual library/absent-tool fingerprints;
            # Syft observations may legitimately change with cataloguer versions.
            declared = sbom.dedupe(sbom.appimage_components(root) if appimage else sbom.native_components(root))
            if any(item not in published['components'] for item in declared):
                raise ValueError('Published SBOM declarations differ from actual package: ' + label)
            checks['dependencies'][label] = fresh['components']
        policy = sbom.load_policy()
        upstream = bun_advisories(bun_versions)
        checks['upstreamAdvisories'] = upstream
        if any(item['severity'].capitalize() in policy['severities']
               and (item['fixedIn'] or not policy.get('requireFix', True)) for item in upstream):
            result['errors'].append('Official Bun advisories exceed the stable threshold; see upstreamAdvisories')
        write(regenerated / 'sbom-receipts.json', {'schemaVersion': 1, 'sboms': generated})
        print('Scanning with a fresh vulnerability database and stable policy', flush=True)
        db = sbom.database_listing()
        checks['database'] = db
        dbdir = output / 'database'
        dbdir.mkdir()
        dbarchive = dbdir / db['name']
        sbom.download(db['url'], dbarchive)
        if sha256(dbarchive) != db['sha256']:
            raise ValueError('Vulnerability database checksum mismatch')
        os.environ['GRYPE_DB_CACHE_DIR'] = str(dbdir / 'cache')
        os.environ['GRYPE_DB_AUTO_UPDATE'] = 'false'
        run(tools_dir / 'grype', 'db', 'import', dbarchive)
        write(output / 'database.json', db)
        grype = output / 'grype'
        grype.mkdir()
        sbom.scan(tools_dir / 'grype', list(regenerated.glob('*.cdx.json')), grype)
        today = datetime.datetime.now(datetime.timezone.utc).date()
        items, flagged, blocks = sbom.report(regenerated, grype, generated, policy, 'stable', db, today)
        write(output / 'vulnerability-report.json', {'releaseTag': args.tag, 'policyChannel': 'stable',
              'database': db, 'policy': policy, 'findings': items})
        (output / 'vulnerability-summary.md').write_text(sbom.summary_markdown(generated, items, db, 'stable', blocks))
        checks['vulnerabilities'] = {'findings': len(items), 'blocking': len(flagged),
                                    'accepted': sum(i['status'].startswith('accepted until ') for i in items)}
        if flagged:
            result['errors'].append('Stable policy has unexplained blocking vulnerabilities; see vulnerability-report.json')
        if checks['remoteAssets'] != remote_snapshot(args.repository, args.tag, directory, checks['assets']):
            raise ValueError('Installer release changed during the audit')
        if checks['remoteSources'] != remote_snapshot(args.repository, delivery['tag'], sources, delivery['assets']):
            raise ValueError('Source release changed during the audit')
        result['passed'] = not result['errors']
    except Exception as error:
        result['errors'].append(str(error))
    finally:
        write(output / 'audit.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--repository', default=REPOSITORY)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--download', action='store_true')
    parser.add_argument('--reuse-downloads', type=Path, help='Previous audit directory; reverify its source bytes without downloading them again')
    parser.add_argument('--source-build-result', type=Path, help='Fresh check_source_build.py results for these exact source bytes')
    parser.add_argument('--source-root', type=Path, default=ROOT, help='Full Git history containing the audited tag')
    args = parser.parse_args()
    result = audit(args)
    print(json.dumps({'passed': result['passed'], 'errors': result['errors']}))
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
