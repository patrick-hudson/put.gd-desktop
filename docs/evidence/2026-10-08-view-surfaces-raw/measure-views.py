"""Frozen adapter for the existing physical empty-view workload; no product edits."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path('/home/patrick/work/put.gd')
parser = argparse.ArgumentParser()
parser.add_argument('--app', required=True)
parser.add_argument('--cli', required=True)
parser.add_argument('--output', required=True)
args = parser.parse_args()
output = Path(args.output).resolve()
assert not output.exists()
command = [sys.executable, str(ROOT / 'tests/performance/bench_views.py'),
           '--app', str(Path(args.app).resolve()), '--cli', str(Path(args.cli).resolve()),
           '--fixture', '/tmp/putgd-startup-reclaim-exp001', '--output', str(output),
           '--cycles', '3', '--idle-seconds', '30']
run = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
if output.exists():
    (output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    (output / 'stdout.jsonl').write_text(run.stdout)
    (output / 'stderr.log').write_text(run.stderr)
if run.returncode:
    raise SystemExit(run.returncode)
result = json.loads((output / 'results.json').read_text())
metrics = {key: result[key] for key in ('passed', 'allThreeViewsReopened', 'operationCount',
                                      'finalHiddenRssBytes', 'peakSampledRssBytes')}
metrics['coldRssBytes'] = result['samples'][0]['rssBytes']
metrics['hiddenXorgShmRssBytes'] = sum(row['rssBytes'] for row in result['samples'][-1]['footprint']
                                       if 'xorg' in row['mapping'].lower())
(output / 'metrics.json').write_text(json.dumps(metrics, indent=2) + '\n')
print(json.dumps(metrics))
