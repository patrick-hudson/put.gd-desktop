"""Fault plans for the put.gd fault-injection harness (tools/lab/action_fuzz.py faults).

  FaultControl   the LD_PRELOAD shim (faultshim.c), its rule file and its log; the network proxy (fault_proxy.py);
                 an optional small filesystem the resident's data lives on, which can be squeezed to the brink of full;
                 resource limits (open files, memory).  Every random choice comes from the harness's seeded generator
                 and is logged, so a finding replays from its seed and --steps.
"""
import json
import os
from pathlib import Path
import resource
import shutil
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
    """A small filesystem prepared by tools/lab/faults/smallfs.sh (tmpfs or loop ext4). The harness fills it, never mounts it."""

    def __init__(self, path):
        self.path = Path(path)
        self.filler = self.path / '.filler'

    def free(self):
        st = os.statvfs(self.path)
        return st.f_bavail * st.f_frsize

    def squeeze(self, free_bytes):
        """Leave about `free_bytes` of space: a filler file takes the rest."""
        try:
            held = self.filler.stat().st_size if self.filler.exists() else 0
        except OSError:
            held = 0
        want = max(0, held + self.free() - free_bytes)
        try:
            fd = os.open(self.filler, os.O_WRONLY | os.O_CREAT, 0o600)
            try:
                os.ftruncate(fd, 0)
                if want:
                    os.posix_fallocate(fd, 0, want)
            finally:
                os.close(fd)
        except OSError:
            pass  # a full disk is the point: the filler took what there was

    def release(self):
        try:
            os.ftruncate(os.open(self.filler, os.O_WRONLY), 0)
        except OSError:
            pass


class FaultControl:
    def __init__(self, run, rng, classes, source_dir, small_fs=None, small_rt=None, log=None, seed=1, net_kinds=None):
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
        self.small_fs = SmallFs(small_fs) if small_fs else None
        self.small_rt = SmallFs(small_rt) if small_rt else None
        self.proxy = None
        self.nofile_lowered = False
        self.build_shim(source_dir)
        self.write_spec([])

    # --- the shim

    def build_shim(self, source_dir):
        source = Path(source_dir) / 'faultshim.c'
        subprocess.run(['gcc', '-O2', '-Wall', '-shared', '-fPIC', '-o', str(self.shim), str(source), '-ldl', '-lpthread'], check=True)

    def environment(self):
        return {'LD_PRELOAD': str(self.shim), 'PUTGD_FAULT_SPEC': str(self.spec), 'PUTGD_FAULT_SHIM': '1'}

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

    def lower_nofile(self, pid, headroom):
        """Cap the open-file limit of a running process just above what it holds now (and so its next open fails with EMFILE)."""
        try:
            held = len(os.listdir('/proc/%d/fd' % pid))
            limit = held + headroom
            _, hard = resource.prlimit(pid, resource.RLIMIT_NOFILE)
            resource.prlimit(pid, resource.RLIMIT_NOFILE, (min(limit, hard), hard))
            self.nofile_lowered = True
            self.log('fault-nofile', pid=pid, held=held, limit=limit)
        except (OSError, ValueError, ProcessLookupError):
            pass

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
                small.release()
