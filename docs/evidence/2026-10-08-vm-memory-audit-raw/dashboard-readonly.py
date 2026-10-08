#!/usr/bin/env python3
"""Read the existing local memory dashboard; never record or alter app/VM state."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from urllib.request import urlopen


def read(endpoint):
    with urlopen('http://127.0.0.1:8767' + endpoint, timeout=5) as response:
        data = response.read(4 * 1024 * 1024)
        metadata = {
            'endpoint': endpoint,
            'status': response.status,
            'contentType': response.headers.get('Content-Type'),
            'cacheControl': response.headers.get('Cache-Control'),
            'bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest(),
        }
        return data, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sources', nargs='+', default=[
        'lab/gnome-001', 'lab/kde-001', 'lab/fresh-cinnamon-u9-20261008'])
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Use a new output path; existing evidence is preserved.')
    observed = datetime.now(timezone.utc).isoformat()
    raw, api_response = read('/api')
    data = json.loads(raw)
    html, html_response = read('/')
    interval = data['interval']
    fields = ['id', 'source', 'pid', 'name', 'identity', 'first', 'last',
              'ended', 'metric', 'current', 'maximum', 'average', 'count']
    sessions = []
    for item in data['sessions']:
        if item['source'] not in args.sources:
            continue
        session = {key: item.get(key) for key in fields}
        session.update({
            'displayedCurrentMiB': f"{item['current'] / 1048576:.2f}",
            'displayedSampledPeakMiB': f"{item['maximum'] / 1048576:.2f}",
            'displayedSampledAverageMiB': f"{item['average'] / 1048576:.2f}",
            'ageSeconds': data['now'] - item['last'],
            'liveByUi': item['ended'] is None and
                        data['now'] - item['last'] < max(5, interval * 3),
            'sessionIdentityMatches': hashlib.sha256(
                (item['source'] + ':' + item['identity']).encode()
            ).hexdigest()[:24] == item['id'],
        })
        sessions.append(session)
    result = {
        'timestampUtc': observed,
        'scope': 'Read-only GET /api and /; no record action or guest sampling.',
        'serverNow': data['now'],
        'intervalSeconds': interval,
        'sources': {name: {key: value.get(key) for key in ['state', 'last', 'count']}
                    for name, value in data['sources'].items()
                    if name in args.sources},
        'sessions': sessions,
        'responses': [api_response, html_response],
        'htmlMatchesWorkingSource': html == Path(
            'work/memory-monitor/index.html').read_bytes(),
        'units': 'API bytes; UI MiB = bytes / 1048576, two decimal places.',
        'limits': 'Card maxima/averages are whole-session sampled RSS. '
                  'Freshness uses server receipt timestamps. No acceptance claim.',
    }
    with args.output.open('x') as stream:
        stream.write(json.dumps(result, indent=2) + '\n')
    print(args.output)


if __name__ == '__main__':
    main()
