"""inspect_tree.py DIR: check each extracted package tree in DIR/x against its
release-builds.json receipt and for stray, debug, secret and build-path content.

Writes DIR/inspect.json. Run with python3 -I.
"""
import hashlib, json, os, re, stat, subprocess, sys

root = sys.argv[1]
receipts = json.load(open(os.path.join(root, 'github', 'release-builds.json')))
SECRET = re.compile(rb'AKIA[0-9A-Z]{16}|ASIA[0-9A-Z]{16}|-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----'
                    rb'|ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{40,}|xox[abpr]-[A-Za-z0-9-]{10,}|sk-[A-Za-z0-9]{32,}'
                    rb'|aws_secret_access_key\s*=\s*\S+|AIza[0-9A-Za-z_-]{35}')
BUILDPATH = re.compile(rb'/home/runner/work[^\x00\s"\']*|/__w/[^\x00\s"\']*|/home/patrick[^\x00\s"\']*|/builddir/build[^\x00\s"\']*')
STRAY = re.compile(r'\.(a|la|o|h|hpp|pc|cmake|pyc|orig|rej|swp|debug)$|(^|/)(tests?|CMakeFiles|__pycache__)(/|$)|~$')


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def readelf(path, *args):
    return subprocess.run(['readelf', '-W', *args, path], capture_output=True, text=True).stdout


result = {}
for label in sorted(os.listdir(os.path.join(root, 'x'))):
    tree = os.path.join(root, 'x', label)
    files, links, elves = {}, {}, {}
    secrets, buildpaths, stray, control = [], {}, [], {}
    for dirpath, dirnames, filenames in os.walk(tree):
        rel_dir = os.path.relpath(dirpath, tree)
        if rel_dir.split(os.sep)[0] == 'DEBIAN':
            for name in filenames:
                control[name] = open(os.path.join(dirpath, name), errors='replace').read()
            continue
        for name in dirnames + filenames:
            path = os.path.join(dirpath, name)
            rel = os.path.normpath(os.path.join(rel_dir, name))
            st = os.lstat(path)
            if stat.S_ISLNK(st.st_mode):
                links[rel] = os.readlink(path)
                continue
            if not stat.S_ISREG(st.st_mode):
                continue
            files[rel] = {'size': st.st_size, 'mode': '%04o' % stat.S_IMODE(st.st_mode), 'sha256': sha(path)}
            if STRAY.search(rel):
                stray.append(rel)
            data = open(path, 'rb').read()
            for m in SECRET.finditer(data):
                secrets.append({'file': rel, 'match': m.group(0)[:40].decode(errors='replace')})
            found = sorted({m.group(0)[:160].decode(errors='replace') for m in BUILDPATH.finditer(data)})
            if found:
                buildpaths[rel] = {'count': len(found), 'examples': found[:4]}
            if data[:4] == b'\x7fELF':
                dyn = readelf(path, '-d')
                sections = readelf(path, '-S')
                notes = readelf(path, '-n')
                elves[rel] = {
                    'runpath': re.findall(r'\(RUNPATH\).*\[(.*)\]', dyn),
                    'rpath': re.findall(r'\(RPATH\).*\[(.*)\]', dyn),
                    'needed': re.findall(r'\(NEEDED\).*\[(.*)\]', dyn),
                    'soname': re.findall(r'\(SONAME\).*\[(.*)\]', dyn),
                    'debugSections': sorted(set(re.findall(r'\] (\.debug_\w+|\.zdebug_\w+)', sections))),
                    'symtab': '.symtab' in sections,
                    'buildId': (re.findall(r'Build ID: ([0-9a-f]+)', notes) or [None])[0],
                }
    receipt = receipts['receipts'][label]
    want = {e['path']: e for e in receipt['inventory']}
    have = {p: f for p, f in files.items()}
    mismatched = [p for p in want if p in have and (want[p].get('sha256') != have[p]['sha256'] or want[p].get('size') != have[p]['size'])]
    missing = [p for p in want if p not in have and p not in links]
    unlisted = [p for p in have if p not in want]
    result[label] = {
        'files': len(files), 'symlinks': len(links), 'bytes': sum(f['size'] for f in files.values()),
        'receiptInventory': len(want), 'receiptMatched': len([p for p in want if p in have and p not in mismatched]),
        'receiptMismatched': mismatched, 'receiptMissing': missing, 'unlistedInReceipt': unlisted,
        'elf': elves, 'stray': stray, 'secrets': secrets, 'buildPaths': buildpaths, 'control': control,
        'docs': sorted(p for p in files if p.startswith('usr/share/doc/')),
        'licenseFiles': sorted(p for p in files if re.search(r'(LICEN[CS]E|COPYING|copyright|NOTICE)', p, re.I)),
        'tree': {p: f for p, f in sorted(files.items())}, 'links': links,
    }
    print(label, {k: (len(v) if isinstance(v, (list, dict)) else v) for k, v in result[label].items()})
json.dump(result, open(os.path.join(root, 'inspect.json'), 'w'), indent=1)
