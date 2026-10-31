set -euo pipefail
umask 077
root=$(mktemp -d /dev/shm/kc.XXXXXX)
mkdir -m 700 "$root"/{config,data,state,cache,runtime}
cd /home/user/work/put.gd/src
python3 tools/diagnostics/check_socket_paths.py --socket "$root/runtime/putgd/session.sock" > /home/user/kde-evidence/catalog-socket-preflight.json
export XDG_CONFIG_HOME="$root/config" XDG_DATA_HOME="$root/data" XDG_STATE_HOME="$root/state" XDG_CACHE_HOME="$root/cache" XDG_RUNTIME_DIR="$root/runtime" QT_QPA_PLATFORM=offscreen PUTGD_CATALOG_THEME=light
build/dev/putgd-ui-catalog /home/user/kde-catalog > /home/user/kde-evidence/catalog-render-host.log 2>&1
python3 - <<'PY'
import hashlib,json
from pathlib import Path
p=Path('/home/user/kde-catalog')
Path('/home/user/kde-evidence/catalog-source.json').write_text(json.dumps(dict(placement='boat',kind='synthetic offscreen reference and controls; not desktop proof',sourceCommit='6ad9f957e6544f30c5e45c08af2306db77a9ff64',qt='host distro Qt 6.4.2',theme='light',manifestSha256=hashlib.sha256((p/'manifest.json').read_bytes()).hexdigest()),indent=2)+'\n')
PY
