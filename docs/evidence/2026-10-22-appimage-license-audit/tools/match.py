"""match.py IMAGE_BUILDIDS DEBDIR -> report which image ELFs match a current-candidate deb by build ID."""
import json, os, subprocess, sys, tempfile, collections
image = json.load(open(sys.argv[1])); debdir = sys.argv[2]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
bmap = {}; versions = {}; files_by_name = collections.defaultdict(list)
for deb in sorted(os.listdir(debdir)):
    if not deb.endswith('.deb'): continue
    pkg, ver = subprocess.run(['dpkg-deb', '-W', '--showformat', '${Package} ${Version}', os.path.join(debdir, deb)], capture_output=True, text=True).stdout.split()
    src = subprocess.run(['dpkg-deb', '-W', '--showformat', '${source:Package} ${source:Version}', os.path.join(debdir, deb)], capture_output=True, text=True).stdout.split()
    versions[pkg] = {'version': ver, 'source': src[0], 'sourceVersion': src[1]}
    with tempfile.TemporaryDirectory() as t:
        subprocess.run(['dpkg-deb', '-x', os.path.join(debdir, deb), t], check=True)
        out = json.loads(subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'buildids.py'), t], capture_output=True, text=True).stdout)
        for rel, bid in out.items():
            if bid: bmap[bid] = (pkg, rel)
            files_by_name[os.path.basename(rel)].append(pkg)
res = {'matched': {}, 'unmatchedSameName': {}, 'other': []}
for rel, bid in sorted(image.items()):
    if bid in bmap:
        res['matched'][rel] = bmap[bid][0]
    elif os.path.basename(rel) in files_by_name:
        res['unmatchedSameName'][rel] = files_by_name[os.path.basename(rel)]
    else:
        res['other'].append(rel)
res['packages'] = versions
json.dump(res, sys.stdout, indent=1, sort_keys=True)
