#!/usr/bin/env python3
"""#344 lab stable channel on pkg.put.gd/lab, signed only with lab keys.

The signer's lab mode publishes nightlies with the nightly key, and its stable path
checks the real stable key, which stays on the workstation. This script stands in
for the stable side under lab/ only: it builds a stable tree from a lab-key
AppImage build, signs SHA256SUMS and the feed with a lab-only minisign key, and
uploads through pkgrepo.py's own Publisher (same cache headers, phase order,
CloudFront invalidation and CDN readback) with the signer's AWS files from the
environment. Its publisher state is its own; the live signer's config, state and
timer are untouched. `clean` deletes every key this script uploaded.

  lab344_stable.py keys DIR                          lab stable key + a foreign key (no passphrase)
  lab344_stable.py stage TREE --keys DIR [--assets D]  stable tree: release null, or D's v0.1.1 build
  lab344_stable.py publish TREE --state S --src SRC --config CFG
  lab344_stable.py clean --state S --src SRC --config CFG
"""
import argparse, hashlib, json, shutil, subprocess, sys, time
from pathlib import Path

TAG, VERSION = 'v0.1.1', '0.1.1'
NAME = f'putgd-{VERSION}-x86_64.AppImage'
LAB = 'https://pkg.put.gd/lab/'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def sign(key, path, trusted):
    subprocess.run(['minisign', '-S', '-q', '-s', str(key), '-m', str(path), '-x', str(path) + '.minisig', '-t', trusted],
                   check=True, stdin=subprocess.DEVNULL)


def keys(args):
    d = Path(args.dir)
    d.mkdir(mode=0o700, parents=True, exist_ok=False)
    for name in ('lab-stable', 'lab-foreign'):
        subprocess.run(['minisign', '-G', '-W', '-p', str(d / f'{name}.pub'), '-s', str(d / f'{name}.key')], check=True,
                       stdout=subprocess.DEVNULL)
    print(json.dumps({name: (d / f'{name}.pub').read_text().splitlines()[1] for name in ('lab-stable', 'lab-foreign')}))


def stage(args):
    sys.path.insert(0, str(Path(args.src) / 'tools/releases'))
    import update_feed, pkgrepo  # noqa: E402
    tree, key = Path(args.tree), Path(args.keys) / 'lab-stable.key'
    if tree.exists():
        shutil.rmtree(tree)
    (tree / 'update/v1').mkdir(parents=True)
    now = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    release, sums = None, {}
    if args.assets:
        assets = Path(args.assets)
        image, zsync = assets / NAME, assets / (NAME + '.zsync')
        sums = {NAME: sha256(image), NAME + '.zsync': sha256(zsync)}
        # The .zsync must name this release's image on the lab mirror, as the signer checks.
        pkgrepo.appimage_inputs(TAG, image, zsync, sums, LAB, ['SHA256SUMS'])
        target = tree / 'releases' / TAG
        target.mkdir(parents=True)
        shutil.copy2(image, target / NAME); shutil.copy2(zsync, target / (NAME + '.zsync'))
        (target / 'SHA256SUMS').write_text(''.join(f'{sums[n]}  {n}\n' for n in sorted(sums)))
        sign(key, target / 'SHA256SUMS', f'put.gd {TAG} stable')
        (tree / 'dl/stable').mkdir(parents=True)
        shutil.copy2(zsync, tree / 'dl/stable/appimage.zsync')
        release = {'tag_name': TAG, 'published_at': now, 'html_url': f'https://github.com/patrick-hudson/put.gd-desktop/releases/tag/{TAG}',
                   'assets': [{'name': n, 'size': (target / n).stat().st_size,
                               'browser_download_url': f'https://github.com/patrick-hudson/put.gd-desktop/releases/download/{TAG}/{n}'}
                              for n in (NAME, NAME + '.zsync')]}
    document = update_feed.feed('stable', release, sums, now)
    update_feed.check_feed(document)
    feed = tree / 'update/v1/stable.json'
    feed.write_text(json.dumps(document, indent=1, sort_keys=True) + '\n')
    sign(key, feed, f'put.gd {TAG if release else "stable-empty"} stable')
    print(json.dumps({'tree': str(tree), 'files': sorted(str(p.relative_to(tree)) for p in tree.rglob('*') if p.is_file()),
                      'feed': document}, indent=1))


def publisher(args):
    sys.path.insert(0, str(Path(args.src) / 'tools/releases'))
    import pkgrepo  # noqa: E402
    state = Path(args.state)
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    config = pkgrepo.Config.load(Path(args.config), state=str(state))
    s3, cloudfront = pkgrepo.aws_clients(config)
    return pkgrepo, config, pkgrepo.Publisher(config, 'lab', 'stable', s3, cloudfront)


def publish(args):
    pkgrepo, config, pub = publisher(args)
    tree = Path(args.tree)
    for relative in pkgrepo.tree_files(tree):
        assert not relative.startswith(('apt/', 'rpm/')), relative
    if args.dry_run:
        plan = [{'key': pub.prefix + r, 'phase': pkgrepo.phase(r), 'cacheControl': pkgrepo.cache_control(pub.prefix + r),
                 'contentType': pkgrepo.content_type(pub.prefix + r), 'sha256': pkgrepo.sha256_file(path),
                 'unchanged': pub.state['objects'].get(pub.prefix + r) == pkgrepo.sha256_file(path)}
                for r, path in sorted(pkgrepo.tree_files(tree).items())]
        assert all(item['key'].startswith('lab/') for item in plan)
        print(json.dumps({'dryRun': True, 'bucket': config.bucket, 'distribution': config.distribution, 'plan': plan}, indent=1))
        return
    receipt = pkgrepo.new_receipt({'tag': TAG}, 'lab')
    changed = pub.upload(tree, {0, 1, 2, 3}, receipt)
    pub.invalidate(changed, receipt)
    feed_keys = pub.upload(tree, {4}, receipt)
    pub.invalidate(feed_keys, receipt)
    pub.readback(tree, [r for r in pkgrepo.tree_files(tree) if r.startswith(('update/', 'dl/')) or r.endswith('SHA256SUMS')], receipt)
    assert all(item['key'].startswith('lab/') for item in receipt['s3']['uploaded'])
    print(json.dumps(receipt, indent=1))


def clean(args):
    pkgrepo, config, pub = publisher(args)
    keys = sorted(set(pub.state['objects']) | set(pub.state['pending']))
    for key in keys:
        assert key.startswith('lab/'), key
        pub.s3.delete_object(Bucket=config.bucket, Key=key)
    receipt = pkgrepo.new_receipt({'tag': TAG}, 'lab')
    pub.invalidate(keys, receipt)
    pub.state = {'objects': {}, 'pending': {}}
    pub.save()
    print(json.dumps({'deleted': keys, 'invalidations': receipt['invalidations']}, indent=1))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('keys').add_argument('dir')
    a = sub.add_parser('stage'); a.add_argument('tree'); a.add_argument('--keys', required=True); a.add_argument('--assets'); a.add_argument('--src', required=True)
    for name in ('publish', 'clean'):
        a = sub.add_parser(name)
        if name == 'publish':
            a.add_argument('tree'); a.add_argument('--dry-run', action='store_true')
        a.add_argument('--state', required=True); a.add_argument('--src', required=True); a.add_argument('--config', required=True)
    args = p.parse_args()
    {'keys': keys, 'stage': stage, 'publish': publish, 'clean': clean}[args.cmd](args)


if __name__ == '__main__':
    main()
