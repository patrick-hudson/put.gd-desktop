#!/bin/sh
# Run once as root inside a disposable KDE neon (Ubuntu 24.04 base) QEMU guest.
# Installs SSH and the lab key, the apt-only sudo rule the runner's --deb install uses,
# Plasma Login Manager autologin and the lab idle settings. neon ships plasmalogin, not SDDM.
set -eu

fix_autologin() {
    # Keep this in the bootstrap so the FAT seed still needs only one script.
    python3 - "$1" <<'PY'
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile

path = Path(sys.argv[1])
try:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        # No main config means the bootstrap's correctly named drop-in suffices.
        print('No plasmalogin configuration file; nothing to repair.')
        sys.exit(0)
    if not stat.S_ISREG(metadata.st_mode):
        raise ValueError('configuration must be a regular file, not a symlink')
    original = path.read_bytes()
    lines = []
    autologin = False
    for line in original.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith(b'['):
            autologin = stripped == b'[Autologin]'
        if autologin:
            line = re.sub(
                rb'^([ \t]*Session[ \t]*=[ \t]*)/usr/share/wayland-sessions/'
                rb'(plasma\.desktop)([ \t]*(?:\r?\n)?)$', rb'\1\2\3', line)
        lines.append(line)
    repaired = b''.join(lines)
    if repaired == original:
        print('No affected neon autologin entry; configuration unchanged.')
        sys.exit(0)

    # Unique backups survive retries and later installer rewrites. Stage the
    # replacement beside the config, retaining its owner, group and permissions.
    backup_fd, backup = tempfile.mkstemp(prefix=path.name + '.putgd-524-', suffix='.bak', dir=path.parent)
    with os.fdopen(backup_fd, 'wb') as output:
        output.write(original)
        output.flush()
        os.fsync(output.fileno())
    print('Original configuration saved to ' + backup, flush=True)
    staged = None
    try:
        staged_fd, staged = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
        with os.fdopen(staged_fd, 'wb') as output:
            output.write(repaired)
            output.flush()
            if (os.geteuid(), os.getegid()) != (metadata.st_uid, metadata.st_gid):
                os.fchown(output.fileno(), metadata.st_uid, metadata.st_gid)
            shutil.copystat(path, staged)
            os.fsync(output.fileno())
        def identity(info):
            # Reading may update atime; that is not a concurrent configuration edit.
            return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
                    info.st_size, info.st_mtime_ns, info.st_ctime_ns)
        if identity(path.lstat()) != identity(metadata) or path.read_bytes() != original:
            raise ValueError('configuration changed during repair; retry after other edits finish')
        os.replace(staged, path)
        staged = None
    finally:
        if staged is not None:
            os.unlink(staged)
    print('Repaired neon autologin Session=plasma.desktop. Takes effect at the next login; no session restarted.')
except (OSError, ValueError) as error:
    print('Autologin repair failed: ' + str(error), file=sys.stderr)
    sys.exit(1)
PY
}

case "${1-}" in
    --help|-h)
        echo 'Usage: bootstrap-neon-guest.sh LAB_PUBLIC_KEY'
        echo '       bootstrap-neon-guest.sh --fix-autologin [CONFIG_FILE]'
        echo 'Repair only the neon installer autologin Session path (default: /etc/plasmalogin.conf).'
        exit 0 ;;
    --fix-autologin)
        [ "$#" -le 2 ] || { echo 'Usage: bootstrap-neon-guest.sh --fix-autologin [CONFIG_FILE]' >&2; exit 1; }
        fix_autologin "${2-/etc/plasmalogin.conf}"
        exit $? ;;
esac

[ "$(id -u)" = 0 ] || { echo 'Run with sudo inside the guest.' >&2; exit 1; }
case "$(systemd-detect-virt)" in qemu|kvm) ;; *) echo 'Refusing: not a QEMU/KVM guest.' >&2; exit 1 ;; esac
[ "$#" = 1 ] || { echo 'Usage: bootstrap.sh LAB_PUBLIC_KEY' >&2; exit 1; }
key=$(cat "$1")
case "$key" in ssh-ed25519\ *) ;; *) echo 'Expected an Ed25519 public key.' >&2; exit 1 ;; esac
id lab >/dev/null
[ -f "$(dirname "$0")/guest_idle.py" ] || { echo 'Missing guest_idle.py companion.' >&2; exit 1; }
[ -f "$(dirname "$0")/package_install.py" ] || { echo 'Missing package_install.py companion.' >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y openssh-server python3
python3 "$(dirname "$0")/guest_idle.py" --install-autostart --user lab
install -d -m 700 -o lab -g lab /home/lab/.ssh
printf '%s\n' "$key" > /home/lab/.ssh/authorized_keys
chown lab:lab /home/lab/.ssh/authorized_keys
chmod 600 /home/lab/.ssh/authorized_keys
# Test package installation only; this rule is never installed on the host. neon's
# neon-settings-2 puts an apt-get wrapper in /usr/sbin, which sudo's secure_path finds first;
# it only execs /usr/bin/apt-get.
install -d -m 755 /usr/local/lib/putgd-lab
install -m 755 "$(dirname "$0")/package_install.py" /usr/local/lib/putgd-lab/package_install.py
printf 'lab ALL=(root) NOPASSWD: /usr/bin/apt-get, /usr/sbin/apt-get, /usr/local/lib/putgd-lab/package_install.py\n' > /etc/sudoers.d/putgd-lab
chmod 440 /etc/sudoers.d/putgd-lab
visudo -cf /etc/sudoers.d/putgd-lab
mkdir -p /etc/plasmalogin.conf.d
printf '[Autologin]\nUser=lab\nSession=plasma.desktop\nRelogin=false\n' > /etc/plasmalogin.conf.d/putgd-lab-autologin.conf
# neon's Calamares writes an absolute Session path into the main config, which
# overrides the drop-in. plasmalogin 6.8.1 and master cannot resolve it (#524).
fix_autologin /etc/plasmalogin.conf
systemctl enable --now ssh
echo 'Guest bootstrap complete. Verify this host key before pinning it:'
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
