"""Narrow Linux mount discovery/cleanup for the private bounded reproducer."""
import json
import os
import subprocess

SOURCE = 'i254-disposable'

def inspect_mount(mount):
    # --mountpoint demands an exact mount, unlike --target's parent fallback.
    result = subprocess.run(['findmnt', '--json', '--mountpoint', str(mount),
                             '--output', 'TARGET,SOURCE,FSTYPE'],
                            capture_output=True, text=True, timeout=10)
    if result.returncode == 1 and not result.stdout.strip():
        return None
    if result.returncode:
        raise RuntimeError('mount discovery failed: ' + result.stderr)
    entries = json.loads(result.stdout)['filesystems']
    if len(entries) != 1:
        raise RuntimeError('ambiguous mount discovery')
    current = entries[0]
    current['device'] = os.stat(mount).st_dev
    return current

def cleanup_mount(mount, acquisition_attempted, known_device=None):
    """Discover even when acquisition raised before its success flag was set.

    Never unlink through the mountpoint. Unmount only this run's exact expected
    mount after an acquisition attempt; reject a foreign/replaced mount. The
    caller has already stopped owned writers and defers repeat terminal signals.
    """
    receipt = {'acquisitionAttempted': acquisition_attempted,
               'knownDevice': known_device, 'observed': None,
               'unmountExit': None, 'errors': []}
    try:
        current = inspect_mount(mount)
        receipt['observed'] = current
        if current is not None:
            owned = (acquisition_attempted and current['target'] == str(mount)
                     and current['source'] == SOURCE and current['fstype'] == 'tmpfs'
                     and (known_device is None or current['device'] == known_device))
            if not owned:
                raise RuntimeError('refusing cleanup of unowned or replaced mount')
            result = subprocess.run(['sudo', '-n', 'umount', str(mount)],
                                    capture_output=True, text=True, timeout=15)
            receipt['unmountExit'] = result.returncode
            if result.returncode:
                raise RuntimeError('unmount failed: ' + result.stderr)
    except Exception as error:
        receipt['errors'].append(str(error))
    receipt['mountRemaining'] = os.path.ismount(mount)
    return receipt
