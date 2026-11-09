#!/usr/bin/env python3
"""Real receivers for the custom HTTP uploader (#1361): Zipline, Chibisafe and XBackBone.

Run this ON a lab builder that has podman. It starts each receiver in its own rootless
pod, bound to one address of that builder (the lab network, or loopback), finishes each
first-run setup, and has a headless browser download the ShareX custom uploader file
(.sxcu) each receiver's own interface offers. put.gd then imports those files unchanged.

  python3 receivers.py up --bind 192.168.68.71     start, set up, download the .sxcu files
  python3 receivers.py sxcu --bind 192.168.68.71   download the .sxcu files again
  python3 receivers.py status --bind 192.168.68.71
  python3 receivers.py down [--purge]              stop; --purge also removes data and state

State (generated passwords and the .sxcu files, which hold upload tokens) lives in
~/putgd-lab/http-receivers, mode 0700, and never leaves the builder except when a test
copies a .sxcu file to the desktop it is testing. Images are pinned by digest. Nothing is
reachable from outside the address given to --bind; ports 18361-18363.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = Path(__file__).resolve().parent
STATE = Path.home() / 'putgd-lab/http-receivers'
IMAGES = {
    'zipline': 'ghcr.io/diced/zipline@sha256:3b66c76cbf0e422f7cd451f124e79f21a8f358f26f2cd93af65fd45f74a35b41',            # 4.8.3
    'postgres': 'docker.io/library/postgres@sha256:aa6cb33cf4c19c765cefcf48d96176258800870a9a1e930ad126200f9d825139',     # 16-alpine
    'chibisafe-web': 'docker.io/chibisafe/chibisafe@sha256:55d01094b9ac75cdf54fdf8cc3ae9ab23208252d8e84bdb69a4e3ee353ef4616',  # 6.7.0
    'chibisafe-server': 'docker.io/chibisafe/chibisafe-server@sha256:3be12397540eddbe2edb8e2b6e2a82b5b3a22186142c4a4f2ec8320e0c7673d5',
    'caddy': 'docker.io/library/caddy@sha256:78bd35025c7ba85961dd149ae670315129954e77abe2383ac47c89106b35ade9',           # 2-alpine
    'xbackbone': 'lscr.io/linuxserver/xbackbone@sha256:06612f8dd8bd3b0cf888eb07716c53c6df1b94fc28a3806fa2018f98ac882289',   # 3.8.2
    'browser': 'mcr.microsoft.com/playwright/python@sha256:209deb0d8f0ba01f66a06d9a96b7ba9fdf14e4f33619abe03f9e6430192e3c2c',  # v1.49.1-noble
}
PORTS = {'zipline': (18361, 3000), 'chibisafe': (18362, 80), 'xbackbone': (18363, 80)}
PODS = {name: 'putgd-rx-' + name for name in PORTS}
VOLUMES = ('putgd-rx-zipline-db', 'putgd-rx-zipline-uploads', 'putgd-rx-chibisafe-database', 'putgd-rx-chibisafe-uploads',
           'putgd-rx-chibisafe-logs', 'putgd-rx-xbackbone-config')
STATE_FILES = ('zipline.secret', 'zipline.dbpw', 'zipline.adminpw', 'chibisafe.adminpw', 'xbackbone.adminpw', 'chibisafe.Caddyfile', 'browser_sxcu.py',
               'zipline-db.env', 'zipline-app.env', 'sxcu/zipline.sxcu', 'sxcu/chibisafe.sxcu', 'sxcu/xbackbone.sxcu')


def podman(*args, check=True, quiet=True):
    result = subprocess.run(['podman', *args], text=True, capture_output=True)
    if check and result.returncode:
        raise SystemExit('podman %s failed: %s' % (' '.join(args[:3]), result.stderr.strip()[:400]))
    return result.stdout.strip() if quiet else result


def exists(kind, name):
    return subprocess.run(['podman', kind, 'exists', name]).returncode == 0


def generated(name, length=18):
    """A generated secret kept in the private state folder; never printed."""
    path = STATE / name
    if not path.exists():
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'w') as file:
            file.write(secrets.token_urlsafe(length))
    return path.read_text().strip()


def environment(name, values):
    """A container's secret settings as a private env file, so none is on a command line."""
    path = STATE / name
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, 'w') as file:
        file.write(''.join('%s=%s\n' % item for item in values.items()))
    return str(path)


def base(bind, name):
    return 'http://%s:%d' % (bind, PORTS[name][0])


def answers(url, seconds=120):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=5) as reply:
                return reply.status
        except urllib.error.HTTPError as error:
            return error.code
        except OSError:
            time.sleep(2)
    return 0


def run(pod, name, image, *options):
    if not exists('container', name):
        podman('run', '-d', '--pod', pod, '--name', name, *options, IMAGES[image])


def up(bind):
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(STATE, 0o700)
    (STATE / 'sxcu').mkdir(mode=0o700, exist_ok=True)
    for volume in VOLUMES:
        if not exists('volume', volume):
            podman('volume', 'create', volume)
    for name, (host, inside) in PORTS.items():
        if not exists('pod', PODS[name]):
            podman('pod', 'create', '--name', PODS[name], '-p', '%s:%d:%d' % (bind, host, inside))
    database = generated('zipline.dbpw', 12)
    run(PODS['zipline'], 'putgd-rx-zipline-db', 'postgres', '--env-file',
        environment('zipline-db.env', {'POSTGRES_USER': 'zipline', 'POSTGRES_PASSWORD': database, 'POSTGRES_DB': 'zipline'}),
        '-v', 'putgd-rx-zipline-db:/var/lib/postgresql/data')
    time.sleep(6)
    run(PODS['zipline'], 'putgd-rx-zipline-app', 'zipline', '--env-file',
        environment('zipline-app.env', {'CORE_SECRET': generated('zipline.secret', 32), 'CORE_HOSTNAME': '0.0.0.0', 'CORE_PORT': '3000',
                                        'DATABASE_URL': 'postgres://zipline:%s@127.0.0.1:5432/zipline' % database}),
        '-v', 'putgd-rx-zipline-uploads:/zipline/uploads')
    shutil.copyfile(HERE / 'chibisafe.Caddyfile', STATE / 'chibisafe.Caddyfile')
    os.chmod(STATE / 'chibisafe.Caddyfile', 0o644)
    run(PODS['chibisafe'], 'putgd-rx-chibisafe-server', 'chibisafe-server', '-v', 'putgd-rx-chibisafe-database:/app/database',
        '-v', 'putgd-rx-chibisafe-uploads:/app/uploads', '-v', 'putgd-rx-chibisafe-logs:/app/logs')
    run(PODS['chibisafe'], 'putgd-rx-chibisafe-web', 'chibisafe-web', '-e', 'BASE_API_URL=http://127.0.0.1:8000')
    run(PODS['chibisafe'], 'putgd-rx-chibisafe-caddy', 'caddy', '-v', '%s:/etc/caddy/Caddyfile:ro,Z' % (STATE / 'chibisafe.Caddyfile'),
        '-v', 'putgd-rx-chibisafe-uploads:/app/uploads:ro')
    run(PODS['xbackbone'], 'putgd-rx-xbackbone-app', 'xbackbone', '-e', 'PUID=1000', '-e', 'PGID=1000', '-e', 'TZ=Etc/UTC',
        '-v', 'putgd-rx-xbackbone-config:/config')
    for name in PORTS:
        podman('pod', 'start', PODS[name])
        print('%-10s %s  HTTP %s' % (name, base(bind, name), answers(base(bind, name) + '/') or 'no answer'), flush=True)
    for name in ('zipline.adminpw', 'chibisafe.adminpw', 'xbackbone.adminpw'):
        generated(name)
    return sxcu(bind)


def sxcu(bind):
    shutil.copyfile(HERE / 'browser_sxcu.py', STATE / 'browser_sxcu.py')
    result = podman('run', '--rm', '--network', 'host', '-v', '%s:/state:Z' % STATE, IMAGES['browser'], 'sh', '-c',
                    'pip install -q playwright==1.49.1 >/dev/null 2>&1; python3 /state/browser_sxcu.py "$0"',
                    json.dumps({name: base(bind, name) for name in PORTS}), check=False, quiet=False)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr.strip()[-600:], file=sys.stderr)
    print('The .sxcu files are in %s (mode 0600): they hold upload tokens, so copy them only to the desktop under test.' % (STATE / 'sxcu'))
    return result.returncode


def status(bind):
    print(podman('ps', '-a', '--filter', 'name=putgd-rx-', '--format', '{{.Names}}\t{{.Status}}'))
    for name in PORTS:
        print('%-10s %s  HTTP %s  sxcu %s' % (name, base(bind, name), answers(base(bind, name) + '/', 6) or 'no answer',
                                             'present' if (STATE / 'sxcu' / (name + '.sxcu')).is_file() else 'missing'))
    return 0


def down(purge):
    for pod in PODS.values():
        if exists('pod', pod):
            podman('pod', 'rm', '-f', pod)
    if purge:
        for volume in VOLUMES:
            if exists('volume', volume):
                podman('volume', 'rm', volume)
        # Exact files only; the folders stay.
        for name in STATE_FILES:
            (STATE / name).unlink(missing_ok=True)
    print('receivers stopped' + (' and their data removed' if purge else ''))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('up', 'sxcu', 'status', 'down'))
    parser.add_argument('--bind', help='the builder address the receivers listen on: its lab-network address, or 127.0.0.1')
    parser.add_argument('--purge', action='store_true', help='with down: also remove volumes, passwords and .sxcu files')
    options = parser.parse_args()
    if options.command != 'down' and not options.bind:
        parser.error('--bind is required')
    if options.command == 'up':
        return up(options.bind)
    if options.command == 'sxcu':
        return sxcu(options.bind)
    if options.command == 'status':
        return status(options.bind)
    return down(options.purge)


if __name__ == '__main__':
    sys.exit(main())
