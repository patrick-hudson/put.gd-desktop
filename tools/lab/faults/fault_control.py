"""Fault plans for the put.gd fault-injection harness (tools/lab/action_fuzz.py faults).

  FaultControl   the LD_PRELOAD shim (faultshim.c), its rule file and its log; the network proxy (fault_proxy.py);
                 an optional small filesystem the resident's data lives on, which can be squeezed to the brink of full;
                 resource limits (open files, memory).  Every random choice comes from the harness's seeded generator
                 and is logged, so a finding replays from its seed and --steps.
"""
import json
import os
from pathlib import Path
import re
import resource
import shutil
import stat
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fault_proxy  # noqa: E402

# What a file the resident writes is called; a rule's path filter picks one of these (or any file under the run).
FS_TARGETS = {
    'database': 'history.sqlite',
    'config': 'config.json',
    'journal': 'journal',
    'archive': '/archive/',
    'state': '/state/',
    'data': '/data/',
    'cache': '/cache/',
    'source': '/fixtures/u',
    'runtime': '/srt/|/rt/',
    'everything': '/r/|/archive/',
}


class SmallFs:
    """A small filesystem prepared by tools/lab/faults/smallfs.sh (tmpfs or loop ext4). The harness fills it, never mounts it.

    claim() refuses anything but a fresh mount that smallfs.sh made: the folder must be a mount point of its own, match the record
    smallfs.sh wrote next to it (DIR.smallfs: the tmpfs source name, or the loop image and its inode), be no larger than `limit`,
    and hold nothing yet. Only then is the filler created, exclusively and without following a link, and the harness keeps its
    descriptor: it never opens the filler by name again. A plain folder, the host's own filesystem or a planted .filler link is
    refused before anything is written."""

    LIMIT = 4 << 30

    def __init__(self, path):
        self.path = Path(os.path.realpath(path))
        self.filler = self.path / '.filler'
        self.fd = None

    @staticmethod
    def _unescape(field):
        return re.sub(r'\\([0-7]{3})', lambda m: chr(int(m.group(1), 8)), field)

    def mount_entry(self):
        """(fstype, source) of the topmost mount exactly at the path, from /proc/self/mountinfo, or None."""
        found = None
        with open('/proc/self/mountinfo') as table:
            for line in table:
                left, _, right = line.partition(' - ')
                fields, tail = left.split(), right.split()
                if len(fields) >= 5 and len(tail) >= 2 and self._unescape(fields[4]) == str(self.path):
                    found = (tail[0], self._unescape(tail[1]))
        return found

    def record(self):
        path = Path(str(self.path) + '.smallfs')
        st = path.lstat()
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid():
            raise SmallFsRefused('%s is not a record this user wrote' % path)
        return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)

    def verify(self):
        """Raise SmallFsRefused unless the path is the size-limited mount smallfs.sh made and recorded."""
        entry = self.mount_entry()
        if entry is None or os.stat(self.path).st_dev == os.stat(self.path.parent).st_dev:
            raise SmallFsRefused('%s is not a mount point: make it with tools/lab/faults/smallfs.sh mount' % self.path)
        fstype, source = entry
        try:
            record = self.record()
        except (OSError, ValueError) as error:
            raise SmallFsRefused('%s has no smallfs.sh record (%s): it was not mounted by smallfs.sh' % (self.path, error))
        if record.get('kind') == 'tmpfs':
            if fstype != 'tmpfs' or source != record.get('source'):
                raise SmallFsRefused('the mount at %s (%s %s) is not the tmpfs smallfs.sh recorded (%s)' % (self.path, fstype, source, record.get('source')))
        elif record.get('kind') == 'ext4':
            image = record.get('image', '')
            try:
                backing = Path('/sys/block/%s/loop/backing_file' % os.path.basename(source)).read_text().strip()
                image_st = os.stat(image)
            except OSError as error:
                raise SmallFsRefused('the mount at %s has no loop image to check (%s)' % (self.path, error))
            if fstype != 'ext4' or backing != image or '%d:%d' % (image_st.st_dev, image_st.st_ino) != record.get('identity'):
                raise SmallFsRefused('the mount at %s (%s on %s) is not the image smallfs.sh recorded (%s)' % (self.path, fstype, backing, image))
        else:
            raise SmallFsRefused('the smallfs.sh record of %s names no kind' % self.path)
        st = os.statvfs(self.path)
        if st.f_blocks * st.f_frsize > self.LIMIT:
            raise SmallFsRefused('%s is %d MiB: more than a small filesystem (%d MiB)' % (self.path, st.f_blocks * st.f_frsize >> 20, self.LIMIT >> 20))

    @classmethod
    def claim(cls, path):
        """Verify a fresh smallfs.sh mount and take it: create the filler and hold it open."""
        small = cls(path)
        small.verify()
        left = sorted(set(os.listdir(small.path)) - {'lost+found'})
        if left:
            raise SmallFsRefused('%s is not fresh (it holds %s): mount a new one for each run' % (small.path, left[:5]))
        try:
            small.fd = os.open(small.filler, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        except OSError as error:
            raise SmallFsRefused('cannot create the filler %s exclusively: %s' % (small.filler, error))
        return small

    def free(self):
        st = os.statvfs(self.path)
        return st.f_bavail * st.f_frsize

    def squeeze(self, free_bytes):
        """Leave about `free_bytes` of space: the filler takes the rest."""
        if self.fd is None:
            return
        held = os.fstat(self.fd).st_size
        want = max(0, held + self.free() - free_bytes)
        try:
            os.ftruncate(self.fd, 0)
            if want:
                os.posix_fallocate(self.fd, 0, want)
        except OSError:
            pass  # a full disk is the point: the filler took what there was

    def release(self):
        if self.fd is None:
            return
        try:
            os.ftruncate(self.fd, 0)
        except OSError:
            pass

    def close(self):
        """Give the space back and close the filler."""
        if self.fd is None:
            return
        try:
            self.release()
        finally:
            os.close(self.fd)
            self.fd = None


class SmallFsRefused(Exception):
    pass


class FaultControl:
    def __init__(self, run, rng, classes, source_dir, target, small_fs=None, small_rt=None, log=None, seed=1, net_kinds=None):
        """target: the resident's executable, the only program the shim injects into. small_fs, small_rt: SmallFs.claim() results."""
        self.run, self.rng, self.classes = run, rng, set(classes)
        self.log = log or (lambda *a, **k: None)
        self.dir = run / 'faults'
        self.dir.mkdir(exist_ok=True)
        self.spec = self.dir / 'faults.spec'
        self.shim_log = run / 'logs' / 'faultshim.log'
        self.shim = self.dir / 'faultshim.so'
        self.seed = seed
        self.net_kinds = net_kinds
        self.rules = []
        self.target = os.path.realpath(target)
        self.small_fs, self.small_rt = small_fs, small_rt
        self.proxy = None
        self.nofile_lowered = False
        self.nofile_events = 0
        self.nofile_starts = set()   # the resident starts (Resident.starts) during which its open-file limit was lowered
        self.build_shim(source_dir)
        self.write_spec([])

    # --- the shim

    def build_shim(self, source_dir):
        source = Path(source_dir) / 'faultshim.c'
        subprocess.run(['gcc', '-O2', '-Wall', '-shared', '-fPIC', '-o', str(self.shim), str(source), '-ldl', '-lpthread'], check=True)

    def environment(self):
        # The shim binds to the first process of the target executable and strips these from its environment, so helpers run without it.
        return {'LD_PRELOAD': str(self.shim), 'PUTGD_FAULT_SPEC': str(self.spec), 'PUTGD_FAULT_EXE': self.target, 'PUTGD_FAULT_SHIM': '1'}

    def faulted_pids(self):
        """{pid: comm} of every process the shim injected a fault into, from its log."""
        found = {}
        if self.shim_log.exists():
            for line in self.shim_log.read_text(errors='replace').splitlines():
                fields = line.split(' ', 3)
                if len(fields) >= 3 and fields[1].isdigit():
                    found[int(fields[1])] = fields[2]
        return found

    def write_spec(self, rules):
        """Atomically replace the rule file; the shim notices the new mtime within 50 ms."""
        self.rules = list(rules)
        text = 'seed %d\nlog %s\n%s\n' % (self.seed, self.shim_log, '\n'.join(rules))
        temporary = self.dir / 'faults.spec.new'
        temporary.write_text(text)
        os.replace(temporary, self.spec)
        self.log('fault-fs', rules=self.rules)

    def random_fs_rules(self):
        rng = self.rng
        rules = []
        for _ in range(rng.choice((1, 1, 2, 3))):
            target = FS_TARGETS[rng.choice(list(FS_TARGETS))]
            probability = rng.choice((0.02, 0.1, 0.3, 1.0))
            kind = rng.choice(('space', 'space', 'io', 'io', 'perm', 'fsync', 'rename', 'short', 'slow', 'open', 'read', 'fds'))
            skip = rng.choice((0, 0, 0, 2, 10, 40))
            tail = ' skip=%d' % skip if skip else ''
            if rng.random() < 0.3:
                tail += ' count=%d' % rng.choice((1, 2, 5, 20))
            if kind == 'space':
                rules.append('rule write,truncate,fallocate %s %s ENOSPC%s' % (target, probability, tail))
            elif kind == 'io':
                rules.append('rule write,truncate %s %s EIO%s' % (target, probability, tail))
            elif kind == 'perm':
                rules.append('rule open,rename,unlink,mkdir %s %s %s%s' % (target, probability, rng.choice(('EACCES', 'EROFS')), tail))
            elif kind == 'fsync':
                rules.append('rule fsync %s %s %s%s' % (target, probability, rng.choice(('EIO', 'ENOSPC', 'EINTR')), tail))
            elif kind == 'rename':
                rules.append('rule rename %s %s %s%s' % (target, probability, rng.choice(('EIO', 'ENOSPC', 'EXDEV', 'EBUSY', 'EACCES')), tail))
            elif kind == 'short':
                rules.append('rule write %s %s short%s' % (target, probability, tail))
            elif kind == 'slow':
                rules.append('rule write,fsync,rename %s %s delay:%d%s' % (target, min(probability, 0.3), rng.choice((20, 100, 400)), tail))
            elif kind == 'open':
                rules.append('rule open %s %s %s%s' % (target, probability, rng.choice(('ENOSPC', 'EIO', 'EMFILE', 'ENOENT', 'EACCES')), tail))
            elif kind == 'read':
                rules.append('rule read %s %s EIO%s' % (target, probability, tail))
            else:
                rules.append('rule open,mkdir %s %s %s%s' % (target, probability, rng.choice(('EMFILE', 'ENFILE', 'ENOMEM')), tail))
        return rules

    # --- network

    def start_proxy(self, upstream_port):
        self.proxy = fault_proxy.Proxy(upstream_port, seed=self.seed, log_path=str(self.run / 'logs' / 'proxy.jsonl')).start()
        return self.proxy.port

    def random_net_profile(self):
        rng = self.rng
        kinds = self.net_kinds or ['reset_before', 'reset_request', 'reset_after_request', 'cut_response', 'stall', 'stall_response', 'dup_response', 'slow', 'latency']
        chosen = rng.sample(kinds, min(len(kinds), rng.choice((1, 2, 3, 5))))
        return {'fault_rate': rng.choice((0.1, 0.3, 0.6, 1.0)), 'weights': {k: rng.choice((1, 2, 4)) for k in chosen},
                'latency_ms': rng.choice((0, 0, 5, 40, 200)), 'jitter_ms': rng.choice((0, 0, 5, 40)), 'stall': rng.choice((3, 6, 12))}

    def set_net(self, profile):
        if self.proxy:
            self.proxy.set_profile(profile)
            self.log('fault-net', profile=profile)

    # --- resources

    def lower_nofile(self, pid, headroom, start=None):
        """Cap the open-file limit of a running process just above what it holds now (and so its next open fails with EMFILE)."""
        try:
            held = len(os.listdir('/proc/%d/fd' % pid))
            limit = held + headroom
            _, hard = resource.prlimit(pid, resource.RLIMIT_NOFILE)
            resource.prlimit(pid, resource.RLIMIT_NOFILE, (min(limit, hard), hard))
            self.nofile_lowered = True
            self.nofile_events += 1
            self.nofile_starts.add(start)
            self.log('fault-nofile', pid=pid, held=held, limit=limit)
            return True
        except (OSError, ValueError, ProcessLookupError):
            return False

    def restore_nofile(self, pids):
        if not self.nofile_lowered:
            return
        for pid in pids:
            try:
                _, hard = resource.prlimit(pid, resource.RLIMIT_NOFILE)
                resource.prlimit(pid, resource.RLIMIT_NOFILE, (min(1024, hard), hard))
            except (OSError, ValueError, ProcessLookupError):
                pass
        self.nofile_lowered = False

    # --- everything

    def clear_all(self, pids=()):
        self.write_spec([])
        if self.proxy:
            self.proxy.clear()
        for small in (self.small_fs, self.small_rt):
            if small:
                small.release()
        self.restore_nofile(pids)

    def tail(self, count=40):
        lines = []
        for path in (self.shim_log,):
            if path.exists():
                lines += path.read_text(errors='replace').splitlines()[-count:]
        return lines

    def stop(self):
        if self.proxy:
            self.proxy.stop()
        for small in (self.small_fs, self.small_rt):
            if small:
                small.close()
