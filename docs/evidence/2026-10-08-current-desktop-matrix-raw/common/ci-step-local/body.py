import json, pathlib, subprocess
pathlib.Path('/home/patrick/work/put.gd/work/cross-desktop-current/ci-step-check-final').mkdir(parents=True, exist_ok=True)
command = ['python3', 'tests/desktop/test_lab.py']
result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
pathlib.Path('/home/patrick/work/put.gd/work/cross-desktop-current/ci-step-check-final/lab-contracts.log').write_text(result.stdout)
pathlib.Path('/home/patrick/work/put.gd/work/cross-desktop-current/ci-step-check-final/lab-contracts.json').write_text(json.dumps({
    'command': command, 'exitCode': result.returncode,
    'scope': 'Host protocol contracts only; no graphical desktop acceptance'
}, indent=2) + '\n')
print(result.stdout, end='')
raise SystemExit(result.returncode)