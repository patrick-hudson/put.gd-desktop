#!/usr/bin/env python3
"""Disk-backed scratch directories for lab tools (stdlib only).

/tmp on the builders is a tmpfs: what a run leaves there stays in RAM until a reboot. Campaign
scratch left there (5.7 GB on k72, 4.5 GB on f44) took the memory the nightly guests need, and the
k72 and f44 Tier 1 rows ran nothing for two nights (#1978). Lab tools make their scratch
directories here instead, under ~/work/tmp, which is on disk. The path stays short, so a Unix
socket inside one still fits the 107-byte limit.

  python3 tools/lab/scratch.py [PREFIX]    make a new private directory and print its path

In a shell recipe without the checkout: mkdir -p -m 700 ~/work/tmp && d=$(mktemp -d ~/work/tmp/pg.XXXXXX)

PUTGD_LAB_SCRATCH names another base. Remove a directory when its run is done.
"""
import os
from pathlib import Path
import stat
import sys
import tempfile

VARIABLE = 'PUTGD_LAB_SCRATCH'


class ScratchError(RuntimeError):
    pass


def root():
    """The scratch base: $PUTGD_LAB_SCRATCH, else ~/work/tmp. Made private (0700) if missing; an
    existing base must be a directory of ours that nobody else can write to."""
    base = Path(os.environ.get(VARIABLE) or Path.home() / 'work/tmp').expanduser()
    if not base.is_absolute():
        raise ScratchError(f'{VARIABLE} must be an absolute path')
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = base.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o022:
        raise ScratchError(f'{base} is not a directory of ours that only we can write to')
    return base


def mkdtemp(prefix='pg.'):
    """A new private directory under root(), like tempfile.mkdtemp. The caller removes it."""
    return Path(tempfile.mkdtemp(prefix=prefix, dir=root()))


def TemporaryDirectory(prefix='pg.'):  # noqa: N802 - the tempfile name, on purpose
    """tempfile.TemporaryDirectory under root(): removed when the with-block ends."""
    return tempfile.TemporaryDirectory(prefix=prefix, dir=root())


def main(argv):
    if len(argv) > 2 or (len(argv) == 2 and (argv[1].startswith('-') or '/' in argv[1])):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    print(mkdtemp(*argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
