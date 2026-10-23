#!/bin/sh
# Run once as root inside a disposable KDE neon (Ubuntu 24.04 base) QEMU guest.
# Installs SSH and the lab key, the apt-only sudo rule the runner's --deb install uses,
# Plasma Login Manager autologin and the lab idle settings. neon ships plasmalogin, not SDDM.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run with sudo inside the guest.' >&2; exit 1; }
case "$(systemd-detect-virt)" in qemu|kvm) ;; *) echo 'Refusing: not a QEMU/KVM guest.' >&2; exit 1 ;; esac
[ "$#" = 1 ] || { echo 'Usage: bootstrap.sh LAB_PUBLIC_KEY' >&2; exit 1; }
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
# Test package installation only; this rule is never installed on the host. neon's
# neon-settings-2 puts an apt-get wrapper in /usr/sbin, which sudo's secure_path finds first;
# it only execs /usr/bin/apt-get.
printf 'lab ALL=(root) NOPASSWD: /usr/bin/apt-get, /usr/sbin/apt-get\n' > /etc/sudoers.d/putgd-lab
chmod 440 /etc/sudoers.d/putgd-lab
visudo -cf /etc/sudoers.d/putgd-lab
mkdir -p /etc/plasmalogin.conf.d
printf '[Autologin]\nUser=lab\nSession=plasma\nRelogin=false\n' > /etc/plasmalogin.conf.d/putgd-lab-autologin.conf
# Lab guests must stay visible on the display wall: no idle lock, dimming, DPMS or suspend.
su - lab -c 'kwriteconfig6 --file kscreenlockerrc --group Daemon --key Autolock false; kwriteconfig6 --file kscreenlockerrc --group Daemon --key LockOnResume false; for g in AC Battery LowBattery; do kwriteconfig6 --file powerdevilrc --group $g --group Display --key DimDisplayWhenIdle false; kwriteconfig6 --file powerdevilrc --group $g --group Display --key TurnOffDisplayWhenIdle false; kwriteconfig6 --file powerdevilrc --group $g --group SuspendAndShutdown --key AutoSuspendAction 0; done'
systemctl enable --now ssh
echo 'Guest bootstrap complete. Verify this host key before pinning it:'
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
