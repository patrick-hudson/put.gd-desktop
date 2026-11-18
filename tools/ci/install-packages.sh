#!/usr/bin/env bash
# Install distro packages with dnf or apt-get, surviving mirror trouble (#368).
#
#   install-packages.sh [--no-install-recommends] [PACKAGE...]
#
# Without packages it only refreshes the package metadata. Each failed online
# attempt is retried with freshly downloaded metadata after a doubling back-off,
# so a later attempt sees a settled mirror sync or other mirrors:
#   PUTGD_PKG_ATTEMPTS  online attempts (default 4)
#   PUTGD_PKG_BACKOFF   seconds before the first retry (default 30)
#   PUTGD_PKG_CACHE     optional snapshot directory, restored and saved by
#                       actions/cache. Online attempts work in PUTGD_PKG_CACHE.live,
#                       seeded from the snapshot, so cached packages are not
#                       downloaded again. After an online success the snapshot is
#                       replaced by that metadata and exactly the installed
#                       packages. When every online attempt fails, one more attempt
#                       installs offline from the snapshot's metadata and packages.
#                       On apt the snapshot also keeps the sources it was built with:
#                       apt names its metadata files after the source URI, so the
#                       offline attempt reads them through those sources, not the
#                       host's, and a snapshot made in a container serves a runner.
#   PUTGD_PKG_ATTEMPTS=0 skips the online attempts and installs from the snapshot.
#   PUTGD_PKG_REUSE_METADATA=true (apt) never refreshes the indexes: every attempt uses the
#                       ones an earlier install of the same job downloaded, and fails when
#                       there are none or a package can't be downloaded against them. Adding
#                       packages to a snapshot this way keeps its indexes matching its .debs.
# With --download-only (adding packages to a snapshot) there is no offline fallback.
#   PUTGD_PKG_TIMEOUT   seconds one attempt may take (default 900)
#   PUTGD_PKG_STALL     seconds an attempt may print nothing (default 600)
# A stalled mirror connection can hang apt or dnf for good (#467): an attempt
# over either limit is stopped and counts as failed. On apt that covers the update and
# the download (`apt-get install --download-only`) only; the install of what was downloaded
# (`--no-download`) and the offline install have no limit, so dpkg is never stopped halfway.
# Downloaded packages stay in the cache, so the next attempt resumes. apt and dnf drop a connection that
# moves nothing (apt) or under 1 kB/s (dnf) for 60 s and retry it themselves.
# On apt, a failed attempt moves the first mirror of $PUTGD_APT_ROOT/putgd-mirrors.txt (the
# PUTGD_UBUNTU_MIRRORS probe's ranked list) behind the other usable ones, so the next attempt
# starts on another mirror.
#   PUTGD_APT_LAST_RESORT_MIRRORS  (apt) URL of a list of mirrors, one URI a line, such as
#                       http://mirrors.ubuntu.com/US.txt. Once the online attempts have failed,
#                       the fastest few of them replace putgd-mirrors.txt (#1668). The snapshot
#                       install then downloads the packages its indexes name but it lacks through
#                       them, and with no snapshot, or one that can't serve the list, one more
#                       online attempt runs through them with fresh metadata:
#   PUTGD_PKG_LAST_RESORT_TIMEOUT  seconds that attempt may take (default 1800)
# Every install is one apt transaction against one set of indexes: entirely online, or the
# snapshot's own indexes with its packages and the ones it lacks, never a mix. apt checks every
# file against the signed indexes, whichever mirror served it.
# Every failed or stopped attempt and fallback is a warning annotation and a row in
# $GITHUB_STEP_SUMMARY. The script fails when the snapshot cannot satisfy the
# install either, and at once when a package name does not exist.
set -euo pipefail

attempts=${PUTGD_PKG_ATTEMPTS:-4}
delay=${PUTGD_PKG_BACKOFF:-30}
limit=${PUTGD_PKG_TIMEOUT:-900}
stall=${PUTGD_PKG_STALL:-600}
snapshot=${PUTGD_PKG_CACHE:-}
live=${snapshot:+$snapshot.live}
options=() packages=()
for arg in "$@"; do
  case "$arg" in -*) options+=("$arg") ;; *) packages+=("$arg") ;; esac
done
if command -v dnf >/dev/null; then manager=dnf
elif command -v apt-get >/dev/null; then manager=apt-get
else echo 'install-packages.sh: neither dnf nor apt-get is available' >&2; exit 1
fi
. /etc/os-release
label="${PUTGD_PKG_LABEL:-$ID $VERSION_ID}, $manager"
log=$(mktemp)
trap 'rm -f "$log" "$log.stopped" "$log.lacking"' EXIT
apt_root=${PUTGD_APT_ROOT:-/etc/apt}
mirror_list=$apt_root/putgd-mirrors.txt
apt_network=(-o Acquire::Retries=3 -o Acquire::http::Timeout=60 -o Acquire::https::Timeout=60)
dnf_network=(--setopt=retries=10 --setopt=timeout=60 --setopt=minrate=1000)

# dnf/apt-get options that keep metadata and packages in DIR.
cache_options() {
  if [ "$manager" = dnf ]; then
    printf '%s\n' --setopt=keepcache=True "--setopt=cachedir=$1" "--setopt=system_cachedir=$1"
  else
    mkdir -p "$1/lists/partial" "$1/archives/partial"
    printf '%s\n' -o "Dir::State::Lists=$1/lists/" -o "Dir::Cache::Archives=$1/archives/"
  fi
}

# bounded COMMAND...: run COMMAND until the attempt's deadline, or until it has
# printed nothing for $stall seconds. Why it was stopped goes to $log.stopped.
bounded() {
  local left=$((deadline - SECONDS)) out pid line status
  [ "$left" -gt 0 ] || left=1
  exec {out}< <(exec timeout --kill-after=30s "$left" "$@" 2>&1)
  pid=$!
  while :; do
    IFS= read -r -t "$stall" -u "$out" line && { printf '%s\n' "$line"; continue; }
    status=$?
    break
  done
  [ -z "$line" ] || printf '%s\n' "$line"
  if [ "$status" -gt 128 ]; then
    echo "printed nothing for $stall s" > "$log.stopped"
    kill -TERM "$pid" 2>/dev/null || true
  fi
  exec {out}<&-
  status=0
  wait "$pid" || status=$?
  # timeout(1) exits 124, or 137 when the command needed the KILL.
  if [ ! -e "$log.stopped" ] && { [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; }; then
    echo "ran over the $limit s attempt limit" > "$log.stopped"
  fi
  return "$status"
}

# One online attempt; $1 is the attempt number.
online() {
  local extra=()
  [ -z "$live" ] || mapfile -t extra < <(cache_options "$live")
  if [ "$manager" = dnf ]; then
    [ "$1" -eq 1 ] || extra+=(--refresh)
    if [ ${#packages[@]} -eq 0 ]; then
      bounded dnf makecache "${dnf_network[@]}" "${extra[@]}"
    else
      bounded dnf install -y "${dnf_network[@]}" "${extra[@]}" "${options[@]}" "${packages[@]}"
    fi
  else
    # A stopped attempt can leave dpkg half done; apt-get refuses to install until it is finished.
    [ -z "$interrupted" ] || bounded dpkg --configure -a || return
    if [ -z "${PUTGD_PKG_REUSE_METADATA:-}" ]; then
      bounded apt-get update --error-on=any "${apt_network[@]}" "${extra[@]}" || return
    elif ! compgen -G "$live/lists/*Packages*" >/dev/null; then
      echo 'E: no package indexes of an earlier install to reuse'
      return 1
    fi
    [ ${#packages[@]} -gt 0 ] || return 0
    # Two phases, so the time limit can never stop dpkg halfway: a stopped download leaves the system
    # untouched and the snapshot fallback starts from the state it was in, while a started install runs to the end.
    DEBIAN_FRONTEND=noninteractive bounded apt-get install -y --download-only "${apt_network[@]}" \
      "${extra[@]}" "${options[@]}" "${packages[@]}" || return
    [[ " ${options[*]} " == *" --download-only "* ]] ||
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-download "${extra[@]}" "${options[@]}" "${packages[@]}"
  fi
}

# Keep the apt sources beside the metadata that was downloaded through them.
save_sources() {
  local file list=$apt_root/sources.list parts=$apt_root/sources.list.d
  # The sources apt really reads, so a custom APT_CONFIG is followed (PUTGD_APT_ROOT overrides, for tests).
  if [ -z "${PUTGD_APT_ROOT:-}" ] && command -v apt-config >/dev/null; then
    eval "$(apt-config shell list Dir::Etc::sourcelist/f parts Dir::Etc::sourceparts/d)"
    parts=${parts%/}
  fi
  mkdir -p "$1/sources"
  rm -f "$1"/sources/*
  # apt picks the parser by the file name's extension: keep .sources for a deb822 main file.
  if [ -s "$list" ]; then
    case "$list" in *.sources) cp "$list" "$1/sources/00-sources-list.sources" ;; *) cp "$list" "$1/sources/00-sources-list.list" ;; esac
  fi
  for file in "$parts"/*.list "$parts"/*.sources; do
    [ ! -f "$file" ] || cp "$file" "$1/sources/"
  done
  # No sources found: save none, so the offline install keeps the host's own instead of an empty set.
  rmdir "$1/sources" 2>/dev/null || true
}

# Install from the snapshot. On apt, packages its indexes name but it has no archive of (a snapshot
# restored from an older package list) are downloaded first, against those same indexes, so the
# whole transaction comes from one index generation. Returns 2 when it failed before dpkg started,
# so a fresh online transaction can still run.
offline() {
  local extra=() uris lacking
  mapfile -t extra < <(cache_options "$snapshot")
  if [ "$manager" = apt-get ] && [ -d "$snapshot/sources" ]; then
    extra+=(-o Dir::Etc::sourcelist=/dev/null -o "Dir::Etc::sourceparts=$snapshot/sources")
  fi
  if [ "$manager" = dnf ]; then
    bounded dnf install -y --cacheonly "${extra[@]}" "${options[@]}" "${packages[@]}"
  else
    [ -z "$interrupted" ] || dpkg --configure -a || return
    # The archives the transaction needs that the snapshot lacks; it fails here, before anything is
    # installed, when its indexes can't satisfy the list at all.
    uris=$(DEBIAN_FRONTEND=noninteractive apt-get install -y --print-uris -qq "${extra[@]}" "${options[@]}" "${packages[@]}") ||
      { printf '%s\n' "$uris"; return 2; }
    if [ -n "$uris" ]; then
      lacking=$(cut -d' ' -f2 <<< "$uris" | tr '\n' ' ')
      echo "$(wc -l <<< "$uris") $lacking" > "$log.lacking"
      echo "The package snapshot lacks $(wc -l <<< "$uris") archives; downloading them against its own indexes: $lacking"
      use_last_resort_mirrors || true
      DEBIAN_FRONTEND=noninteractive bounded apt-get install -y --download-only "${apt_network[@]}" \
        "${extra[@]}" "${options[@]}" "${packages[@]}" ||
        { echo "E: could not download the archives the package snapshot lacks: $lacking"; return 2; }
    fi
    # Nothing is downloaded, so no time limit that could stop dpkg halfway.
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-download "${extra[@]}" "${options[@]}" "${packages[@]}"
  fi
}

# Move the first mirror behind the other usable ones (the rule of putgd_demote_mirror in the workflows).
demote_mirror() {
  local usable first
  [ "$manager" = apt-get ] && [ -s "$mirror_list" ] || return 0
  usable=$(cat "$mirror_list.usable" 2>/dev/null || true)
  [ "${usable:-0}" -gt 0 ] || usable=$(wc -l < "$mirror_list")
  [ "$usable" -gt 1 ] || return 0
  first=$(head -n 1 "$mirror_list" | cut -f1)
  cut -f1 "$mirror_list" | awk -v n="$usable" 'NR == 1 { first = $0; next } { print } NR == n { print first }' |
    awk '{ printf "%s\tpriority:%d\n", $0, NR }' > "$mirror_list.new"
  mv "$mirror_list.new" "$mirror_list"
  echo "Ubuntu mirror moved behind the other usable mirrors: $first"
}

# Replace putgd-mirrors.txt with the fastest 8 of the first 60 mirrors of $PUTGD_APT_LAST_RESORT_MIRRORS,
# once. Each is timed on 256 KB of this release's package index for this architecture, so a mirror without
# it (a ports-only mirror) is left out. Fails when there is no such list or no mirror answers.
use_last_resort_mirrors() {
  local arch probe ranked
  arch=$(dpkg --print-architecture 2>/dev/null || true)
  probe=dists/$VERSION_CODENAME/main/binary-${arch:-amd64}/Packages.gz
  [ "$manager" = apt-get ] && [ -n "${PUTGD_APT_LAST_RESORT_MIRRORS:-}" ] || return 1
  [ ! -e "$mirror_list.last-resort" ] || return 0
  # Only plain mirror URIs: the list comes from the network.
  ranked=$(curl -fsS --max-time 60 "$PUTGD_APT_LAST_RESORT_MIRRORS" | tr -d '\r' |
    grep -E '^https?://[A-Za-z0-9.-]+(:[0-9]+)?/[A-Za-z0-9._~/-]*/$' | head -n 60 |
    xargs -r -P 16 -n 1 bash -c 'speed=$(curl -fsS -o /dev/null --max-time 5 -r 0-262143 -w "%{speed_download}" "$2$1") && echo "${speed%.*} $2"' _ "$probe" |
    sort -rn | head -n 8 | cut -d' ' -f2) || true
  if [ -z "$ranked" ]; then
    echo "E: no mirror of $PUTGD_APT_LAST_RESORT_MIRRORS answered"
    return 1
  fi
  mkdir -p "$apt_root"
  awk '{ printf "%s\tpriority:%d\n", $0, NR }' <<< "$ranked" > "$mirror_list"
  rm -f "$mirror_list.usable"
  echo "$PUTGD_APT_LAST_RESORT_MIRRORS" > "$mirror_list.last-resort"
  echo "Last-resort mirrors from $PUTGD_APT_LAST_RESORT_MIRRORS, fastest first: $(tr '\n' ' ' <<< "$ranked")"
}

# Replace the snapshot with the live metadata and the installed packages only.
refresh_snapshot() {
  if [ "$manager" = dnf ]; then
    find "$live" -path '*/packages/*.rpm' -printf '%f\n' | sort > "$log.cached"
    rpm -qa --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}.rpm\n' | sort > "$log.installed"
    comm -23 "$log.cached" "$log.installed" | while read -r stale; do
      find "$live" -path "*/packages/$stale" -delete
    done
    rm -f "$log.cached" "$log.installed"
  else
    local extra=()
    mapfile -t extra < <(cache_options "$live")
    apt-get autoclean "${extra[@]}" >/dev/null
    save_sources "$live"
  fi
  rm -rf "$snapshot.new"
  # Hard links: nothing writes to the live cache after this.
  cp -al "$live" "$snapshot.new"
  echo "$(date -u +%FT%RZ)${GITHUB_RUN_ID:+ by $GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID}" \
    > "$snapshot.new/putgd-snapshot"
  rm -rf "$snapshot"
  mv "$snapshot.new" "$snapshot"
  [ -z "${GITHUB_ENV:-}" ] || echo 'PUTGD_PKG_CACHE_REFRESHED=true' >> "$GITHUB_ENV"
}

reported=
# report LEVEL ATTEMPT OUTCOME [DETAIL]
report() {
  local detail=${4:-}
  echo "::$1::Package install ($label): attempt $2: $3${detail:+: $detail}"
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
  if [ -z "$reported" ]; then
    printf '### Package install retries (%s)\n\n| Attempt | Outcome | Detail |\n| --- | --- | --- |\n' \
      "$label" >> "$GITHUB_STEP_SUMMARY"
    reported=1
  fi
  detail=${detail//|/\\|}
  printf '| %s | %s | %s |\n' "$2" "$3" "${detail:+\`${detail//\`/\'}\`}" >> "$GITHUB_STEP_SUMMARY"
}

# The most specific error line; apt's closing "Some index files failed" names no cause.
last_error() {
  grep -aE 'Error|error:|^E: |Failed|Cannot download|Cache-only|Curl error|All mirrors' "$log" | grep -av 'Some index files failed' |
    tail -n 1 | cut -c1-300 || true
}

# Why the attempt failed: the reason it was stopped and its last output line, or its last error.
failure() {
  if [ -e "$log.stopped" ]; then
    local last
    last=$(grep -av '^[[:space:]]*$' "$log" | tail -n 1 | tr -d '\r' | cut -c1-200 || true)
    printf '%s%s' "$(cat "$log.stopped")" "${last:+ after: $last}"
  else
    last_error
  fi
}

if [ -n "$snapshot" ] && [ -d "$snapshot" ] && [ ! -e "$live" ]; then
  cp -a "$snapshot" "$live"
  rm -f "$live/putgd-snapshot"
fi

interrupted=
for ((attempt = 1; attempt <= attempts; attempt++)); do
  rm -f "$log.stopped"
  deadline=$((SECONDS + limit))
  if online "$attempt" 2>&1 | tee "$log"; then
    [ "$attempt" -eq 1 ] || report notice "$attempt of $attempts" '**passed on retry**'
    [ -z "$snapshot" ] || [ ${#packages[@]} -eq 0 ] || refresh_snapshot
    exit 0
  fi
  error=$(failure)
  outcome=failed
  interrupted=
  if [ -e "$log.stopped" ]; then outcome='**stopped**'; interrupted=1; fi
  if missing=$(grep -aE 'No match for argument|Unable to locate package' "$log"); then
    report error "$attempt of $attempts" '**failed**, not retried: a package does not exist' "${missing%%$'\n'*}"
    exit 1
  fi
  demote_mirror
  if [ "$attempt" -lt "$attempts" ]; then
    report warning "$attempt of $attempts" "$outcome, retrying in $delay s with refreshed metadata" "$error"
    sleep "$delay"
    delay=$((delay * 2))
  else
    report warning "$attempt of $attempts" "$outcome" "$error"
  fi
done

if [ ${#packages[@]} -eq 0 ]; then
  report error '-' '**failed**: package metadata could not be refreshed'
  exit 1
elif [[ " ${options[*]} " == *" --download-only "* ]]; then
  report error '-' '**failed**: the packages could not be downloaded into the snapshot'
  exit 1
fi
if [ -n "$snapshot" ] && [ -f "$snapshot/putgd-snapshot" ]; then
  saved=$(cat "$snapshot/putgd-snapshot")
  rm -f "$log.stopped" "$log.lacking"
  deadline=$((SECONDS + limit))
  status=0
  offline 2>&1 | tee "$log" || status=$?
  if [ "$status" -eq 0 ]; then
    if [ -e "$log.lacking" ]; then
      read -r count lacking < "$log.lacking"
      report warning snapshot "**installed offline from the package snapshot** saved $saved; archives it lacked, downloaded against its indexes: $count" "$lacking"
    else
      report warning snapshot "**installed offline from the package snapshot** saved $saved"
    fi
    exit 0
  fi
  report error snapshot '**failed**' "$(failure)"
  # dpkg started: never run a second transaction over a half-done one.
  [ "$status" -eq 2 ] || exit 1
else
  report error snapshot '**failed**: no package snapshot to install from'
fi
if [ "$manager" = apt-get ] && [ -n "${PUTGD_APT_LAST_RESORT_MIRRORS:-}" ]; then
  rm -f "$log.stopped"
  limit=${PUTGD_PKG_LAST_RESORT_TIMEOUT:-1800}
  deadline=$((SECONDS + limit))
  { use_last_resort_mirrors && online last-resort; } 2>&1 | tee "$log" && {
    report warning last-resort "**installed online through the last-resort mirrors** from $PUTGD_APT_LAST_RESORT_MIRRORS"
    exit 0
  }
  report error last-resort '**failed**' "$(failure)"
fi
exit 1
