#!/usr/bin/env python3
"""Checks the lab storage servers (#1871, #1872) the way a desktop test would use them.

Run on the builder that hosts them: python3 check.py --bind 192.168.68.71 (or servers.py check).
It signs in to SFTP with a key, with a passphrase key through an ssh-agent and with a password,
proves the read-only and full folders and the no-SFTP port refuse as they should, drives WebDAV
(PUT, If-None-Match, DELETE), runs a Nextcloud Login Flow v2 sign-in in a headless browser, makes
public links and quota/sharing changes, and fetches every public URL with curl and a headless
Chromium (screenshots go to the state folder's checks/ folder).

Every file it makes has a name unique to the run, and only those are removed again. It does not
rotate the host keys unless --rotate-hostkey is given, and it puts the Nextcloud quota, sharing
and share-password settings back to what it read before it changed them (--no-settings skips
those checks and leaves the settings alone). Passwords, poll tokens and share links stay in
0600 files that curl and the browser read; none is on a command line. Prints one line per check,
never a secret; exits 0 when all pass.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = Path(__file__).resolve().parent
STATE = Path.home() / 'putgd-lab/storage-servers'
CLIENT = STATE / 'client'
CA = STATE / 'ca/putgd-lab-root.crt'
PLAYWRIGHT = 'mcr.microsoft.com/playwright/python@sha256:209deb0d8f0ba01f66a06d9a96b7ba9fdf14e4f33619abe03f9e6430192e3c2c'
results = []
RUN = secrets.token_hex(4)


def name(base):
    """A name unique to this run."""
    return 'check-%s-%s' % (RUN, base)


def private(path, text):
    """A file created with mode 0600 from the first byte."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, 'w') as file:
        file.write(text)
    return Path(path)


def option(key, value):
    """One line of a curl config file."""
    return '%s = %s\n' % (key, json.dumps(value))


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', name, '  (%s)' % detail if detail else ''), flush=True)
    return ok


def png(path, size=(160, 100), color=(40, 120, 200)):
    """A synthetic PNG: solid colour bands."""
    width, height = size
    rows = b''.join(b'\x00' + b''.join(bytes(color if (x // 20 + y // 20) % 2 else (240, 240, 240)) for x in range(width))
                    for y in range(height))
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def run(command, **options):
    return subprocess.run(command, text=True, capture_output=True, **options)


class Lab:
    def __init__(self, bind, work):
        self.bind, self.work = bind, Path(work)
        self.known = CLIENT / 'known_hosts'
        self.created = {}      # what this run made and has not yet removed: key -> a function that removes it
        self.prior = None      # the Nextcloud settings as read before the run changed them
        self.sftp_options = ['-P', '18371', '-o', 'UserKnownHostsFile=%s' % self.known, '-o', 'StrictHostKeyChecking=yes', '-o', 'IdentitiesOnly=yes']

    def askpass(self, secret_file):
        script = self.work / ('askpass-' + secret_file.name + '.sh')
        script.write_text('#!/bin/sh\ncat %s\n' % secret_file)
        script.chmod(0o700)
        return script

    def track(self, key, remover):
        self.created[key] = remover

    def untrack(self, key):
        self.created.pop(key, None)

    def cleanup(self):
        """Removes what this run made and did not already remove, newest first."""
        for key in reversed(list(self.created)):
            try:
                self.created.pop(key)()
            except Exception as error:
                print('cleanup %s failed: %s' % (key[0], type(error).__name__), flush=True)

    def sftp_rm(self, file_name):
        """Removes an upload, then asks the server for the exact path again: its answer is the proof."""
        return self.sftp('putgd-key', 'rm /upload/%s\nls -1 /upload/%s' % (file_name, file_name), key=CLIENT / 'putgd-key.ed25519', methods='publickey')

    def dav_rm(self, file_name):
        return self.curl('-X', 'DELETE', 'https://%s:18375/%s' % (self.bind, file_name), user='putgd-dav', password_file=CLIENT / 'webdav.password')

    def environment(self, secret_file=None, agent=None):
        env = dict(os.environ, SSH_ASKPASS_REQUIRE='force' if secret_file else 'never')
        env.pop('SSH_AUTH_SOCK', None)
        if secret_file:
            env['SSH_ASKPASS'] = str(self.askpass(secret_file))
        if agent:
            env['SSH_AUTH_SOCK'] = agent
        return env

    def sftp(self, user, commands, key=None, secret_file=None, agent=None, port=18371, methods=None, agent_key=None):
        options = ['-P', str(port)] + self.sftp_options[2:]
        if key:
            options += ['-i', str(key)]
        if agent_key:
            # IdentitiesOnly also limits the agent: the public key file names the one to offer.
            options += ['-i', str(agent_key) + '.pub']
        if methods == 'password':
            options += ['-o', 'PreferredAuthentications=password', '-o', 'PubkeyAuthentication=no']
        elif methods == 'publickey':
            options += ['-o', 'PreferredAuthentications=publickey', '-o', 'BatchMode=yes']
        result = run(['sftp', *options, '%s@%s' % (user, self.bind)], input=commands + '\nquit\n', env=self.environment(secret_file, agent),
                     timeout=60)
        return result.stdout + result.stderr

    def curl(self, *args, user=None, password_file=None, cacert=True, secret=()):
        """Runs curl; the login and anything secret (a poll token, a share link) go in a 0600 config
        file, never on the command line. Returns (HTTP status, curl's error text)."""
        command = ['curl', '-sS', '--max-time', '60', '-o', str(self.work / 'body'), '-w', '%{http_code}']
        command += ['--cacert', str(CA)] if cacert is True else (['--cacert', str(cacert)] if cacert else [])
        lines = list(secret)
        if user:
            lines.insert(0, option('user', '%s:%s' % (user, Path(password_file).read_text().strip())))
        if lines:
            config = private(self.work / 'curl.conf', ''.join(lines))
            command += ['--config', str(config)]
        result = run([*command, *args])
        return result.stdout.strip(), result.stderr.strip()

    def body(self):
        return (self.work / 'body').read_bytes()


def data(reply):
    """The data of an OCS reply: a dict on success, an empty list on a refusal."""
    found = reply.get('ocs', {}).get('data', {})
    return found if isinstance(found, dict) else {}


def listed(out, file_name):
    """True when the server's answer to `ls -1 /upload/<name>` names the file (the 'Uploading' and
    'Removing' echoes and error lines do not count)."""
    return any(line.strip() in ('/upload/' + file_name, file_name) for line in out.splitlines()
               if not line.startswith(('Uploading', 'Removing')))


def sftp_checks(lab):
    host = lab.bind
    upload = lab.work / 'sftp-upload.png'
    png(upload)
    key, passkey = CLIENT / 'putgd-key.ed25519', CLIENT / 'putgd-key-pass.ed25519'
    key_png, agent_png, pw_png = name('key.png'), name('agent.png'), name('pw.png')

    def made(file_name, out):
        """Registers a file the run uploaded, once the server's own listing shows it."""
        if listed(out, file_name):
            lab.track(('sftp', file_name), lambda: lab.sftp_rm(file_name))
        return listed(out, file_name)
    out = lab.sftp('putgd-key', 'put %s /upload/%s\nls -1 /upload/%s' % (upload, key_png, key_png), key=key, methods='publickey')
    check('sftp: key user uploads with the plain key', made(key_png, out), out.strip().splitlines()[-1][:80])
    out = lab.sftp('putgd-key', 'ls /upload', key=passkey, methods='publickey')
    check('sftp: the passphrase key is refused without an agent', 'denied' in out.lower() or 'passphrase' in out.lower(), out.strip().splitlines()[-1][:80] if out.strip() else '')
    agent = lab.work / 'agent.sock'
    started = run(['ssh-agent', '-a', str(agent)], env=lab.environment())
    pid = int(started.stdout.split('SSH_AGENT_PID=')[1].split(';')[0])
    try:
        added = run(['ssh-add', str(passkey)], stdin=subprocess.DEVNULL, env=lab.environment(CLIENT / 'putgd-key-pass.passphrase', agent=str(agent)))
        check('sftp: ssh-add takes the passphrase key (passphrase from a file)', added.returncode == 0, added.stderr.strip()[:80])
        out = lab.sftp('putgd-key', 'put %s /upload/%s\nls -1 /upload/%s' % (upload, agent_png, agent_png), agent=str(agent), methods='publickey', agent_key=passkey)
        check('sftp: key user uploads through the agent', made(agent_png, out), '')
    finally:
        os.kill(pid, 15)
    out = lab.sftp('putgd-pw', 'put %s /upload/%s\nls -1 /upload/%s' % (upload, pw_png, pw_png), secret_file=CLIENT / 'putgd-pw.password', methods='password')
    check('sftp: password user uploads', made(pw_png, out), '')
    out = lab.sftp('putgd-pw', 'ls /upload', key=key, methods='publickey')
    check('sftp: the password user refuses a key', 'denied' in out.lower(), '')
    out = lab.sftp('putgd-key', 'ls /upload', secret_file=CLIENT / 'putgd-pw.password', methods='password')
    check('sftp: the key user refuses a password', 'denied' in out.lower(), '')
    out = lab.sftp('putgd-key', 'put %s /ro/%s' % (upload, name('no.png')), key=key, methods='publickey')
    check('sftp: writing to /ro fails with permission denied', 'ermission denied' in out, out.strip().splitlines()[-1][:80])
    out = lab.sftp('putgd-key', 'ls /ro', key=key, methods='publickey')
    check('sftp: /ro can be read', 'readme.txt' in out, '')
    out = lab.sftp('putgd-key', 'put %s /full/%s' % (upload, name('no.png')), key=key, methods='publickey')
    check('sftp: writing to /full fails', 'write remote' in out and 'Failure' in out, out.strip().splitlines()[-1][:100])
    out = lab.sftp('putgd-key', 'df /full', key=key, methods='publickey')
    avail = [line.split() for line in out.splitlines() if line.startswith('Filesystem') is False and len(line.split()) >= 4 and line.split()[1].isdigit()]
    check('sftp: statvfs reports /full as having no free space (OpenSSH answers the write itself only with a generic Failure)',
          bool(avail) and avail[0][3] == '0', out.strip().splitlines()[-1][:100])
    out = lab.sftp('putgd-key', 'ls /', key=key, methods='publickey')
    check('sftp: the chroot shows only upload, ro and full', all(word in out for word in ('upload', 'ro', 'full')) and 'etc' not in out.split(), '')
    out = lab.sftp('putgd-key', 'ls /', key=key, methods='publickey', port=18372)
    check('sftp: the no-SFTP port refuses the subsystem', 'subsystem request failed' in out.lower() or 'connection closed' in out.lower(), out.strip().splitlines()[-1][:80])
    shell = run(['ssh', '-p', '18372', '-i', str(key), '-o', 'UserKnownHostsFile=%s' % lab.known, '-o', 'StrictHostKeyChecking=yes',
                 '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', 'putgd-key@' + host, 'echo shell-ok'], env=lab.environment())
    check('sftp: the no-SFTP port still gives a shell', 'shell-ok' in shell.stdout, shell.stderr.strip()[:80])
    # The key user's shell must not be able to read the password user's password or another
    # service's secret, wherever it might be mounted. Only which path was readable is reported.
    paths = ['/run/putgd-pw.password', '/server/putgd-pw.password', '/server/webdav.htpasswd', '/server/nextcloud.env',
             '/web/webdav.htpasswd', '/web/nextcloud.env']
    probe = 'for f in %s; do [ -r "$f" ] && echo "READABLE $f"; done; echo done' % ' '.join(paths)
    looked = run(['ssh', '-p', '18372', '-i', str(key), '-o', 'UserKnownHostsFile=%s' % lab.known, '-o', 'StrictHostKeyChecking=yes',
                  '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', 'putgd-key@' + host, probe], env=lab.environment())
    readable = [line.split()[1] for line in looked.stdout.splitlines() if line.startswith('READABLE ') and len(line.split()) == 2]
    check('sftp: the key user\'s shell on the no-SFTP port cannot read the password user\'s password or other services\' secrets',
          'done' in looked.stdout and not readable, ('readable: ' + ', '.join(readable)) if readable else ('' if 'done' in looked.stdout else 'probe did not finish'))
    status, error = lab.curl('https://%s:18376/sftp/%s' % (host, key_png))
    check('public: https /sftp/<file> serves the SFTP upload', status == '200' and lab.body() == upload.read_bytes(), 'HTTP %s %s' % (status, error))
    status, _ = lab.curl('http://%s:18377/sftp/%s' % (host, key_png), cacert=False)
    check('public: http /sftp/<file> serves the SFTP upload', status == '200' and lab.body() == upload.read_bytes(), 'HTTP %s' % status)
    status, _ = lab.curl('https://%s:18376/sftp/%s' % (host, name('missing.png')))
    check('public: a missing file is 404', status == '404', 'HTTP %s' % status)
    for file_name in (key_png, agent_png, pw_png):
        if ('sftp', file_name) in lab.created:
            out = lab.sftp_rm(file_name)
            # Removed, and the server no longer has the exact path.
            if check('sftp: delete %s' % file_name, 'Removing' in out and not listed(out, file_name) and 'not found' in out, ''):
                lab.untrack(('sftp', file_name))


def hostkey_checks(lab):
    key = CLIENT / 'putgd-key.ed25519'
    servers = HERE / 'servers.py'
    rotated = run([sys.executable, str(servers), 'rotate-hostkey'])
    check('rotate-hostkey changes the host key', rotated.returncode == 0 and 'host key changed' in rotated.stdout, '')
    time.sleep(2)
    out = lab.sftp('putgd-key', 'ls /', key=key, methods='publickey')
    check('sftp: the old pin is refused after the host key changed', 'IDENTIFICATION HAS CHANGED' in out or 'Host key verification failed' in out, '')
    pinned = run([sys.executable, str(servers), 'known-hosts', '--bind', lab.bind])
    out = lab.sftp('putgd-key', 'ls /', key=key, methods='publickey')
    check('sftp: signs in again once the new host key is pinned', pinned.returncode == 0 and 'upload' in out, '')


def webdav_checks(lab):
    base = 'https://%s:18375' % lab.bind
    password = CLIENT / 'webdav.password'
    upload = lab.work / 'dav.png'
    png(upload, color=(200, 80, 60))
    main_png, fresh_png, folder = name('a.png'), name('fresh.png'), name('folder')
    dav = {'user': 'putgd-dav', 'password_file': password}

    def put(file_name, *extra):
        status, _ = lab.curl('-X', 'PUT', '--data-binary', '@%s' % upload, *extra, '%s/%s' % (base, file_name), **dav)
        if status in ('201', '204'):
            lab.track(('dav', file_name), lambda: lab.dav_rm(file_name))
        return status
    status, _ = lab.curl('-X', 'PUT', '--data-binary', '@%s' % upload, '%s/%s' % (base, name('anon.png')))
    check('webdav: a PUT without login is 401', status == '401', 'HTTP %s' % status)
    status = put(main_png)
    check('webdav: PUT creates a file', status == '201', 'HTTP %s' % status)
    status = put(main_png, '-H', 'If-None-Match: *')
    check('webdav: PUT with If-None-Match: * on an existing file is 412', status == '412', 'HTTP %s' % status)
    status = put(fresh_png, '-H', 'If-None-Match: *')
    check('webdav: PUT with If-None-Match: * on a new file is 201', status == '201', 'HTTP %s' % status)
    status = put(main_png)
    check('webdav: a plain PUT over an existing file is 204', status == '204', 'HTTP %s' % status)
    status, _ = lab.curl('-X', 'MKCOL', '%s/%s/' % (base, folder), **dav)
    if status == '201':
        # The folder is this run's own and empty; removing it removes nothing else.
        lab.track(('dav', folder + '/'), lambda: lab.dav_rm(folder + '/'))
    check('webdav: MKCOL makes a folder', status == '201', 'HTTP %s' % status)
    status, _ = lab.curl('-X', 'PROPFIND', '-H', 'Depth: 1', base + '/', **dav)
    check('webdav: PROPFIND lists the folder', status == '207' and main_png.encode() in lab.body(), 'HTTP %s' % status)
    status, _ = lab.curl('%s/%s' % (base, main_png), **dav)
    check('webdav: GET with login returns the file', status == '200' and lab.body() == upload.read_bytes(), 'HTTP %s' % status)
    status, _ = lab.curl('https://%s:18376/webdav/%s' % (lab.bind, main_png))
    check('public: https /webdav/<file> serves the WebDAV file', status == '200' and lab.body() == upload.read_bytes(), 'HTTP %s' % status)
    status, _ = lab.curl('http://%s:18377/webdav/%s' % (lab.bind, main_png), cacert=False)
    check('public: http /webdav/<file> serves the WebDAV file', status == '200', 'HTTP %s' % status)
    status, error = lab.curl(base + '/', cacert=False)
    check('webdav: an unverified certificate is refused by curl without --cacert', status in ('000', '') and 'certificate' in error.lower(), error[:80])
    for target in (main_png, fresh_png, folder + '/'):
        if ('dav', target) in lab.created:
            status, _ = lab.dav_rm(target)
            if check('webdav: DELETE %s' % target, status == '204', 'HTTP %s' % status):
                lab.untrack(('dav', target))
    status, _ = lab.curl('-X', 'DELETE', '%s/%s' % (base, main_png), **dav)
    check('webdav: DELETE of a missing file is 404', status == '404', 'HTTP %s' % status)
    status, _ = lab.curl('https://%s:18376/webdav/%s' % (lab.bind, main_png))
    check('public: the deleted file is 404', status == '404', 'HTTP %s' % status)


def container(lab, jobs):
    """Runs browser jobs in the Playwright container; returns its output lines. The jobs, which
    carry login and share links, are a 0600 file in the mounted checks folder, not an argument."""
    shots = STATE / 'checks'
    shots.mkdir(mode=0o700, exist_ok=True)
    shutil.copyfile(HERE / 'browser_check.py', shots / 'browser_check.py')
    job_file = private(shots / ('jobs-%s.json' % RUN), json.dumps(jobs))
    try:
        # Only the checks folder and one password file are mounted (and relabelled for this container):
        # mounting the whole state folder would take the servers' own files away from their containers.
        result = run(['podman', 'run', '--rm', '--network', 'host', '-v', '%s:/checks:Z' % shots,
                      '-v', '%s:/secrets/nextcloud-putgd.password:ro,Z' % (CLIENT / 'nextcloud-putgd.password'), PLAYWRIGHT, 'sh', '-c',
                      'pip install -q playwright==1.49.1 >/dev/null 2>&1; python3 /checks/browser_check.py "$0"', '/checks/' + job_file.name])
    finally:
        job_file.unlink(missing_ok=True)
    return result.stdout.strip().splitlines(), result.stderr.strip()[-300:]


def occ(*args):
    return run(['podman', 'exec', '-u', 'www-data', 'putgd-st-nextcloud', 'php', 'occ', *args])


def read_settings():
    """The Nextcloud settings the checks change, as stored now. A value that is not set is None;
    a read that fails raises, so nothing is changed on a guess."""
    listed_core = occ('config:list', 'core', '--output=json')
    try:
        stored = json.loads(listed_core.stdout)['apps']['core']
    except (ValueError, KeyError, TypeError):
        stored = None
    if listed_core.returncode or stored is None:
        raise RuntimeError('config:list core failed')
    quota = occ('user:setting', 'putgd', 'files', 'quota')
    if quota.returncode == 0:
        current = quota.stdout.strip()
    elif 'The setting does not exist' in quota.stdout + quota.stderr:
        current = None
    else:
        raise RuntimeError('reading the quota failed')
    return {'quota': current, 'shareapi_enabled': stored.get('shareapi_enabled'),
            'shareapi_enforce_links_password': stored.get('shareapi_enforce_links_password')}


def restore_settings(prior):
    """Puts the quota, sharing and share-password settings back to what read_settings saw.
    Returns the names of the settings that could not be restored."""
    failed = []
    commands = [('quota', ['user:setting', 'putgd', 'files', 'quota', prior['quota']] if prior['quota'] is not None
                 else ['user:setting', '--delete', 'putgd', 'files', 'quota'])]
    for key in ('shareapi_enabled', 'shareapi_enforce_links_password'):
        value = prior[key]
        commands.append((key, ['config:app:delete', 'core', key] if value is None else
                         ['config:app:set', 'core', key, '--value=' + ('yes' if value in (True, 'yes', '1', 'true') else 'no')]))
    for label, command in commands:
        if occ(*command).returncode:
            failed.append(label)
    print('Nextcloud quota, sharing and share-password put back to the values read before the checks' if not failed
          else 'FAIL could not restore: ' + ', '.join(failed), flush=True)
    return failed


def nextcloud_checks(lab, settings):
    nc = 'https://%s:18374' % lab.bind
    selfsigned = 'https://%s:18373' % lab.bind
    status, _ = lab.curl(nc + '/status.php')
    check('nextcloud: status.php over the trusted listener (CA pinned)', status == '200' and b'"installed":true' in lab.body(), 'HTTP %s' % status)
    status, error = lab.curl(nc + '/status.php', cacert=False)
    check('nextcloud: the same listener fails without the CA', status in ('000', '') and 'certificate' in error.lower(), error[:70])
    status, error = lab.curl(selfsigned + '/status.php')
    check('nextcloud: the self-signed listener is not trusted by the exported CA', status in ('000', '') and 'certificate' in error.lower(), error[:70])
    status, _ = lab.curl('-k', selfsigned + '/status.php', cacert=False)
    check('nextcloud: the self-signed listener serves the same Nextcloud with -k', status == '200', 'HTTP %s' % status)
    # Login Flow v2: start, sign in in a browser, poll.
    status, _ = lab.curl('-X', 'POST', '-A', 'put.gd lab check', nc + '/index.php/login/v2')
    flow = json.loads(lab.body()) if status == '200' else {}
    check('nextcloud: POST /index.php/login/v2 answers with a login URL and a poll endpoint', 'login' in flow and 'poll' in flow, 'HTTP %s' % status)
    if not flow:
        return
    check('nextcloud: the login URL names the LAN address and the HTTPS port', flow['login'].startswith(nc + '/'), '')
    poll = [option('data-urlencode', 'token=%s' % flow['poll']['token'])]
    status, _ = lab.curl('-X', 'POST', flow['poll']['endpoint'], secret=poll)
    check('nextcloud: polling before sign-in answers 404', status == '404', 'HTTP %s' % status)
    lines, error = container(lab, [{'kind': 'login_flow', 'name': 'nextcloud', 'url': flow['login'], 'user': 'putgd',
                                    'password_file': '/secrets/nextcloud-putgd.password', 'shot': '/checks/nextcloud-login-flow.png'}])
    check('nextcloud: headless Chromium signs in and grants access', any('login_flow nextcloud True' in line for line in lines), ' '.join(lines)[:100] or error)
    status, _ = lab.curl('-X', 'POST', flow['poll']['endpoint'], secret=poll)
    reply = json.loads(lab.body()) if status == '200' else {}
    check('nextcloud: polling after sign-in returns the server, login name and an app password',
          bool(reply.get('appPassword')) and reply.get('loginName') == 'putgd' and reply.get('server', '').startswith(nc), 'HTTP %s' % status)
    if not reply.get('appPassword'):
        return
    app_password = private(lab.work / 'app-password', reply['appPassword'])
    ocs = ['-H', 'OCS-APIRequest: true', '-H', 'Accept: application/json']
    login = {'user': 'putgd', 'password_file': app_password}
    # Registered first, so it is the last thing cleanup does: the other removals need it.
    lab.track(('nc-apppw',), lambda: lab.curl('-X', 'DELETE', nc + '/ocs/v2.php/core/apppassword', *ocs, **login))
    dav = nc + '/remote.php/dav/files/putgd/'
    upload = lab.work / 'nc.png'
    png(upload, color=(60, 160, 90))
    nc_png, big = name('nc.png'), name('big.bin')

    def nc_put(file_name, source, *extra):
        status, _ = lab.curl('-X', 'PUT', '--data-binary', '@%s' % source, *extra, dav + file_name, **login)
        if status in ('201', '204'):
            lab.track(('nc', file_name), lambda: lab.curl('-X', 'DELETE', dav + file_name, **login))
        return status
    status = nc_put(nc_png, upload)
    check('nextcloud: WebDAV PUT with the app password', status in ('201', '204'), 'HTTP %s' % status)
    status = nc_put(nc_png, upload, '-H', 'If-None-Match: *')
    check('nextcloud: PUT with If-None-Match: * on an existing file is 412', status == '412', 'HTTP %s' % status)
    servers = HERE / 'servers.py'

    def share(extra=()):
        status, _ = lab.curl('-X', 'POST', *ocs, '--data-urlencode', 'path=/%s' % nc_png, '--data-urlencode', 'shareType=3', *extra,
                             nc + '/ocs/v2.php/apps/files_sharing/api/v1/shares', **login)
        try:
            return status, json.loads(lab.body())
        except ValueError:
            return status, {}
    if settings:
        try:
            lab.prior = read_settings()
        except RuntimeError as error:
            # Nothing has been changed: the settings checks are skipped.
            check('nextcloud: the quota, sharing and share-password settings could be read first', False, str(error))
            settings = False
    if settings:
        run([sys.executable, str(servers), 'sharing', 'on'])
        run([sys.executable, str(servers), 'share-password', 'off'])
    status, reply = share()
    url = data(reply).get('url', '')
    check('nextcloud: a public share link is created', status == '200' and '/s/' in url, 'HTTP %s' % status)
    if url:
        status, _ = lab.curl(secret=[option('url', url)])
        check('nextcloud: the share link loads with curl', status == '200', 'HTTP %s' % status)
        lines, error = container(lab, [{'kind': 'shot', 'name': 'share', 'url': url, 'shot': '/checks/nextcloud-share.png'}])
        check('nextcloud: the share link loads in headless Chromium', any(line.startswith('shot share 200') for line in lines), ' '.join(lines)[:100] or error)
    if settings:
        flipped = run([sys.executable, str(servers), 'share-password', 'on'])
        status, reply = share()
        check('nextcloud: share-password on: a link without a password is refused', flipped.returncode == 0 and 'url' not in data(reply), 'HTTP %s' % status)
        run([sys.executable, str(servers), 'share-password', 'off'])
        run([sys.executable, str(servers), 'sharing', 'off'])
        status, reply = share()
        check('nextcloud: sharing off: creating a link is refused', 'url' not in data(reply), 'HTTP %s' % status)
        run([sys.executable, str(servers), 'sharing', 'on'])
        run([sys.executable, str(servers), 'quota', '1MB'])
        blob = lab.work / 'big.bin'
        blob.write_bytes(os.urandom(2 * 1024 * 1024))
        status = nc_put(big, blob)
        check('nextcloud: quota 1MB: a 2 MB PUT is refused with 507', status == '507', 'HTTP %s' % status)
        run([sys.executable, str(servers), 'quota', 'none'])
        status = nc_put(big, blob)
        check('nextcloud: quota none: the same PUT works', status in ('201', '204'), 'HTTP %s' % status)
    for target in (big, nc_png):
        if ('nc', target) in lab.created:
            status, _ = lab.curl('-X', 'DELETE', dav + target, **login)
            if check('nextcloud: DELETE %s' % target, status == '204', 'HTTP %s' % status):
                lab.untrack(('nc', target))
    status, _ = lab.curl('-X', 'DELETE', nc + '/ocs/v2.php/core/apppassword', *ocs, **login)
    if check('nextcloud: the app password is revoked', status == '200', 'HTTP %s' % status):
        lab.untrack(('nc-apppw',))


def public_browser_checks(lab):
    upload = lab.work / 'pub.png'
    png(upload, size=(240, 140), color=(160, 90, 190))
    key = CLIENT / 'putgd-key.ed25519'
    password = CLIENT / 'webdav.password'
    host = lab.bind
    public = name('public.png')
    out = lab.sftp('putgd-key', 'put %s /upload/%s\nls -1 /upload/%s' % (upload, public, public), key=key, methods='publickey')
    if listed(out, public):
        lab.track(('sftp', public), lambda: lab.sftp_rm(public))
    status, _ = lab.curl('-X', 'PUT', '--data-binary', '@%s' % upload, 'https://%s:18375/%s' % (host, public), user='putgd-dav', password_file=password)
    if status in ('201', '204'):
        lab.track(('dav', public), lambda: lab.dav_rm(public))
    lines, error = container(lab, [
        {'kind': 'shot', 'name': 'sftp-public-https', 'url': 'https://%s:18376/sftp/%s' % (host, public), 'shot': '/checks/public-sftp-https.png'},
        {'kind': 'shot', 'name': 'webdav-public-https', 'url': 'https://%s:18376/webdav/%s' % (host, public), 'shot': '/checks/public-webdav-https.png'},
        {'kind': 'shot', 'name': 'webdav-public-http', 'url': 'http://%s:18377/webdav/%s' % (host, public), 'shot': '/checks/public-webdav-http.png'}])
    for label in ('sftp-public-https', 'webdav-public-https', 'webdav-public-http'):
        check('public: headless Chromium loads %s' % label, any(line.startswith('shot %s 200' % label) for line in lines), ' '.join(lines)[:100] or error)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bind', required=True)
    parser.add_argument('--rotate-hostkey', action='store_true', help='also rotate the SSH host keys and check the old pin is refused (not done by default)')
    parser.add_argument('--no-settings', action='store_true', help='skip the Nextcloud quota, sharing and share-password checks and leave those settings alone')
    parser.add_argument('--only', choices=('sftp', 'webdav', 'nextcloud', 'public'), action='append')
    options = parser.parse_args()
    work = tempfile.mkdtemp(prefix='putgd-st-check-')
    lab = Lab(options.bind, work)
    try:
        wanted = options.only or ['sftp', 'webdav', 'nextcloud', 'public']
        if 'sftp' in wanted:
            sftp_checks(lab)
            if options.rotate_hostkey:
                hostkey_checks(lab)
        if 'webdav' in wanted:
            webdav_checks(lab)
        if 'nextcloud' in wanted:
            nextcloud_checks(lab, not options.no_settings)
        if 'public' in wanted:
            public_browser_checks(lab)
    finally:
        # What this run made, then the settings it changed, then its own private folder's files.
        lab.cleanup()
        if lab.prior and restore_settings(lab.prior):
            results.append(False)
        for leftover in Path(work).iterdir():
            leftover.unlink()
        Path(work).rmdir()
    print('%d of %d checks passed' % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
