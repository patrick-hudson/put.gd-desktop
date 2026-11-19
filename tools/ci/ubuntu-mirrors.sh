#!/usr/bin/env bash
# Point apt's Ubuntu sources at a ranked list of mirrors (#467, #1668).
#
#   tools/ci/ubuntu-mirrors.sh        (as root, before tools/ci/install-packages.sh)
#
# Ubuntu containers name one archive host, so an outage there fails every
# retry. Rank alternatives by bounded download throughput, including mirrors
# that answer HTTP 200 but deliver packages too slowly. APT fails over file by
# file and still verifies everything against the Ubuntu keyring, but it goes
# back to the first mirror for every file. A mirror can probe fast and then
# stall on real downloads, so install-packages.sh bounds each attempt and moves
# a failed attempt's first mirror behind the others. Sources without an Ubuntu
# archive host (Debian, Fedora, Arch) are left alone.
set -euo pipefail
apt_root=${PUTGD_APT_ROOT:-/etc/apt}
# security.ubuntu.com too: the mirrors carry the -security pocket, and that
# host has stalled together with archive.ubuntu.com.
sources=$(grep -lE 'http://(archive|security)\.ubuntu\.com/ubuntu' "$apt_root"/sources.list "$apt_root"/sources.list.d/* 2>/dev/null || true)
if [ -n "$sources" ]; then
  probe_dir=$(mktemp -d)
  # Four sequential probes: <= 12.8 s, <= 8 MiB of response bodies.
  # 1 MiB/s fetches 483 MiB in ~8 minutes, within one install attempt.
  floor=1048576
  for endpoint in ${PUTGD_UBUNTU_MIRROR_HOSTS:-us.archive.ubuntu.com mirrors.edge.kernel.org azure.archive.ubuntu.com archive.ubuntu.com}; do
    host=${endpoint%%:*}; port=${endpoint#*:}; [ "$port" != "$endpoint" ] || port=80
    start=$(date +%s%N)
    # Sample a base noble package: cached indexes can be fast while pool
    # downloads crawl. This version remains referenced by noble/main.
    # HTTP/1.0 avoids chunked encoding. Range bounds cooperative servers;
    # head also bounds servers that ignore Range. A partial body at timeout
    # is measured too, so a live trickle cannot pass as a fast mirror.
    timeout --kill-after=0.2s 3s bash -c '
      exec 3<>/dev/tcp/$1/$2 || exit 1
      printf "GET /ubuntu/pool/main/g/glibc/libc6_2.39-0ubuntu8_amd64.deb HTTP/1.0\r\nHost: %s\r\nRange: bytes=0-2097151\r\n\r\n" "$1" >&3
      IFS= read -r status <&3
      case "$status" in *" 200 "*|*" 206 "*) ;; *) exit 1;; esac
      while IFS= read -r line <&3; do [ "$line" != $'"'"'\r'"'"' ] || break; done
      head -c 2097152 <&3 > "$3"
    ' _ "$host" "$port" "$probe_dir/body" 2>/dev/null || true
    elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
    [ "$elapsed" -gt 0 ] || elapsed=1
    bytes=0
    [ ! -f "$probe_dir/body" ] || bytes=$(wc -c < "$probe_dir/body")
    speed=$(( bytes * 1000 / elapsed ))
    tier=2; label=unavailable
    if [ "$speed" -ge "$floor" ]; then tier=0; label=fast
    elif [ "$speed" -gt 0 ]; then tier=1; label=below-floor; fi
    echo "Ubuntu mirror throughput: $endpoint $speed B/s ($bytes bytes in $elapsed ms; $label; floor=$floor B/s)"
    printf '%d %d %s\n' "$tier" "$speed" "$endpoint" >> "$probe_dir/rank"
    rm -f "$probe_dir/body"
  done
  priority=0
  : > "$apt_root/putgd-mirrors.txt"
  while read -r tier speed endpoint; do
    priority=$((priority + 1))
    printf 'http://%s/ubuntu/\tpriority:%d\n' "$endpoint" "$priority" >> "$apt_root/putgd-mirrors.txt"
  done < <(LC_ALL=C sort -s -k1,1n -k2,2nr "$probe_dir/rank")
  awk '$1 < 2' "$probe_dir/rank" | wc -l > "$apt_root/putgd-mirrors.txt.usable"
  rm -f "$probe_dir/rank"
  rmdir "$probe_dir"
  sed -i -E "s#http://(archive|security)\.ubuntu\.com/ubuntu/?#mirror+file:$apt_root/putgd-mirrors.txt#" $sources
fi
