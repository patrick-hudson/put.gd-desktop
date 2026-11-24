#!/usr/bin/env python3
"""Real SFTP, Nextcloud and WebDAV servers for the storage destination tests (#1871, #1872).

Run this ON a lab builder that has podman. It starts the servers in two rootless pods, bound
to one address of that builder (the lab network, or loopback), generates every credential and
key, and finishes Nextcloud's first-run setup, so a desktop under test can be pointed at them.

  python3 servers.py up --bind 192.168.68.71     start and set up
  python3 servers.py status --bind 192.168.68.71
  python3 servers.py env --bind 192.168.68.71    write the env file the test harness sources
  python3 servers.py ca --bind 192.168.68.71     export the CA that signs the trusted HTTPS listeners
  python3 servers.py known-hosts --bind ADDR     pin the SFTP servers' current host keys
  python3 servers.py rotate-hostkey              change the SFTP servers' host keys
  python3 servers.py quota 5MB|none              the Nextcloud user's quota
  python3 servers.py sharing on|off              Nextcloud sharing on or off
  python3 servers.py share-password on|off       passwords enforced on Nextcloud public links
  python3 servers.py check --bind ADDR           run the server checks (check.py)
  python3 servers.py down [--purge]              stop; --purge also removes data and state

State (generated passwords, keys, host keys) lives in ~/putgd-lab/storage-servers, mode 0700.
Images are pinned by digest. Nothing is reachable from outside the address given to --bind;
ports 18371-18379 (18371-18377 in use).
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = Path(__file__).resolve().parent
STATE = Path.home() / 'putgd-lab/storage-servers'
CLIENT = STATE / 'client'      # what the test harness reads: private keys, passwords, known_hosts
SERVER = STATE / 'server'      # what the SSH containers read (a directory mount): host keys, configuration
WEB = STATE / 'web'            # what the web containers read, each file mounted on its own
CA_DIR = STATE / 'ca'
ENV_FILE = STATE / 'storage-servers.env'
IMAGES = {
    'alpine': 'docker.io/library/alpine@sha256:f59abe656b692d901e184be3445e084786602108599d2a51d229c0f5a48c71bf',            # 3.24, base of the sshd image
    'nextcloud': 'docker.io/library/nextcloud@sha256:f22d1dd63844bd15bbf6c2019149653c3d1c775ba48b173b56c3c6ad5f386196',      # 35.0.3-apache
    'httpd': 'docker.io/library/httpd@sha256:32e9403360ab2f1047523e72f256527fb9aee68b342abf6ad03f0d629157f3d9',              # 2.4.71 (Debian: its apr-util has the DBM driver mod_dav's lock database needs)
    'caddy': 'docker.io/library/caddy@sha256:78bd35025c7ba85961dd149ae670315129954e77abe2383ac47c89106b35ade9',              # 2-alpine
}
SSHD_IMAGE = 'localhost/putgd-st-sshd:2'
# Port -> what it is. 18378 and 18379 are free for the next server.
PORTS = {'sftp': 18371, 'sftp_nosftp': 18372, 'nextcloud_selfsigned': 18373, 'nextcloud': 18374,
         'webdav': 18375, 'public_https': 18376, 'public_http': 18377}
PODS = {'sftp': 'putgd-st-sftp', 'web': 'putgd-st-web'}
CONTAINERS = ('putgd-st-sshd', 'putgd-st-sshd-nosftp', 'putgd-st-nextcloud', 'putgd-st-webdav',
              'putgd-st-caddy-trusted', 'putgd-st-caddy-selfsigned')
VOLUMES = ('putgd-st-sftp-upload', 'putgd-st-nextcloud-data', 'putgd-st-webdav-data',
           'putgd-st-caddy-trusted-data', 'putgd-st-caddy-selfsigned-data')
NEXTCLOUD = 'putgd-st-nextcloud'
USERS = {'sftp_key': 'putgd-key', 'sftp_pw': 'putgd-pw', 'webdav': 'putgd-dav', 'nextcloud': 'putgd', 'nextcloud_admin': 'putgd-admin'}
# Exact files of the state folders; "down --purge" removes these and then the empty folders.
STATE_FILES = ('client/putgd-key.ed25519', 'client/putgd-key.ed25519.pub', 'client/putgd-key-pass.ed25519',
               'client/putgd-key-pass.ed25519.pub', 'client/putgd-key-pass.passphrase', 'client/putgd-pw.password',
               'client/webdav.password', 'client/nextcloud-putgd.password', 'client/nextcloud-putgd.env',
               'client/nextcloud-admin.password', 'client/known_hosts', 'client/.askpass.sh',
               'server/hostkeys/ssh_host_ed25519_key', 'server/hostkeys/ssh_host_ed25519_key.pub',
               'server/hostkeys/ssh_host_rsa_key', 'server/hostkeys/ssh_host_rsa_key.pub',
               'server/authorized_keys.putgd-key', 'server/authorized_keys.putgd-pw',
               'server/sshd_config.sftp', 'server/sshd_config.nosftp', 'web/webdav.httpd.conf',
               'web/webdav.htpasswd', 'web/Caddyfile.trusted', 'web/Caddyfile.selfsigned',
               'web/nextcloud.env', 'ca/putgd-lab-root.crt', 'storage-servers.env')
STATE_DIRS = ('client', 'server/hostkeys', 'server', 'web', 'ca')


def podman(*args, check=True, quiet=True, stdin=None):
    result = subprocess.run(['podman', *args], text=True, capture_output=True, stdin=stdin)
    if check and result.returncode:
        raise SystemExit('podman %s failed: %s' % (' '.join(args[:3]), result.stderr.strip()[:400]))
    return result.stdout.strip() if quiet else result


def exists(kind, name):
    return subprocess.run(['podman', kind, 'exists', name]).returncode == 0


def write(path, text, mode=0o600):
    """A file written whole, with its mode set from the first byte. A file that already holds this
    text is left alone: a running container's single-file mount follows the inode, and a rewrite
    would leave it looking at the old one."""
    path = Path(path)
    if path.is_file() and path.read_text() == text and (path.stat().st_mode & 0o777) == mode:
        return path
    temporary = path.with_name(path.name + '.new')
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    with os.fdopen(descriptor, 'w') as file:
        file.write(text)
    os.chmod(temporary, mode)
    os.replace(temporary, path)
    return path


def generated(path, length=18):
    """A generated secret kept in the private state folder; never printed."""
    path = Path(path)
    if not path.exists():
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'w') as file:
            file.write(secrets.token_urlsafe(length))
    return path.read_text().strip()


def lab_address(text):
    """--bind as given when it is loopback or one of the lab builders' addresses (192.168.68.69 to
    .74); exits otherwise."""
    try:
        address = ipaddress.ip_address(text)
    except ValueError:
        raise SystemExit('--bind takes one IPv4 address of this builder: 127.0.0.1 or a lab builder address, 192.168.68.69 to 192.168.68.74')
    lab = ipaddress.IPv4Address('192.168.68.69') <= address <= ipaddress.IPv4Address('192.168.68.74') if address.version == 4 else False
    if str(address) != text or not (str(address) == '127.0.0.1' or lab):
        raise SystemExit('--bind %s is not 127.0.0.1 or a lab builder address (192.168.68.69 to 192.168.68.74); the servers would not stay on the lab network' % text)
    return text


def published(pod):
    """Every port binding of an existing pod's infrastructure container, from its inspection:
    a sorted list of (protocol, container port, host address, host port). Exits when the pod has
    none, so a stopped or unreadable pod is never reused on a guess."""
    infra = podman('pod', 'inspect', pod, '--format', '{{.InfraContainerID}}', check=False)
    raw = podman('inspect', infra, '--format', '{{json .HostConfig.PortBindings}}', check=False) if infra else ''
    try:
        bindings = json.loads(raw) or {}
        found = sorted((key.partition('/')[2], int(key.partition('/')[0]), item['HostIp'], int(item['HostPort']))
                       for key, items in bindings.items() for item in items)
    except (ValueError, KeyError, TypeError):
        found = []
    if not found:
        raise SystemExit('cannot read the port bindings of pod %s (it has none, or its inspection failed): run "down" first' % pod)
    return found


def answers(url, seconds=120):
    """The HTTP status once the server answers for itself (certificates are not checked here);
    its last 5xx, or 0, at the deadline."""
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    deadline = time.monotonic() + seconds
    last = 0
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=5, context=context) as reply:
                return reply.status
        except urllib.error.HTTPError as error:
            if error.code < 500:
                return error.code
            last = error.code
        except OSError:
            pass
        time.sleep(2)
    return last


def ssh_banner(bind, port, seconds=60):
    """True once something on the port speaks SSH."""
    import socket
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((bind, port), timeout=3) as connection:
                if connection.recv(4) == b'SSH-':
                    return True
        except OSError:
            pass
        time.sleep(1)
    return False


def container_state(name):
    if not exists('container', name):
        return 'absent'
    return podman('inspect', name, '--format', '{{.State.Status}}', check=False) or 'unknown'


def run(pod, name, image, *options):
    """Starts a container in a pod. An existing one whose bind-mount sources differ from the
    wanted ones (a file moved since it was made) is recreated; its volumes stay."""
    wanted = {options[i + 1].split(':')[0] for i, option in enumerate(options[:-1]) if option == '-v' and options[i + 1].startswith('/')}
    if exists('container', name):
        have = set(podman('inspect', name, '--format', '{{range .Mounts}}{{.Source}}\n{{end}}', check=False).split())
        if not wanted <= have:
            podman('rm', '-f', name)
    if not exists('container', name):
        podman('run', '-d', '--pod', pod, '--name', name, *options, image)
    elif container_state(name) != 'running':
        podman('start', name)


def fingerprint(public_key):
    out = subprocess.run(['ssh-keygen', '-lf', str(public_key)], text=True, capture_output=True).stdout.split()
    return out[1] if len(out) > 1 else ''


# --- state, keys and configuration ---

def make_state():
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(STATE, 0o700)
    for name in ('client', 'server', 'server/hostkeys', 'web', 'ca'):
        (STATE / name).mkdir(mode=0o700, exist_ok=True)
    # The containers' view of the state is read only; its folders must be traversable by them.
    for name in ('server', 'server/hostkeys'):
        os.chmod(STATE / name, 0o755)


def keygen(path, comment, passphrase_file=None):
    """An ed25519 key pair, made in a private staging folder and moved into place only when it is
    complete. With a passphrase file the private key is encrypted first; the passphrase reaches
    ssh-keygen through SSH_ASKPASS, never a command line."""
    if path.exists():
        return
    staging = CLIENT / ('.staging-' + secrets.token_hex(4))
    staging.mkdir(mode=0o700)
    key = staging / 'key'
    askpass = staging / 'askpass.sh'
    try:
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', comment, '-f', str(key)], check=True)
        if passphrase_file:
            write(askpass, '#!/bin/sh\ncat %s\n' % passphrase_file, 0o700)
            subprocess.run(['ssh-keygen', '-q', '-p', '-f', str(key)], check=True, stdin=subprocess.DEVNULL,
                           env=dict(os.environ, SSH_ASKPASS=str(askpass), SSH_ASKPASS_REQUIRE='force'))
            # Encryption must have worked: loading the key with an empty passphrase has to fail.
            if subprocess.run(['ssh-keygen', '-y', '-P', '', '-f', str(key)], capture_output=True).returncode == 0:
                raise SystemExit('the passphrase key was not encrypted; nothing was kept')
        os.chmod(key, 0o600)
        os.replace(str(key) + '.pub', str(path) + '.pub')
        os.replace(key, path)
    finally:
        # The staging folder's own files, then the empty folder.
        for leftover in staging.iterdir():
            leftover.unlink()
        staging.rmdir()


def host_keys(replace=False):
    """The SFTP servers' host keys, an ed25519 and an RSA one."""
    folder = SERVER / 'hostkeys'
    for kind, extra in (('ed25519', []), ('rsa', ['-b', '3072'])):
        path = folder / ('ssh_host_%s_key' % kind)
        if path.exists() and not replace:
            continue
        fresh = folder / ('new_%s_key' % kind)
        for leftover in (fresh, fresh.with_name(fresh.name + '.pub')):
            leftover.unlink(missing_ok=True)
        subprocess.run(['ssh-keygen', '-q', '-t', kind, *extra, '-N', '', '-C', 'putgd-lab-host', '-f', str(fresh)], check=True)
        os.chmod(fresh, 0o600)
        os.replace(fresh, path)
        os.replace(fresh.with_name(fresh.name + '.pub'), path.with_name(path.name + '.pub'))


def prepare(bind):
    make_state()
    keygen(CLIENT / 'putgd-key.ed25519', 'putgd-key')
    generated(CLIENT / 'putgd-key-pass.passphrase', 18)
    keygen(CLIENT / 'putgd-key-pass.ed25519', 'putgd-key-pass', CLIENT / 'putgd-key-pass.passphrase')
    public = [CLIENT / (name + '.ed25519.pub') for name in ('putgd-key', 'putgd-key-pass')]
    write(SERVER / 'authorized_keys.putgd-key', ''.join(path.read_text() for path in public), 0o644)
    write(SERVER / 'authorized_keys.putgd-pw', '', 0o644)
    # The password user's password is mounted from the client folder, mode 0600, into the two SSH
    # containers only; an older copy beside the host keys is removed.
    generated(CLIENT / 'putgd-pw.password')
    os.chmod(CLIENT / 'putgd-pw.password', 0o600)
    (SERVER / 'putgd-pw.password').unlink(missing_ok=True)
    host_keys()
    # Files the web containers mount used to live beside the SSH ones; a rename keeps the inode a
    # running container has mounted.
    for name in ('webdav.httpd.conf', 'webdav.htpasswd', 'Caddyfile.trusted', 'Caddyfile.selfsigned', 'nextcloud.env'):
        if (SERVER / name).is_file() and not (WEB / name).exists():
            os.rename(SERVER / name, WEB / name)
    for name in ('sshd_config.sftp', 'sshd_config.nosftp'):
        write(SERVER / name, (HERE / name).read_text(), 0o644)
    write(WEB / 'webdav.httpd.conf', (HERE / 'webdav.httpd.conf').read_text(), 0o644)
    for name in ('Caddyfile.trusted', 'Caddyfile.selfsigned'):
        write(WEB / name, (HERE / name).read_text().replace('@BIND@', bind), 0o644)
    # WebDAV: an htpasswd entry made by openssl, the password on its standard input. It is made
    # once: each run would otherwise salt a new hash and replace a file the container has mounted.
    password = generated(CLIENT / 'webdav.password')
    if not (WEB / 'webdav.htpasswd').is_file():
        entry = subprocess.run(['openssl', 'passwd', '-apr1', '-stdin'], input=password, text=True, capture_output=True, check=True).stdout.strip()
        write(WEB / 'webdav.htpasswd', '%s:%s\n' % (USERS['webdav'], entry), 0o644)
    admin = generated(CLIENT / 'nextcloud-admin.password')
    user = generated(CLIENT / 'nextcloud-putgd.password')
    domains = ' '.join(['%s:%d' % (bind, PORTS['nextcloud']), '%s:%d' % (bind, PORTS['nextcloud_selfsigned']), bind, 'localhost'])
    write(WEB / 'nextcloud.env', ''.join('%s=%s\n' % item for item in {
        'NEXTCLOUD_ADMIN_USER': USERS['nextcloud_admin'], 'NEXTCLOUD_ADMIN_PASSWORD': admin, 'SQLITE_DATABASE': 'nextcloud',
        'NEXTCLOUD_TRUSTED_DOMAINS': domains, 'OVERWRITEPROTOCOL': 'https', 'TRUSTED_PROXIES': '127.0.0.1'}.items()))
    write(CLIENT / 'nextcloud-putgd.env', 'OC_PASS=%s\n' % user)


# --- Nextcloud ---

def occ(*args, env_file=None, check=True):
    options = ['-u', 'www-data'] + (['--env-file', str(env_file)] if env_file else [])
    return podman('exec', *options, NEXTCLOUD, 'php', 'occ', *args, check=check, quiet=False)


def wait_installed(seconds=420):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = podman('exec', '-u', 'www-data', NEXTCLOUD, 'php', 'occ', 'status', '--output=json', check=False, quiet=False)
        if result.returncode == 0 and '"installed":true' in result.stdout.replace(' ', ''):
            return True
        time.sleep(5)
    return False


def nextcloud_setup(bind):
    if not wait_installed():
        raise SystemExit('Nextcloud did not finish its first-run install: see "podman logs %s"' % NEXTCLOUD)
    system = lambda key, value, kind='string', *index: occ('config:system:set', key, *index, '--value=%s' % value, '--type=%s' % kind)
    for index, domain in enumerate(['%s:%d' % (bind, PORTS['nextcloud']), '%s:%d' % (bind, PORTS['nextcloud_selfsigned']), bind, 'localhost']):
        system('trusted_domains', domain, 'string', str(index))
    system('trusted_proxies', '127.0.0.1', 'string', '0')
    system('overwriteprotocol', 'https')
    system('overwrite.cli.url', 'https://%s:%d' % (bind, PORTS['nextcloud']))
    system('auth.bruteforce.protection.enabled', 'false', 'boolean')
    system('allow_local_remote_servers', 'true', 'boolean')
    # The image's APCu cache keeps serving the old sharing settings to the web server after an
    # occ change; the lab wants "sharing off" to bite at once, so its config file says no cache.
    subprocess.run(['podman', 'exec', '-i', NEXTCLOUD, 'sh', '-c', 'cat > /var/www/html/config/apcu.config.php'], check=True, text=True,
                   input="<?php\n$CONFIG = array (\n  'memcache.local' => '\\OC\\Memcache\\ArrayCache',\n);\n")
    for app in ('firstrunwizard', 'recommendations', 'dashboard'):
        occ('app:disable', app, check=False)
    # Sharing defaults: on, links allowed, no password required; later "sharing" and
    # "share-password" runs are not overwritten by another "up".
    for key, value in (('shareapi_enabled', 'yes'), ('shareapi_allow_links', 'yes'), ('shareapi_enforce_links_password', 'no')):
        if not occ('config:app:get', 'core', key, check=False).stdout.strip():
            occ('config:app:set', 'core', key, '--value=' + value)
    name = USERS['nextcloud']
    env_file = CLIENT / 'nextcloud-putgd.env'
    if occ('user:info', name, check=False).returncode:
        occ('user:add', '--password-from-env', '--display-name=put.gd lab', name, env_file=env_file)
    else:
        occ('user:resetpassword', '--password-from-env', name, env_file=env_file)


def nextcloud_config(key, value):
    if container_state(NEXTCLOUD) != 'running':
        raise SystemExit('Nextcloud is not running: run "up" first')
    occ('config:app:set', 'core', key, '--value=' + value)


# --- commands ---

def up(bind):
    published_ports = {'sftp': ((PORTS['sftp'], 22), (PORTS['sftp_nosftp'], 2222)),
                       'web': tuple((PORTS[name], PORTS[name]) for name in ('nextcloud_selfsigned', 'nextcloud', 'webdav', 'public_https', 'public_http'))}
    # An existing pod must publish exactly these bindings on --bind, and no others; checked before
    # anything is written.
    for key, pod in PODS.items():
        if exists('pod', pod):
            expected = sorted(('tcp', inside, bind, host) for host, inside in published_ports[key])
            if published(pod) != expected:
                raise SystemExit('the %s pod does not publish exactly %s: run "down" first' % (pod, ', '.join('%s:%d' % (bind, port) for _, _, _, port in expected)))
    prepare(bind)
    for volume in VOLUMES:
        if not exists('volume', volume):
            podman('volume', 'create', volume)
    for key, pod in PODS.items():
        if not exists('pod', pod):
            podman('pod', 'create', '--name', pod, *sum((['-p', '%s:%d:%d' % (bind, host, inside)] for host, inside in published_ports[key]), []))
    if not exists('image', SSHD_IMAGE):
        podman('build', '-t', SSHD_IMAGE, '-f', str(HERE / 'sftp.Containerfile'), str(HERE))
    server = '%s:/server:ro,z' % SERVER
    password = '%s:/run/putgd-pw.password:ro,z' % (CLIENT / 'putgd-pw.password')
    run(PODS['sftp'], 'putgd-st-sshd', SSHD_IMAGE, '-v', server, '-v', password, '-v', 'putgd-st-sftp-upload:/srv/sftp/upload',
        '--mount', 'type=tmpfs,destination=/srv/sftp/full,tmpfs-size=1m,tmpfs-mode=1777', '--entrypoint',
        '["/usr/local/bin/entrypoint","sftp"]')
    run(PODS['sftp'], 'putgd-st-sshd-nosftp', SSHD_IMAGE, '-v', server, '-v', password, '--entrypoint', '["/usr/local/bin/entrypoint","nosftp"]')
    run(PODS['web'], NEXTCLOUD, IMAGES['nextcloud'], '--env-file', str(WEB / 'nextcloud.env'), '-v', 'putgd-st-nextcloud-data:/var/www/html')
    run(PODS['web'], 'putgd-st-webdav', IMAGES['httpd'], '--user', '1:1', '-v', 'putgd-st-webdav-data:/dav',
        '-v', '%s:/usr/local/apache2/conf/httpd.conf:ro,z' % (WEB / 'webdav.httpd.conf'),
        '-v', '%s:/etc/webdav.htpasswd:ro,z' % (WEB / 'webdav.htpasswd'))
    # The trusted Caddy first, so its root CA exists before anything else is asked of it.
    run(PODS['web'], 'putgd-st-caddy-trusted', IMAGES['caddy'], '-v', '%s:/etc/caddy/Caddyfile:ro,z' % (WEB / 'Caddyfile.trusted'),
        '-v', 'putgd-st-caddy-trusted-data:/data', '-v', 'putgd-st-sftp-upload:/public/sftp:ro',
        '-v', 'putgd-st-webdav-data:/public/webdav:ro')
    run(PODS['web'], 'putgd-st-caddy-selfsigned', IMAGES['caddy'], '-v', '%s:/etc/caddy/Caddyfile:ro,z' % (WEB / 'Caddyfile.selfsigned'),
        '-v', 'putgd-st-caddy-selfsigned-data:/data')
    for pod in PODS.values():
        podman('pod', 'start', pod, check=False)
    # The unprivileged user Apache runs as (uid 1) owns the WebDAV folder. Podman resets a new
    # volume's owner when its first container starts, so this comes after the pods start.
    subprocess.run(['podman', 'unshare', 'chown', '1:1', podman('volume', 'inspect', 'putgd-st-webdav-data', '--format', '{{.Mountpoint}}')], check=True)
    ready = {}
    for name in ('sftp', 'sftp_nosftp'):
        ready[name] = ssh_banner(bind, PORTS[name], 60)
        print('%-20s %s:%d  %s' % (name.replace('_', '-'), bind, PORTS[name], 'SSH' if ready[name] else 'no answer'), flush=True)
    nextcloud_setup(bind)
    for name in ('nextcloud', 'nextcloud_selfsigned', 'webdav', 'public_https', 'public_http'):
        url = base(bind, name)
        code = answers(url + ('/status.php' if name.startswith('nextcloud') else '/'), 60)
        ready[name] = 0 < code < 500
        print('%-20s %s  HTTP %s' % (name.replace('_', '-'), url, code or 'no answer'), flush=True)
    if not all(ready.values()):
        raise SystemExit('not ready: ' + ', '.join(name for name, ok in ready.items() if not ok) + ' did not answer; see "podman logs"')
    export_ca()
    # An existing pin is kept: after rotate-hostkey it is the "host key changed" case on purpose.
    if not (CLIENT / 'known_hosts').is_file():
        known_hosts(bind)
    write_env(bind)
    print('State is in %s (mode 0700). The env file for the harness: %s' % (STATE, ENV_FILE))
    return 0


def base(bind, name):
    scheme = 'http' if name == 'public_http' else 'https'
    return '%s://%s:%d' % (scheme, bind, PORTS[name])


def export_ca():
    root = podman('exec', 'putgd-st-caddy-trusted', 'cat', '/data/caddy/pki/authorities/local/root.crt', check=False)
    if not root.startswith('-----BEGIN CERTIFICATE-----'):
        raise SystemExit('the trusted Caddy has no root certificate yet: see "podman logs putgd-st-caddy-trusted"')
    path = write(CA_DIR / 'putgd-lab-root.crt', root + '\n', 0o644)
    digest = subprocess.run(['openssl', 'x509', '-noout', '-fingerprint', '-sha256', '-in', str(path)], text=True, capture_output=True).stdout.strip()
    print('CA root certificate: %s  %s' % (path, digest.partition('=')[2]))
    return path


def known_hosts(bind):
    """known_hosts lines for the two SFTP servers from their current host keys."""
    lines = []
    for name in ('sftp', 'sftp_nosftp'):
        for kind in ('ed25519', 'rsa'):
            key = (SERVER / 'hostkeys' / ('ssh_host_%s_key.pub' % kind)).read_text().split()
            lines.append('[%s]:%d %s %s\n' % (bind, PORTS[name], key[0], key[1]))
    path = write(CLIENT / 'known_hosts', ''.join(lines), 0o600)
    print('known_hosts for the SFTP servers: %s (ed25519 %s)' % (path, fingerprint(SERVER / 'hostkeys/ssh_host_ed25519_key.pub')))
    return path


def rotate_hostkey():
    if not (SERVER / 'hostkeys/ssh_host_ed25519_key').exists():
        raise SystemExit('no host keys yet: run "up" first')
    old = fingerprint(SERVER / 'hostkeys/ssh_host_ed25519_key.pub')
    host_keys(replace=True)
    for name in ('putgd-st-sshd', 'putgd-st-sshd-nosftp'):
        if exists('container', name):
            podman('restart', name)
    print('host key changed: ed25519 %s -> %s' % (old, fingerprint(SERVER / 'hostkeys/ssh_host_ed25519_key.pub')))
    print('client/known_hosts still holds the old key on purpose; "known-hosts --bind ADDRESS" pins the new one.')
    return 0


def quota(value):
    text = value.strip()
    if text.lower() == 'none':
        text = 'none'
    else:
        match = re.fullmatch(r'(\d+)\s*([KMG]B)', text, re.IGNORECASE)
        if not match:
            raise SystemExit('quota takes none or a size such as 5MB, 500KB, 2GB')
        text = '%s %s' % (match.group(1), match.group(2).upper())
    if container_state(NEXTCLOUD) != 'running':
        raise SystemExit('Nextcloud is not running: run "up" first')
    occ('user:setting', USERS['nextcloud'], 'files', 'quota', text)
    print('Nextcloud user %s quota: %s' % (USERS['nextcloud'], text))
    return 0


def secret_state(path):
    path = Path(path)
    if not path.is_file():
        return 'missing'
    mode = path.stat().st_mode & 0o777
    return 'present (mode %o, %s)' % (mode, '*' * 8)


def status(bind):
    ours = [(name, container_state(name)) for name in CONTAINERS]
    states = dict(ours)
    sftp_up = states['putgd-st-sshd'] == 'running' and ssh_banner(bind, PORTS['sftp'], 6)
    nosftp_up = states['putgd-st-sshd-nosftp'] == 'running' and ssh_banner(bind, PORTS['sftp_nosftp'], 6)
    keys = (SERVER / 'hostkeys/ssh_host_ed25519_key.pub')
    rows = [
        ('sftp', sftp_up, 'sftp://%s:%d' % (bind, PORTS['sftp']),
         'users putgd-key (key or passphrase key), putgd-pw (password); folders /upload /ro /full; host key %s' % (fingerprint(keys) if keys.exists() else 'none')),
        ('sftp-nosftp', nosftp_up, 'ssh://%s:%d' % (bind, PORTS['sftp_nosftp']), 'same users; shell and scp, no SFTP subsystem'),
    ]
    for name, label, details in (
            ('nextcloud', 'nextcloud', 'user putgd, admin putgd-admin; WebDAV %s/remote.php/dav/files/putgd/; CA exported (ca/putgd-lab-root.crt)'),
            ('nextcloud_selfsigned', 'nextcloud-selfsigned', 'same Nextcloud behind a certificate whose CA is not exported'),
            ('webdav', 'webdav', 'user putgd-dav, basic auth (Apache mod_dav), signed by the exported CA'),
            ('public_https', 'public-https', 'no login; /sftp/<file> serves the SFTP upload folder, /webdav/<file> the WebDAV folder; exported CA'),
            ('public_http', 'public-http', 'no login; the same two locations over plain HTTP')):
        url = base(bind, name)
        # Nextcloud's front page can take longer than the probe; its status page is quick.
        code = answers(url + ('/status.php' if name.startswith('nextcloud') else '/'), 8)
        rows.append((label, bool(code) and code < 500, url, details % url if '%s' in details else details))
    print('%-21s %-5s %-34s %s' % ('service', 'state', 'URL', 'details'))
    for name, up_now, url, details in rows:
        print('%-21s %-5s %-34s %s' % (name, 'up' if up_now else 'down', url, details))
    print('\ncontainers: ' + ', '.join('%s %s' % item for item in ours))
    print('\nsecrets (values are never printed):')
    for label, path in (('sftp key (putgd-key)', CLIENT / 'putgd-key.ed25519'), ('sftp passphrase key (putgd-key)', CLIENT / 'putgd-key-pass.ed25519'),
                        ('sftp passphrase', CLIENT / 'putgd-key-pass.passphrase'), ('sftp password (putgd-pw)', CLIENT / 'putgd-pw.password'),
                        ('webdav password (putgd-dav)', CLIENT / 'webdav.password'), ('nextcloud password (putgd)', CLIENT / 'nextcloud-putgd.password'),
                        ('nextcloud admin password', CLIENT / 'nextcloud-admin.password')):
        print('  %-33s %s' % (label, secret_state(path)))
    print('  %-33s %s' % ('CA root certificate (public)', 'present' if (CA_DIR / 'putgd-lab-root.crt').is_file() else 'missing'))
    print('  %-33s %s' % ('env file', ENV_FILE if ENV_FILE.is_file() else 'missing: run "env"'))
    return 0 if all(row[1] for row in rows) else 1


def write_env(bind):
    values = {
        'PUTGD_LAB_STORAGE_HOST': bind, 'PUTGD_LAB_STORAGE_DIR': STATE,
        'PUTGD_LAB_SFTP_PORT': PORTS['sftp'], 'PUTGD_LAB_SFTP_NOSFTP_PORT': PORTS['sftp_nosftp'],
        'PUTGD_LAB_SFTP_KNOWN_HOSTS': CLIENT / 'known_hosts',
        'PUTGD_LAB_SFTP_KEY_USER': USERS['sftp_key'], 'PUTGD_LAB_SFTP_KEY_FILE': CLIENT / 'putgd-key.ed25519',
        'PUTGD_LAB_SFTP_PASSPHRASE_KEY_FILE': CLIENT / 'putgd-key-pass.ed25519',
        'PUTGD_LAB_SFTP_PASSPHRASE_FILE': CLIENT / 'putgd-key-pass.passphrase',
        'PUTGD_LAB_SFTP_PW_USER': USERS['sftp_pw'], 'PUTGD_LAB_SFTP_PW_FILE': CLIENT / 'putgd-pw.password',
        'PUTGD_LAB_SFTP_UPLOAD_DIR': '/upload', 'PUTGD_LAB_SFTP_RO_DIR': '/ro', 'PUTGD_LAB_SFTP_FULL_DIR': '/full',
        'PUTGD_LAB_SFTP_PUBLIC_URL': '%s/sftp' % base(bind, 'public_https'),
        'PUTGD_LAB_NEXTCLOUD_URL': base(bind, 'nextcloud'), 'PUTGD_LAB_NEXTCLOUD_SELFSIGNED_URL': base(bind, 'nextcloud_selfsigned'),
        'PUTGD_LAB_NEXTCLOUD_USER': USERS['nextcloud'], 'PUTGD_LAB_NEXTCLOUD_PASSWORD_FILE': CLIENT / 'nextcloud-putgd.password',
        'PUTGD_LAB_NEXTCLOUD_ADMIN_USER': USERS['nextcloud_admin'], 'PUTGD_LAB_NEXTCLOUD_ADMIN_PASSWORD_FILE': CLIENT / 'nextcloud-admin.password',
        'PUTGD_LAB_CA_FILE': CA_DIR / 'putgd-lab-root.crt',
        'PUTGD_LAB_WEBDAV_URL': base(bind, 'webdav') + '/', 'PUTGD_LAB_WEBDAV_USER': USERS['webdav'],
        'PUTGD_LAB_WEBDAV_PASSWORD_FILE': CLIENT / 'webdav.password',
        'PUTGD_LAB_WEBDAV_PUBLIC_URL': '%s/webdav' % base(bind, 'public_https'),
        'PUTGD_LAB_PUBLIC_HTTP_URL': base(bind, 'public_http'),
    }
    write(ENV_FILE, '# put.gd storage lab servers: paths and URLs only, no secret values. Source it on this builder.\n'
          + ''.join('%s=%s\n' % item for item in values.items()))
    print('env file written: %s (mode 0600)' % ENV_FILE)
    return 0


def down(purge):
    for pod in PODS.values():
        if exists('pod', pod):
            podman('pod', 'rm', '-f', pod)
    if purge:
        for volume in VOLUMES:
            if exists('volume', volume):
                podman('volume', 'rm', volume)
        if exists('image', SSHD_IMAGE):
            podman('rmi', SSHD_IMAGE)
        # Exact files only, then the folders that are left empty.
        for name in STATE_FILES:
            (STATE / name).unlink(missing_ok=True)
        for name in STATE_DIRS:
            try:
                (STATE / name).rmdir()
            except OSError:
                pass
    print('servers stopped' + (' and their data removed' if purge else ''))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('up', 'status', 'env', 'ca', 'known-hosts', 'rotate-hostkey', 'quota', 'sharing',
                                            'share-password', 'check', 'down'))
    parser.add_argument('value', nargs='?', help='quota: 5MB or none; sharing and share-password: on or off')
    parser.add_argument('--bind', help='the builder address the servers listen on: its lab-network address, or 127.0.0.1')
    parser.add_argument('--rotate-hostkey', action='store_true', help='with check: also rotate the SSH host keys (off by default)')
    parser.add_argument('--no-settings', action='store_true', help='with check: skip the Nextcloud quota, sharing and share-password checks')
    parser.add_argument('--purge', action='store_true', help='with down: also remove volumes, passwords, keys and the built image')
    options = parser.parse_args()
    if options.command in ('up', 'status', 'env', 'ca', 'known-hosts', 'check') and not options.bind:
        parser.error('--bind is required')
    if options.command == 'down':
        return down(options.purge)
    if options.command == 'rotate-hostkey':
        return rotate_hostkey()
    if options.command in ('quota', 'sharing', 'share-password'):
        if not options.value:
            parser.error('%s needs a value' % options.command)
        if options.command == 'quota':
            return quota(options.value)
        if options.value not in ('on', 'off'):
            parser.error('%s takes on or off' % options.command)
        key = 'shareapi_enabled' if options.command == 'sharing' else 'shareapi_enforce_links_password'
        nextcloud_config(key, 'yes' if options.value == 'on' else 'no')
        print('Nextcloud %s: %s' % (options.command, options.value))
        return 0
    bind = lab_address(options.bind)
    if options.command == 'up':
        return up(bind)
    if options.command == 'ca':
        export_ca()
        return write_env(bind)
    if options.command == 'known-hosts':
        known_hosts(bind)
        return 0
    if options.command == 'env':
        return write_env(bind)
    if options.command == 'check':
        return subprocess.call([sys.executable, str(HERE / 'check.py'), '--bind', bind] + (['--rotate-hostkey'] if options.rotate_hostkey else [])
                               + (['--no-settings'] if options.no_settings else []))
    return status(bind)


if __name__ == '__main__':
    sys.exit(main())
