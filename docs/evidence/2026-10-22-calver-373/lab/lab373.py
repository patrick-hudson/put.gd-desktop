"""#373 lab publication on d69 through the signer's lab scope, from the PR branch's pkgrepo.py.

The live signer's config is read by path (its AWS and key files only through the
environment it names); `state` is this run's own directory, so the live signer's
state, checkout and timer are never touched. Every key is under lab/.
  lab373.py SRC CONFIG STATE check
  lab373.py SRC CONFIG STATE dryrun TAG ASSETS     build and sign the next lab tree; list its keys
  lab373.py SRC CONFIG STATE publish TAG ASSETS
  lab373.py SRC CONFIG STATE clean TAG...          delete every tracked lab object and receipt, invalidate /lab/*
"""
import fcntl, json, shutil, sys, urllib.error, urllib.request
from pathlib import Path
SRC, CONFIG, STATE, COMMAND = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
sys.path.insert(0, str(SRC / 'tools/releases'))
import pkgrepo  # noqa: E402
config = pkgrepo.Config.load(CONFIG, state=str(STATE), keys=str(SRC / 'packaging/linux/repo/keys'))
assert config.state == STATE and not str(STATE).startswith(str(Path.home() / 'putgd-signer'))
STATE.mkdir(parents=True, exist_ok=True)


def status(url):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=30) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


with open(STATE / 'lock', 'w') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    signer = pkgrepo.Signer(config)
    if COMMAND == 'check':
        for path in ('lab/update/v1/nightly.json', 'lab/dl/nightly/appimage.zsync', 'update/v1/nightly.json'):
            print(path, status(f'{config.base_url}/{path}'))
    elif COMMAND in ('dryrun', 'publish'):
        tag, assets = sys.argv[5], Path(sys.argv[6])
        inputs = pkgrepo.local_inputs(tag, assets, base=config.base_url + '/' + pkgrepo.SCOPES['lab'])
        published = signer.workspace('lab', 'nightly').published()
        if published and pkgrepo.version_key(tag[1:]) <= pkgrepo.version_key(published['version']):
            raise SystemExit('The lab repository already carries ' + published['version'])
        if COMMAND == 'publish':
            signer.publish_nightly('lab', inputs)
            print(json.dumps(signer.workspace('lab', 'nightly').published()))
        else:
            receipt = pkgrepo.new_receipt(inputs, 'lab')
            workspace = signer.build_next('lab', 'nightly', inputs, receipt)
            signer.sign_nightly(workspace, inputs, receipt)
            keys = sorted('lab/' + str(path.relative_to(workspace.tree)) for path in workspace.tree.rglob('*') if path.is_file())
            print('\n'.join(keys))
            print(len(keys), 'keys, all under lab/:', all(key.startswith('lab/') for key in keys))
    elif COMMAND == 'clean':
        s3, cloudfront = signer.clients()
        state_path = config.state / 'published' / 'lab-nightly.json'
        state = json.loads(state_path.read_text()) if state_path.exists() else {'objects': {}, 'pending': {}}
        keys = sorted(set(state['objects']) | set(state['pending'])) + [f'lab/receipts/{t}.json' for t in sys.argv[5:]]
        for key in keys:
            assert key.startswith('lab/'), key
            s3.delete_object(Bucket=config.bucket, Key=key)
            print('deleted', key)
        print('deleted', len(keys), 'lab objects')
        answer = cloudfront.create_invalidation(DistributionId=config.distribution, InvalidationBatch={
            'Paths': {'Quantity': 1, 'Items': ['/lab/*']}, 'CallerReference': 'putgd-373-lab-clean-' + str(len(keys))})
        print('invalidation', answer['Invalidation']['Id'])
    else:
        raise SystemExit(__doc__)
