"""summarize.py DIR OUT: cross-check the receipts and write the compact evidence
files (packages.json, elf.json, lint-tags.json) from the outputs of inspect_tree.py,
repo.py and lint.sh. Run with python3 -I."""
import hashlib, json, os, re, sys

root, out = sys.argv[1:3]
os.makedirs(out, exist_ok=True)
builds = json.load(open(os.path.join(root, 'github', 'release-builds.json')))
inspect = json.load(open(os.path.join(root, 'inspect.json')))
release = builds['release']
debug_ids = json.dumps(builds['receipts'].get('debugSymbols', {}))
sums = dict(reversed(line.split()) for line in open(os.path.join(root, 'github', 'SHA256SUMS')))
packages = {}
for label, tree in inspect.items():
    receipt = builds['receipts'][label]
    ext = 'rpm' if label == 'fedora44' else 'deb'
    name = f"putgd-{release['version']}-{label}-x86_64.{ext}"
    path = os.path.join(root, 'github', name)
    digest = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    build_info = json.load(open(os.path.join(root, 'x', label, 'usr/share/putgd/build-info.json')))
    elves = tree['elf']
    packages[label] = {
        'asset': name, 'sha256': digest, 'bytes': os.path.getsize(path),
        'inSHA256SUMS': sums.get(name) == digest,
        'receiptPackageSha256': receipt['package']['sha256'] == digest,
        'receiptPackage': {k: receipt['package'][k] for k in ('file', 'version', 'size')},
        'receiptManifestEqualsBuildInfo': receipt['manifest'] == build_info,
        'buildInfo': build_info,
        'releaseCommit': release['gitCommit'] == build_info['gitCommit'],
        'inventory': {'receipt': tree['receiptInventory'], 'matched': tree['receiptMatched'] + tree['symlinks'],
                      'mismatched': tree['receiptMismatched'], 'missing': tree['receiptMissing'],
                      'unlisted': tree['unlistedInReceipt']},
        'files': tree['files'], 'symlinks': tree['symlinks'], 'unpackedBytes': tree['bytes'],
        'elfFiles': len(elves),
        'buildIdsInDebugReceipt': sum(1 for e in elves.values() if e['buildId'] and e['buildId'] in debug_ids),
        'runpaths': sorted({f"{'RPATH' if e['rpath'] else 'RUNPATH'}={','.join(e['rpath'] or e['runpath'])}"
                            for e in elves.values() if e['rpath'] or e['runpath']}),
        'elfWithDebugSections': sorted(p for p, e in elves.items() if e['debugSections']),
        'elfWithSymtab': sum(1 for e in elves.values() if e['symtab']),
        'stray': tree['stray'], 'secrets': tree['secrets'],
        'buildPathFiles': {p: v['count'] for p, v in tree['buildPaths'].items()},
        'docs': tree['docs'],
        'proton': {p: tree['tree'][p]['sha256'] for p in tree['tree'] if 'proton/THIRD-PARTY-NOTICES.txt' in p},
        'control': {k: v for k, v in tree['control'].items() if k != 'md5sums'},
    }
json.dump({'release': release, 'packages': packages}, open(os.path.join(out, 'packages.json'), 'w'), indent=1)
json.dump({label: tree['elf'] for label, tree in inspect.items()}, open(os.path.join(out, 'elf.json'), 'w'), indent=1)
tags = {}
for name in sorted(os.listdir(os.path.join(root, 'lint'))):
    text = open(os.path.join(root, 'lint', name), errors='replace').read()
    found = re.findall(r'^([EWIPX]): putgd[^:]*: ([a-z0-9-]+)', text, re.M) or \
        re.findall(r'^putgd[^:]*: ([EW]): ([a-z0-9%-]+)', text, re.M)
    counts = {}
    for level, tag in found:
        counts[f'{level} {tag}'] = counts.get(f'{level} {tag}', 0) + 1
    tags[name] = {'tool': text.splitlines()[0] if text else '', 'tags': dict(sorted(counts.items()))}
json.dump(tags, open(os.path.join(out, 'lint-tags.json'), 'w'), indent=1)
for label, p in packages.items():
    print(label, {k: v for k, v in p.items() if k in ('sha256', 'inSHA256SUMS', 'receiptPackageSha256', 'receiptManifestEqualsBuildInfo',
                                                      'releaseCommit', 'inventory', 'elfFiles', 'buildIdsInDebugReceipt', 'runpaths',
                                                      'elfWithDebugSections', 'stray', 'secrets', 'proton')})
