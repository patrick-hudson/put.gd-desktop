"""#293 phase 3 lab clean-up on d69: delete every object the signer's lab scope tracks (objects and pending) and the
lab receipts this acceptance published, invalidate /lab/*, and remove the local lab workspace and publisher state."""
import json, shutil, sys
from pathlib import Path
SRC, CONFIG = Path(sys.argv[1]), Path(sys.argv[2])
TAGS = sys.argv[3:]
sys.path.insert(0, str(SRC / 'tools/releases'))
import pkgrepo  # noqa: E402
config = pkgrepo.Config.load(CONFIG)
s3, cloudfront = pkgrepo.Signer(config).clients()
state_path = config.state / 'published' / 'lab-nightly.json'
state = json.loads(state_path.read_text())
keys = sorted(set(state['objects']) | set(state['pending'])) + [f'lab/receipts/{t}.json' for t in TAGS]
for key in keys:
    assert key.startswith('lab/'), key
    s3.delete_object(Bucket=config.bucket, Key=key)
print('deleted', len(keys), 'lab objects')
answer = cloudfront.create_invalidation(DistributionId=config.distribution, InvalidationBatch={
    'Paths': {'Quantity': 1, 'Items': ['/lab/*']}, 'CallerReference': 'putgd-293p3-lab-clean'})
print('invalidation', answer['Invalidation']['Id'])
shutil.rmtree(config.state / 'workspaces' / 'lab')
state_path.unlink()
print('lab workspace and publisher state removed')
