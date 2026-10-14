set -eu
umask 077
mkdir -p /home/lab/i33/source
cd /home/lab/i33
sha256sum source.tar
python3 - <<'PY'
import hashlib,tarfile,json
from pathlib import Path
root=Path('/home/lab/i33');meta=json.loads((root/'source.json').read_text());assert hashlib.sha256((root/'source.tar').read_bytes()).hexdigest()==meta['archiveSha256']
with tarfile.open(root/'source.tar') as t:
 for m in t:
  p=Path(m.name);assert not p.is_absolute() and '..' not in p.parts and 'credentials.txt' not in p.parts and p.parts[0] not in ('build','work','.git')
 t.extractall(root/'source',filter='data')
print('Tracked archive identity verified and extracted.')
PY
cd source
python3 tools/verify_dependencies.py /home/lab/putgd-memory-build/dependencies/aws-sdk-cpp
cmake --preset dev -DCMAKE_PREFIX_PATH=/home/lab/putgd-memory-build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-memory-build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-memory-build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_SOURCE_COMMIT=061c2d9c569dc10b5c8681a7ad7beb3093e61b80 -DPUTGD_SOURCE_TREE=445dc0cbdc844833f534d589bbeaa78e1b03b454 -DPUTGD_SOURCE_COMMIT_COUNT=176
cmake --build --preset dev
mkdir -m 700 -p /tmp/i33v/r /tmp/i33v/t
python3 - <<'PREFLIGHT'
import json,subprocess
from pathlib import Path
saved=json.loads(Path('/home/lab/i33/contracts-socket-preflight.json').read_text())
assert saved['passed']
subprocess.run(['python3','tools/diagnostics/check_socket_paths.py','--socket']+[p['path'] for p in saved['paths']],check=True)
PREFLIGHT
XDG_RUNTIME_DIR=/tmp/i33v/r TMPDIR=/tmp/i33v/t ctest --preset dev --output-junit /home/lab/i33/dev-tests.xml
cmake --preset release -DCMAKE_PREFIX_PATH=/home/lab/putgd-memory-build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-memory-build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-memory-build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_SOURCE_COMMIT=061c2d9c569dc10b5c8681a7ad7beb3093e61b80 -DPUTGD_SOURCE_TREE=445dc0cbdc844833f534d589bbeaa78e1b03b454 -DPUTGD_SOURCE_COMMIT_COUNT=176
cmake --build --preset release --target putgd putgd-ui putgd-cli putgd-desktop-fixture putgd_s3_module putgd-capture putgd-s3-worker putgd-kde-region
umask 022
cpack --config build/release/CPackConfig.cmake -G DEB -D 'CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS=OWNER_READ;OWNER_WRITE;OWNER_EXECUTE;GROUP_READ;GROUP_EXECUTE;WORLD_READ;WORLD_EXECUTE' -B /home/lab/i33/packages
