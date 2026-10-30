"""Importable physical.py transport: pinned SSH, SCP and shared VNC witnesses."""
from pathlib import Path
try:
    from . import lab, rfb
except ImportError:
    import lab, rfb


class PhysicalHost:
    def __init__(self, config):
        self.config = config
        self.run = Path(config['run']).resolve()
        self.ssh_arguments = ['ssh', '-i', str(Path(config['identity']).expanduser()), '-o', 'IdentitiesOnly=yes',
                              '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=8',
                              '-o', 'UserKnownHostsFile=' + config['known_hosts']]
        if config.get('ssh_port'):
            self.ssh_arguments += ['-p', str(config['ssh_port'])]
        self.endpoint = config['user'] + '@' + config['host']
        self.scp_arguments = ['scp', '-q'] + self.ssh_arguments[1:]
        if config.get('ssh_port'):
            self.scp_arguments[-2] = '-P'

    def ssh(self, _run, script, timeout=60, bootstrap=False):
        return lab.command(self.ssh_arguments + [self.endpoint, 'sh', '-s'], timeout=timeout, input=script).stdout

    def put(self, _run, local, remote):
        lab.command(self.scp_arguments + [str(local), self.endpoint + ':' + str(remote)])

    def get(self, _run, remote, local):
        lab.command(self.scp_arguments + [self.endpoint + ':' + str(remote), str(local)])

    def screenshot(self, _run, name='witness', head=0):
        path = (self.run / f'{name}-head{head}.ppm').resolve()
        if path.parent != self.run:
            raise lab.LabError('Witness filename must stay in run directory')
        client = rfb.RFB(self.config['vnc_port'])
        try:
            w, h, bgrx = client.frame()
        finally:
            client.close()
        rgb = bytearray(w * h * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = bgrx[2::4], bgrx[1::4], bgrx[0::4]
        path.write_bytes(b'P6\n%d %d\n255\n' % (w, h) + bytes(rgb))
        return {'path': str(path), 'sha256': lab.hash_file(path)}
