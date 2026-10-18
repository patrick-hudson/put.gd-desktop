"""Repository-scoped GitHub API access shared by release preparation and publication."""
import json
import re
import subprocess
from urllib.parse import quote


def full_sha(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError('Full lowercase source commit SHA required')
    return value


class GitHub:
    def __init__(self, repository):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
            raise ValueError('Exact repository owner/name required')
        self.repository = repository

    def json(self, path, *, paginate=False, method=None, data=None):
        command = ['gh', 'api', 'repos/' + self.repository + '/' + path]
        if paginate:
            command += ['--paginate', '--slurp']
        if method:
            command += ['--method', method]
        if data is not None:
            command += ['--input', '-']
        return json.loads(subprocess.check_output(command, text=True,
                          input=json.dumps(data) if data is not None else None, timeout=60) or 'null')

    def asset_bytes(self, asset_id):
        if type(asset_id) is not int or asset_id <= 0:
            raise ValueError('Invalid release asset ID')
        return subprocess.check_output(['gh', 'api', '-H', 'Accept: application/octet-stream',
                    'repos/' + self.repository + '/releases/assets/' + str(asset_id)], timeout=600)


def remote_tag_commit(api, tag):
    value = api.json('git/ref/tags/' + quote(tag, safe=''))['object']
    for _ in range(8):
        if value.get('type') == 'commit':
            return full_sha(value['sha'])
        if value.get('type') != 'tag':
            break
        value = api.json('git/tags/' + full_sha(value['sha']))['object']
    raise ValueError('Remote tag does not resolve to a bounded commit')


def matching_tags(api, prefix):
    """Return every remote tag name starting with prefix; an empty list is a real answer."""
    pages = api.json('git/matching-refs/tags/' + quote(prefix, safe=''), paginate=True)
    refs = [ref for page in pages for ref in page]
    names = []
    for ref in refs:
        name = ref.get('ref', '')
        if not name.startswith('refs/tags/'):
            raise ValueError('Unexpected matching tag reference')
        names.append(name[len('refs/tags/'):])
    return names


def tag_exists(api, tag):
    return tag in matching_tags(api, tag)
