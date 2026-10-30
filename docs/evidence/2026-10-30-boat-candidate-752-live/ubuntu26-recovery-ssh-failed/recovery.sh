set -euo pipefail
umask 077
mkdir -p /home/user/candidate/recovery-output
git clone --shared /home/user/candidate/source /home/user/candidate/recovery-source
cd /home/user/candidate/recovery-source
git checkout --detach 0c329b0692cadc52f03f91e59c06c5eda760bf04
test "$(git rev-parse HEAD)" = 0c329b0692cadc52f03f91e59c06c5eda760bf04
test -z "$(git status --porcelain)"
cat > /home/user/candidate/recovery-recipe.sh <<'PUTGD_RECOVERY_RECIPE'
set -euo pipefail
sudo() { if [[ ${1:-} == --preserve-env=* ]]; then shift; fi; "$@"; }
export -f sudo
export PATH=/usr/bin:/bin:$PATH
runner_state=$(mktemp -d /tmp/putgd-runner.XXXXXX)
export GITHUB_WORKSPACE=$PWD GITHUB_STEP_SUMMARY=$runner_state/summary.md GITHUB_ENV=$runner_state/env GITHUB_OUTPUT=$runner_state/output RUNNER_TEMP=$runner_state
export CCACHE_DIR=$PWD/build/ccache CCACHE_BASEDIR=$PWD CCACHE_MAXSIZE=256M
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
export PUTGD_SDK_JOBS=$(nproc) npm_config_fetch_retries=5 RELEASE_TAG=
(
apt=(-o Acquire::Retries=3 -o Acquire::http::Timeout=60 -o Acquire::https::Timeout=60)
bounded() { timeout --kill-after=30s 10m "$@"; }
for attempt in 1 2 3; do
  if command -v apt-get >/dev/null; then
    bounded apt-get update --error-on=any "${apt[@]}" && bounded apt-get install -y "${apt[@]}" git ca-certificates && exit 0
  else
    bounded dnf install -y --setopt=retries=10 --setopt=timeout=60 --setopt=minrate=1000 $([ "$attempt" = 1 ] || echo --refresh) git ca-certificates && exit 0
  fi
  echo "::warning::Package install (bootstrap): attempt $attempt of 3 failed"
  [ "$attempt" != 1 ] || printf '### Package install retries (native (%s) bootstrap)\n\n| Attempt | Outcome |\n| --- | --- |\n' 'ubuntu26.04' >> "$GITHUB_STEP_SUMMARY"
  echo "| $attempt of 3 | failed |" >> "$GITHUB_STEP_SUMMARY"
  [ "$attempt" = 3 ] || sleep $((attempt * 30))
done
exit 1
)
git config --global --add safe.directory "$PWD"
git config --global --add safe.directory "$GITHUB_WORKSPACE"
bash tools/releases/install-native-deps.sh
bash tools/ci/sdk-source.sh
bash tools/releases/provision-sdk.sh
python3 tools/proton/provision_runtime.py
export FORMAT=DEB PACKAGE_TARGET=ubuntu26.04 KDE_REQUIREMENT=--require-kde
ccache --zero-stats
cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF -DPUTGD_RELEASE_TAG="$RELEASE_TAG" -DPUTGD_BUNDLE_PROTON=ON -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_PACKAGE_TARGET="$PACKAGE_TARGET"
cmake --build --preset release --parallel "$(nproc)"
python3 tools/releases/native.py --build build/release --format "$FORMAT" --output "$PWD/build/release-packages" --proton-manifest build/dependencies/proton-runtime/manifest.json --require-proton $KDE_REQUIREMENT --package-target "$PACKAGE_TARGET"
ccache --show-stats
python3 tools/releases/stage_artifacts.py --kind native --packages-only
cp -a build/release-artifact/. /out/

cp build/release/build-identity/build-info.json /out/build-info.json
build/release/putgd-cli --version-json > /out/binary-version.json
git status --porcelain > /out/source-status.txt
test ! -s /out/source-status.txt
(dpkg-query -W || rpm -qa) > /out/packages.txt

PUTGD_RECOVERY_RECIPE
sudo -n nice -n 10 docker run --rm --init --network host --shm-size=1g \
 -e DEBIAN_FRONTEND=noninteractive -e LANG=C.UTF-8 -e LC_ALL=C.UTF-8 \
 -v /home/user/candidate/recovery-source:/src -v /home/user/candidate/recovery-output:/out \
 -v /home/user/candidate/recovery-recipe.sh:/recipe.sh:ro -w /src ubuntu:26.04@sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7 \
 bash /recipe.sh
sudo -n chown -R user:user /home/user/candidate/recovery-output
