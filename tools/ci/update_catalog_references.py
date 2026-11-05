#!/usr/bin/env python3
"""Rewrite the UI catalog's reference images from a render.

Only images that differ beyond the tolerance (tools/ci/image_diff.py) are
rewritten, so rendering noise never creates a commit. New states get a
reference; references of states the catalog no longer renders are deleted.

The render must come from the pinned environment, the Ubuntu 26.04 CI leg.
Take it from a CI run (its `ui-catalog-check` artifact, uploaded when a
ui_catalog test fails), from a directory holding dark/ and light/ renders, or
render it now with --catalog in a matching ubuntu:26.04 container:

    update_catalog_references.py --run RUN_ID
    update_catalog_references.py --from DIR
    update_catalog_references.py --catalog build/dev/putgd-ui-catalog

--dry-run lists the changes without writing them.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_catalog_references as check  # noqa: E402
import image_diff  # noqa: E402

ARTIFACT = 'ui-catalog-check'


def plan(rendered, references, tolerance=check.TOLERANCE):
    """(added, changed, removed) state names for one scheme."""
    actual, expected = check.states(rendered), check.states(references)
    exempt = set(check.IMAGE_EXEMPT)
    added = sorted(actual.keys() - expected.keys() - exempt)
    # An exempt state keeps no reference.
    removed = sorted((expected.keys() - actual.keys()) | (expected.keys() & exempt))
    changed = sorted(name for name in (actual.keys() & expected.keys()) - exempt
                     if not image_diff.compare_files(expected[name], actual[name], tolerance).matches)
    return added, changed, removed


def apply(rendered, references, dry_run=False, out=sys.stdout):
    added, changed, removed = plan(rendered, references)
    actual = check.states(rendered)
    for label, names in (('add', added), ('update', changed), ('delete', removed)):
        for name in names:
            print('  %-6s %s' % (label, name), file=out)
    if not dry_run:
        references.mkdir(parents=True, exist_ok=True)
        for name in added + changed:
            shutil.copyfile(actual[name], references / (name + '.png'))
        for name in removed:
            (references / (name + '.png')).unlink()
    return len(added) + len(changed) + len(removed)


def download(run, directory):
    subprocess.run(['gh', 'run', 'download', str(run), '--name', ARTIFACT, '--dir', str(directory)], check=True)
    return directory


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--run', help='a CI run whose %s artifact holds the renders' % ARTIFACT)
    source.add_argument('--from', dest='source', type=Path, help='a directory holding SCHEME/ renders')
    source.add_argument('--catalog', type=Path, help='render now with this putgd-ui-catalog executable')
    parser.add_argument('--scheme', choices=check.SCHEMES, action='append', help='default: every scheme')
    parser.add_argument('--references', type=Path, default=check.REFERENCES)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    schemes = args.scheme or list(check.SCHEMES)
    with tempfile.TemporaryDirectory(prefix='pgcat-update-') as scratch:
        if args.run:
            root = download(args.run, Path(scratch))
        elif args.source:
            root = args.source
        else:
            root = Path(scratch)
            for scheme in schemes:
                status, seconds = check.render(args.catalog, scheme, root / scheme)
                if status != 0:
                    raise SystemExit('putgd-ui-catalog (%s) failed its own checks (exit %d)' % (scheme, status))
                print('rendered %s in %.0f s' % (scheme, seconds))
        total = 0
        for scheme in schemes:
            rendered = root / scheme
            # A partial render would delete the references of every state it lacks.
            problem = check.render_problem(rendered)
            if problem:
                raise SystemExit('%s is not a complete catalog render: %s' % (rendered, problem))
        for scheme in schemes:
            rendered = root / scheme
            print('%s:' % scheme)
            total += apply(rendered, args.references / scheme, args.dry_run)
    print('%d reference images %s.' % (total, 'would change' if args.dry_run else 'changed'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
