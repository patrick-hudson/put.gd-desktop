"""#342 acceptance, after run342.sh: retention, then remove every lab object this run put there.

retire: the signer's own retire pass for the lab scope (delete_after 120 s in signer-lab.json),
then a HEAD at the origin for each lab image shows which ones are gone. clean: delete every key the
lab publisher state lists (objects and pending) plus the lab receipts, invalidate /lab/*, and remove
the local lab workspace and state, so the lab scope is empty again.
"""
import json
from pathlib import Path
import shutil
import sys

SRC, CONFIG, ACTION = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
sys.path.insert(0, str(SRC / 'tools/releases'))
import pkgrepo  # noqa: E402

config = pkgrepo.Config.load(CONFIG)
signer = pkgrepo.Signer(config)
s3, cloudfront = signer.clients()
state_path = config.state / 'published' / 'lab-nightly.json'


def heads():
    state = json.loads(state_path.read_text())
    for number in (701, 702, 703):
        key = f'lab/releases/v0.1.1-nightly.{number}/putgd-0.1.1-nightly.{number}-x86_64.AppImage'
        try:
            s3.head_object(Bucket=config.bucket, Key=key)
            present = 'present at the origin'
        except s3.exceptions.ClientError:
            present = 'deleted at the origin'
        print(key, '-', 'pending deletion' if key in state['pending'] else 'tracked' if key in state['objects'] else 'no longer tracked', '-', present)


if ACTION == 'retire':
    signer.retire_only('lab')
    heads()
elif ACTION == 'clean':
    state = json.loads(state_path.read_text())
    keys = sorted(set(state['objects']) | set(state['pending']))
    receipts = config.state / 'receipts' / 'lab'
    keys += ['lab/receipts/' + path.stem + '.json' for path in sorted(receipts.glob('v0.1.1-nightly.70?.json'))]
    for key in keys:
        assert key.startswith('lab/'), key
        s3.delete_object(Bucket=config.bucket, Key=key)
    print('deleted', len(keys), 'lab objects')
    answer = cloudfront.create_invalidation(DistributionId=config.distribution, InvalidationBatch={
        'Paths': {'Quantity': 1, 'Items': ['/lab/*']}, 'CallerReference': 'putgd-342-lab-clean'})
    print('invalidation', answer['Invalidation']['Id'])
    shutil.rmtree(config.state / 'workspaces' / 'lab')
    state_path.unlink()
    print('lab workspace and publisher state removed; receipts kept in', receipts)
