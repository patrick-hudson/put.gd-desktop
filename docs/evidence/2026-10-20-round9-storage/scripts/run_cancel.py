#!/usr/bin/env python3
"""#203 cancel-latency measurement on a builder: moto on loopback behind a
throttled uplink proxy, the nightly ("before") and #203 ("after") builds of
tests/performance/s3_upload_probe in cancel mode. Synthetic credentials and a
synthetic file only. Writes one JSON line per run to OUT.

  run_cancel.py WORK_DIR OUT BEFORE_BUILD AFTER_BUILD
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import time

work, out, before, after = sys.argv[1:5]
os.makedirs(work, exist_ok=True)
mib = 1 << 20
RATES = [int(r) for r in os.environ.get('RATES', '2,8').split(',')]       # MiB/s uplink
LANES = [int(l) for l in os.environ.get('LANES', '0,3').split(',')]       # extra lanes lent
REPS = int(os.environ.get('REPS', '3'))
SIZE = int(os.environ.get('SIZE_MIB', '160')) * mib


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


env = dict(os.environ)
for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS'):
    env.pop(key, None)
for key in ('XDG_STATE_HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR'):
    env[key] = os.path.join(work, key.lower())
    os.makedirs(env[key], mode=0o700, exist_ok=True)
env['QT_QPA_PLATFORM'] = 'offscreen'
env['AWS_ACCESS_KEY_ID'] = 'PUTGD_SYNTHETIC_ACCESS'
env['AWS_SECRET_ACCESS_KEY'] = 'putgd-synthetic-secret-not-a-real-key'
env['AWS_DEFAULT_REGION'] = 'us-east-1'

started = []
try:
    moto_port = free_port()
    moto = subprocess.Popen(['moto_server', '-H', '127.0.0.1', '-p', str(moto_port)], stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(work, 'moto.log'), 'w'), env=env)
    started.append(moto)
    for _ in range(100):
        try:
            socket.create_connection(('127.0.0.1', moto_port), 0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    import boto3  # moto's own dependency, against loopback only
    boto3.client('s3', endpoint_url=f'http://127.0.0.1:{moto_port}', region_name='us-east-1', aws_access_key_id=env['AWS_ACCESS_KEY_ID'], aws_secret_access_key=env['AWS_SECRET_ACCESS_KEY']).create_bucket(Bucket='putgd-cancel')
    payload = os.path.join(work, 'cancel.bin')
    with open(payload, 'wb') as f:
        seed = os.urandom(mib)
        for i in range(SIZE // mib):
            f.write(seed[i % 997:] + seed[:i % 997])
    with open(out, 'a') as results:
        for rate in RATES:
            proxy_port = free_port()
            proxy = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'throttle_proxy.py'),
                                      str(proxy_port), str(moto_port), str(rate * mib)], stdout=subprocess.PIPE, env=env)
            started.append(proxy)
            proxy.stdout.readline()
            for lanes in LANES:
                for rep in range(REPS):
                    for variant, build in (('before', before), ('after', after)):
                        probe = os.path.join(build, 's3_upload_probe')
                        begin = time.monotonic()
                        run = subprocess.run([probe, f'http://127.0.0.1:{proxy_port}', 'putgd-cancel', payload, 'cancel', '0', str(lanes)],
                                             capture_output=True, text=True, env=env, timeout=900, cwd=work)
                        line = {}
                        for text in run.stdout.splitlines():
                            try:
                                line = json.loads(text)
                            except ValueError:
                                pass
                        line.update({'variant': variant, 'rateMiBps': rate, 'lanes': lanes + 1, 'rep': rep,
                                     'exitCode': run.returncode, 'wallS': round(time.monotonic() - begin, 2)})
                        results.write(json.dumps(line) + '\n')
                        results.flush()
                        print(json.dumps({k: line.get(k) for k in ('variant', 'rateMiBps', 'lanes', 'rep', 'status', 'errorCode', 'cancelLatencyMs', 'incompleteUpload')}), flush=True)
            proxy.terminate()
            proxy.wait(10)
finally:
    for process in reversed(started):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
    if os.path.exists(os.path.join(work, 'cancel.bin')):
        os.remove(os.path.join(work, 'cancel.bin'))
