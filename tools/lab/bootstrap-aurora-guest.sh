#!/bin/sh
# Run once as root inside a disposable Aurora (Universal Blue, Fedora Atomic KDE) QEMU guest.
# Installs the lab SSH key, a narrow sudo rule, enables sshd and Plasma Login Manager autologin.
# Aurora is image-based: no package manager step, and the display manager is plasmalogin, not SDDM.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run with sudo inside the guest.' >&2; exit 1; }
case "$(systemd-detect-virt)" in qemu|kvm) ;; *) echo 'Refusing: not a QEMU/KVM guest.' >&2; exit 1 ;; esac
[ "$#" = 1 ] || { echo 'Usage: bootstrap.sh LAB_PUBLIC_KEY' >&2; exit 1; }
key=$(cat "$1")
case "$key" in ssh-ed25519\ *) ;; *) echo 'Expected an Ed25519 public key.' >&2; exit 1 ;; esac
id lab >/dev/null
install -d -m 700 -o lab -g lab /home/lab/.ssh
printf '%s\n' "$key" > /home/lab/.ssh/authorized_keys
chown lab:lab /home/lab/.ssh/authorized_keys
chmod 600 /home/lab/.ssh/authorized_keys
command -v restorecon >/dev/null && restorecon -RF /home/lab/.ssh
printf 'lab ALL=(root) NOPASSWD: /usr/bin/rpm-ostree, /usr/bin/bootc, /usr/bin/flatpak, /usr/bin/systemctl, /usr/bin/loginctl, /usr/bin/journalctl\n' > /etc/sudoers.d/putgd-lab
chmod 440 /etc/sudoers.d/putgd-lab
visudo -cf /etc/sudoers.d/putgd-lab
mkdir -p /etc/plasmalogin.conf.d
printf '[Autologin]\nUser=lab\nSession=plasma\nRelogin=false\n' > /etc/plasmalogin.conf.d/putgd-lab-autologin.conf
# Lab guests must stay visible on the display wall: no idle lock, dimming, DPMS or suspend.
su - lab -c 'kwriteconfig6 --file kscreenlockerrc --group Daemon --key Autolock false; kwriteconfig6 --file kscreenlockerrc --group Daemon --key LockOnResume false; for g in AC Battery LowBattery; do kwriteconfig6 --file powerdevilrc --group $g --group Display --key DimDisplayWhenIdle false; kwriteconfig6 --file powerdevilrc --group $g --group Display --key TurnOffDisplayWhenIdle false; kwriteconfig6 --file powerdevilrc --group $g --group SuspendAndShutdown --key AutoSuspendAction 0; done'
systemctl enable --now sshd
if systemctl is-active --quiet firewalld; then firewall-cmd --permanent --add-service=ssh; firewall-cmd --reload; fi
echo 'Guest bootstrap complete. Verify this host key before pinning it:'
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
