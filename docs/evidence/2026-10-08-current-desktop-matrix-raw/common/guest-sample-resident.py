import argparse, datetime, hashlib, json, os, pathlib, time
parser = argparse.ArgumentParser()
parser.add_argument('--pid', type=int, required=True)
parser.add_argument('--start-ticks', type=int, required=True)
parser.add_argument('--sha256', required=True)
parser.add_argument('--seconds', type=float, default=300)
parser.add_argument('--interval', type=float, default=5)
args = parser.parse_args()
assert 1 <= args.seconds <= 600 and .5 <= args.interval <= 30
proc = pathlib.Path('/proc') / str(args.pid)
assert proc.stat().st_uid == os.getuid()
assert (proc / 'comm').read_text().strip() == 'putgd'
def digest():
    value = hashlib.sha256()
    with (proc / 'exe').open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            value.update(block)
    return value.hexdigest()
assert digest() == args.sha256
clock_ticks = os.sysconf('SC_CLK_TCK')
def sample():
    fields = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
    assert int(fields[19]) == args.start_ticks
    counters = {}
    for line in (proc / 'smaps_rollup').read_text().splitlines():
        key, _, value = line.partition(':')
        if key in ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty', 'Shared_Clean', 'Shared_Dirty', 'Swap'):
            counters[key + 'Bytes'] = int(value.split()[0]) * 1024
    return {'monotonicSeconds': time.monotonic(), 'cpuTicks': int(fields[11]) + int(fields[12]), **counters}
time.sleep(5)
start = sample()
result = {'schemaVersion': 1, 'scope': 'Current installed resident after functional/visible-view wave; VM observation, not cold or physical performance acceptance',
          'timestampUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'pid': args.pid, 'startTicks': args.start_ticks, 'appSha256': args.sha256,
          'kernel': os.uname().release, 'clockTicksPerSecond': clock_ticks,
          'parameters': {'durationSeconds': args.seconds, 'intervalSeconds': args.interval, 'warmupSeconds': 5},
          'samples': [start]}
while time.monotonic() - start['monotonicSeconds'] < args.seconds:
    time.sleep(min(args.interval, max(0, args.seconds - (time.monotonic() - start['monotonicSeconds']))))
    result['samples'].append(sample())
assert digest() == args.sha256
last = result['samples'][-1]
elapsed = last['monotonicSeconds'] - start['monotonicSeconds']
result.update(elapsedSeconds=elapsed, peakSampledRssBytes=max(x['RssBytes'] for x in result['samples']),
              finalRssBytes=last['RssBytes'], finalPssBytes=last['PssBytes'],
              cpuPercentOneCore=100 * (last['cpuTicks'] - start['cpuTicks']) / clock_ticks / elapsed,
              processIdentityUnchanged=True)
print(json.dumps(result))
