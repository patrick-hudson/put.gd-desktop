#!/usr/bin/env bash
# A small filesystem for the fault harness's ENOSPC runs (tools/lab/action_fuzz.py faults --small-fs DIR).
#   smallfs.sh mount DIR SIZE [tmpfs|ext4]   mount a size-limited tmpfs (default) or a loop-mounted ext4 image at DIR, owned by you
#   smallfs.sh umount DIR                    unmount it, and remove its image, only if this script made both
# Every mount gets a record next to it, DIR.smallfs: the mount's unique source name (tmpfs) or its image file and that file's
# device and inode (ext4). umount, and the harness before it writes anything there, check the live mount against that record,
# so a mount or an .img this script did not make is never unmounted, deleted or filled. The record and the image are created
# exclusively: an existing one stops the mount. It uses passwordless sudo (sudo -n), which the lab's builders allow for test
# setup. Nothing else on the host is touched.
set -euo pipefail
action=${1:?mount|umount}; dir=${2:?folder}
dir=$(realpath -m -- "$dir")
record="$dir.smallfs"

# The source and type of the mount exactly at DIR (empty when DIR is not a mount point).
mounted_source() { findmnt -n -o SOURCE --mountpoint "$dir" 2> /dev/null | tail -1; }
mounted_type() { findmnt -n -o FSTYPE --mountpoint "$dir" 2> /dev/null | tail -1; }
field() { sed -n "s/^$1=//p" "$record"; }
# Unmount, waiting out a target that is busy for a moment (the private session bus of a run that was just interrupted still exiting).
unmount_dir() {
  local tries=0
  until sudo -n umount -- "$dir" 2> /dev/null; do
    tries=$((tries + 1))
    if [ "$tries" -ge 60 ]; then sudo -n umount -- "$dir"; return; fi
    sleep 0.5
  done
}

case "$action" in
  mount)
    size=${3:?size, e.g. 64m}; kind=${4:-tmpfs}
    case "$kind" in tmpfs|ext4) ;; *) echo "kind must be tmpfs or ext4" >&2; exit 2 ;; esac
    mkdir -p -- "$dir"
    if mountpoint -q -- "$dir"; then echo "$dir is already a mount point" >&2; exit 1; fi
    if [ -n "$(ls -A -- "$dir")" ]; then echo "$dir is not empty" >&2; exit 1; fi
    # The record first, exclusively: a second mount at the same folder, or a leftover from a run that did not clean up, stops here.
    ( set -o noclobber; : > "$record" ) || { echo "$record exists: unmount that run first (smallfs.sh umount $dir)" >&2; exit 1; }
    chmod 0600 "$record"
    if [ "$kind" = tmpfs ]; then
      source="putgd-smallfs-$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
      printf 'kind=tmpfs\nsource=%s\n' "$source" >> "$record"
      if ! sudo -n mount -t tmpfs -o "size=$size,mode=0700,uid=$(id -u),gid=$(id -g)" "$source" "$dir"; then
        rm -f -- "$record"; echo "mounting a tmpfs at $dir failed" >&2; exit 1
      fi
    else
      image="$dir.img"
      ( set -o noclobber; : > "$image" ) || { rm -f -- "$record"; echo "$image exists: not overwriting it" >&2; exit 1; }
      identity=$(stat -c '%d:%i' -- "$image")
      printf 'kind=ext4\nimage=%s\nidentity=%s\n' "$image" "$identity" >> "$record"
      mkfs=$(PATH="$PATH:/usr/sbin:/sbin" command -v mkfs.ext4 || echo mkfs.ext4)   # in sbin, often not on a user's PATH
      if ! { truncate -s "$size" -- "$image" && "$mkfs" -q -F -m 0 "$image" && sudo -n mount -o loop,nosuid,nodev -- "$image" "$dir"; }; then
        [ "$(stat -c '%d:%i' -- "$image" 2> /dev/null)" = "$identity" ] && rm -f -- "$image"
        rm -f -- "$record"; echo "mounting an ext4 image at $dir failed" >&2; exit 1
      fi
      if ! { sudo -n chown "$(id -u):$(id -g)" "$dir" && chmod 0700 "$dir"; }; then
        sudo -n umount -- "$dir"; rm -f -- "$image" "$record"; echo "could not take ownership of $dir" >&2; exit 1
      fi
    fi
    df -h -- "$dir" | tail -1
    ;;
  umount)
    [ -f "$record" ] || { echo "$record is missing: $dir was not mounted by smallfs.sh, leaving it alone" >&2; exit 1; }
    source=$(mounted_source)
    [ -n "$source" ] || { echo "$dir is not a mount point; its record $record is left for a look" >&2; exit 1; }
    kind=$(field kind)
    if [ "$kind" = tmpfs ]; then
      if [ "$(mounted_type)" != tmpfs ] || [ "$source" != "$(field source)" ]; then
        echo "the mount at $dir ($source) is not the one smallfs.sh made ($(field source)): leaving it alone" >&2; exit 1
      fi
      unmount_dir
    elif [ "$kind" = ext4 ]; then
      image=$(field image); identity=$(field identity)
      loop=${source#/dev/}
      backing=$(cat "/sys/block/$loop/loop/backing_file" 2> /dev/null || true)
      if [ "$(mounted_type)" != ext4 ] || [ "$backing" != "$image" ] || [ "$(stat -c '%d:%i' -- "$image" 2> /dev/null)" != "$identity" ]; then
        echo "the mount at $dir ($source on '$backing') is not the image smallfs.sh made ($image): leaving it alone" >&2; exit 1
      fi
      unmount_dir
      [ "$(stat -c '%d:%i' -- "$image" 2> /dev/null)" = "$identity" ] && rm -f -- "$image"
    else
      echo "$record does not name a kind: leaving $dir alone" >&2; exit 1
    fi
    if mountpoint -q -- "$dir"; then echo "$dir is still a mount point after umount" >&2; exit 1; fi
    rm -f -- "$record"
    ;;
  *) echo "usage: $0 mount DIR SIZE [tmpfs|ext4] | umount DIR" >&2; exit 2 ;;
esac
