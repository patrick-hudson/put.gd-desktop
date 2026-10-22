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
# Every failed attempt and fallback is a warning annotation and a row in
# $GITHUB_STEP_SUMMARY. The script fails when the snapshot cannot satisfy the
# install either, and at once when a package name does not exist.
set -euo pipefail

attempts=${PUTGD_PKG_ATTEMPTS:-4}
delay=${PUTGD_PKG_BACKOFF:-30}
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
trap 'rm -f "$log"' EXIT

# dnf/apt-get options that keep metadata and packages in DIR.
cache_options() {
  if [ "$manager" = dnf ]; then
    printf '%s\n' --setopt=keepcache=True "--setopt=cachedir=$1" "--setopt=system_cachedir=$1"
  else
    mkdir -p "$1/lists/partial" "$1/archives/partial"
    printf '%s\n' -o "Dir::State::Lists=$1/lists/" -o "Dir::Cache::Archives=$1/archives/"
  fi
}

# One online attempt; $1 is the attempt number.
online() {
  local extra=()
  [ -z "$live" ] || mapfile -t extra < <(cache_options "$live")
  if [ "$manager" = dnf ]; then
    [ "$1" -eq 1 ] || extra+=(--refresh)
    if [ ${#packages[@]} -eq 0 ]; then
      dnf makecache --setopt=retries=10 "${extra[@]}"
    else
      dnf install -y --setopt=retries=10 "${extra[@]}" "${options[@]}" "${packages[@]}"
    fi
  else
    apt-get update --error-on=any -o Acquire::Retries=3 "${extra[@]}" || return
    [ ${#packages[@]} -eq 0 ] || DEBIAN_FRONTEND=noninteractive apt-get install -y -o Acquire::Retries=3 \
      "${extra[@]}" "${options[@]}" "${packages[@]}"
  fi
}

offline() {
  local extra=()
  mapfile -t extra < <(cache_options "$snapshot")
  if [ "$manager" = dnf ]; then
    dnf install -y --cacheonly "${extra[@]}" "${options[@]}" "${packages[@]}"
  else
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-download "${extra[@]}" "${options[@]}" "${packages[@]}"
  fi
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
  grep -aE 'Error|error:|^E: |Failed|Cannot download|Curl error|All mirrors' "$log" | grep -av 'Some index files failed' |
    tail -n 1 | cut -c1-300 || true
}

if [ -n "$snapshot" ] && [ -d "$snapshot" ] && [ ! -e "$live" ]; then
  cp -a "$snapshot" "$live"
  rm -f "$live/putgd-snapshot"
fi

for ((attempt = 1; attempt <= attempts; attempt++)); do
  if online "$attempt" 2>&1 | tee "$log"; then
    [ "$attempt" -eq 1 ] || report notice "$attempt of $attempts" '**passed on retry**'
    [ -z "$snapshot" ] || [ ${#packages[@]} -eq 0 ] || refresh_snapshot
    exit 0
  fi
  error=$(last_error)
  if missing=$(grep -aE 'No match for argument|Unable to locate package' "$log"); then
    report error "$attempt of $attempts" '**failed**, not retried: a package does not exist' "${missing%%$'\n'*}"
    exit 1
  fi
  if [ "$attempt" -lt "$attempts" ]; then
    report warning "$attempt of $attempts" "failed, retrying in $delay s with refreshed metadata" "$error"
    sleep "$delay"
    delay=$((delay * 2))
  else
    report warning "$attempt of $attempts" 'failed' "$error"
  fi
done

if [ ${#packages[@]} -eq 0 ]; then
  report error '-' '**failed**: package metadata could not be refreshed'
elif [ -n "$snapshot" ] && [ -f "$snapshot/putgd-snapshot" ]; then
  saved=$(cat "$snapshot/putgd-snapshot")
  if offline 2>&1 | tee "$log"; then
    report warning snapshot "**installed offline from the package snapshot** saved $saved"
    exit 0
  fi
  report error snapshot '**failed**' "$(last_error)"
else
  report error snapshot '**failed**: no package snapshot to install from'
fi
exit 1
