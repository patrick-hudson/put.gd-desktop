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
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y openssh-server
install -d -m 700 -o lab -g lab /home/lab/.ssh
printf '%s\n' "$key" > /home/lab/.ssh/authorized_keys
chown lab:lab /home/lab/.ssh/authorized_keys
chmod 600 /home/lab/.ssh/authorized_keys
# Test package installation only; this rule is never installed on the host.
printf '%s\n' 'lab ALL=(root) NOPASSWD: /usr/bin/apt-get' > /etc/sudoers.d/putgd-lab
chmod 440 /etc/sudoers.d/putgd-lab
visudo -cf /etc/sudoers.d/putgd-lab
systemctl enable --now ssh
echo 'Guest bootstrap complete. Verify this host key before pinning it:'
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
