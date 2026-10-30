#!/bin/sh
# Run once as root inside a disposable graphical QEMU guest. No host sudo needed.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run with sudo inside the guest.' >&2; exit 1; }
case "$(systemd-detect-virt)" in
  qemu|kvm) ;; # Accelerated QEMU reports kvm; software emulation reports qemu.
  *) echo 'Refusing to provision a non-QEMU/KVM system.' >&2; exit 1 ;;
esac
[ "$#" = 1 ] || { echo 'Usage: bootstrap-guest.sh LAB_PUBLIC_KEY' >&2; exit 1; }
key=$(cat "$1")
case "$key" in ssh-ed25519\ *) ;; *) echo 'Expected an Ed25519 public key.' >&2; exit 1 ;; esac
id lab >/dev/null
[ -f "$(dirname "$0")/guest_idle.py" ] || { echo 'Missing guest_idle.py companion.' >&2; exit 1; }
[ -f "$(dirname "$0")/package_install.py" ] || { echo 'Missing package_install.py companion.' >&2; exit 1; }
[ -f "$(dirname "$0")/package_scope.py" ] || { echo 'Missing package_scope.py companion.' >&2; exit 1; }
if command -v apt-get >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y openssh-server python3 util-linux
  package_manager=/usr/bin/apt-get
  ssh_service=ssh
elif command -v dnf >/dev/null; then
  dnf install -y openssh-server python3 util-linux
  package_manager=/usr/bin/dnf
  ssh_service=sshd
else
  echo 'Guest requires apt-get or dnf.' >&2
  exit 1
fi
python3 "$(dirname "$0")/guest_idle.py" --install-autostart --user lab
install -d -m 700 -o lab -g lab /home/lab/.ssh
printf '%s\n' "$key" > /home/lab/.ssh/authorized_keys
chown lab:lab /home/lab/.ssh/authorized_keys
chmod 600 /home/lab/.ssh/authorized_keys
if command -v restorecon >/dev/null; then
  restorecon -RF /home/lab/.ssh
fi
# Test package installation and its system scopes only; never installed on the host.
install -d -m 755 /usr/local/lib/putgd-lab
install -m 755 "$(dirname "$0")/package_install.py" /usr/local/lib/putgd-lab/package_install.py
# The scope launcher fixes every systemd-run argument; never grant systemd-run itself.
install -m 755 "$(dirname "$0")/package_scope.py" /usr/local/lib/putgd-lab/package_scope.py
printf 'lab ALL=(root) NOPASSWD: %s, /usr/local/lib/putgd-lab/package_install.py, /usr/local/lib/putgd-lab/package_scope.py\n' "$package_manager" > /etc/sudoers.d/putgd-lab
chmod 440 /etc/sudoers.d/putgd-lab
visudo -cf /etc/sudoers.d/putgd-lab
systemctl enable --now "$ssh_service"
if systemctl is-active --quiet firewalld; then
  firewall-cmd --permanent --add-service=ssh
  firewall-cmd --reload
fi
echo 'Guest bootstrap complete. Verify this host key before pinning it:'
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
