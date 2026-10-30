#!/usr/bin/env python3
"""Stage the GNOME witness from the QEMU host into an authenticated overlay."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.lab import lab


def stage(run, source):
    # Reuse the exact disk/process/forwarded-SSH/QMP checks of lab preflight.
    # A guest-side flag or a DMI string cannot authenticate an owned overlay.
    lab.verify_owned_guest(run)
    profile = lab.load_profile(lab.state(run)['profile'])
    if profile['session_process'] != 'gnome-shell':
        raise lab.LabError('GNOME witness requires a disposable GNOME overlay')
    files = {name: (Path(source) / name).read_text() for name in ('extension.js','metadata.json')}
    script = "python3 - <<'SD_STAGE'\nfrom pathlib import Path\n"
    script += "target=Path.home()/'.local/share/gnome-shell/extensions/putgd-lab-witness@put.gd'\n"
    script += "target.mkdir(parents=True,exist_ok=True)\n"
    script += 'for name,content in ' + repr(files) + '.items():\n (target/name).write_text(content)\nSD_STAGE\n'
    script += 'gnome-extensions enable putgd-lab-witness@put.gd 2>/dev/null || true\n'
    script += 'gnome-extensions info putgd-lab-witness@put.gd 2>/dev/null || true\n'
    # Check again at the actual mutation boundary, after local input reads.
    lab.verify_owned_guest(run)
    return lab.guest_session(run, script)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', help='Owned running lab.py overlay directory on this host')
    parser.add_argument('source', nargs='?', default=str(Path(__file__).with_name('gnome-witness')))
    args = parser.parse_args()
    try:
        print(stage(args.run, args.source))
    except (lab.LabError, OSError, ValueError) as error:
        parser.exit(2, 'GNOME witness staging refused: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
