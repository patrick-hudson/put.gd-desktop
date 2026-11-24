#!/bin/sh
# Starts one sshd of the lab: "sftp" (chrooted SFTP only) or "nosftp" (shell and scp, no SFTP
# subsystem). Everything it needs is under /server, mounted read-only from the state folder.
set -eu
mode=$1
for entry in putgd-key:1001 putgd-pw:1002; do
    name=${entry%%:*}
    adduser -D -u "${entry##*:}" -s /bin/ash "$name" 2>/dev/null || true
done
# The key user has no usable password; the password user's comes from a file, never a command line.
printf 'putgd-key:%s\n' "$(head -c 32 /dev/urandom | base64 | tr -d '\n')" | chpasswd -c sha512
printf 'putgd-pw:%s\n' "$(cat /run/putgd-pw.password)" | chpasswd -c sha512
if [ "$mode" = sftp ]; then
    # A chroot root must be root-owned and not writable; the three folders are inside it.
    mkdir -p /srv/sftp/upload /srv/sftp/ro /srv/sftp/full
    chown root:root /srv/sftp /srv/sftp/ro
    chmod 0755 /srv/sftp /srv/sftp/ro
    chmod 0777 /srv/sftp/upload
    [ -f /srv/sftp/ro/readme.txt ] || echo 'This folder is read-only for every lab user.' > /srv/sftp/ro/readme.txt
    # "full" is a 1 MB tmpfs; fill it so any write fails with no space left.
    chmod 1777 /srv/sftp/full
    rm -f /srv/sftp/full/.filler
    dd if=/dev/zero of=/srv/sftp/full/.filler bs=4k 2>/dev/null || true
fi
exec /usr/sbin/sshd -D -e -f "/server/sshd_config.$mode"
