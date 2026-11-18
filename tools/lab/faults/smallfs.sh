#!/usr/bin/env bash
# A small filesystem for the fault harness's ENOSPC runs (tools/lab/action_fuzz.py faults --small-fs DIR).
#   smallfs.sh mount DIR SIZE [tmpfs|ext4]   mount a size-limited tmpfs (default) or a loop-mounted ext4 image at DIR, owned by you
#   smallfs.sh umount DIR                    unmount it (a loop image is removed too: its file is DIR.img)
# It uses passwordless sudo, which the lab's builders allow for test setup. Nothing else on the host is touched.
set -euo pipefail
action=${1:?mount|umount}; dir=${2:?folder}
case "$action" in
  mount)
    size=${3:?size, e.g. 64m}; kind=${4:-tmpfs}
    mkdir -p "$dir"
    if mountpoint -q "$dir"; then echo "$dir is already a mount point" >&2; exit 1; fi
    if [ "$kind" = tmpfs ]; then
      sudo -n mount -t tmpfs -o "size=$size,mode=0700,uid=$(id -u),gid=$(id -g)" putgd-smallfs "$dir"
    else
      truncate -s "$size" "$dir.img"
      mkfs.ext4 -q -F -m 0 "$dir.img"
      sudo -n mount -o loop "$dir.img" "$dir"
      sudo -n chown "$(id -u):$(id -g)" "$dir"; chmod 0700 "$dir"
    fi
    df -h "$dir" | tail -1
    ;;
  umount)
    mountpoint -q "$dir" || { echo "$dir is not a mount point" >&2; exit 1; }
    sudo -n umount "$dir"
    [ -f "$dir.img" ] && rm -f -- "$dir.img"
    ;;
  *) echo "usage: $0 mount DIR SIZE [tmpfs|ext4] | umount DIR" >&2; exit 2 ;;
esac
