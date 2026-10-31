"""Resume only the existing prepared native tree; never reapply source markers."""
from pathlib import Path
import json
import os
import subprocess
import time

root = Path('/p3')
assert (root / 'prepared/assembly.json').is_file()
assert (root / 'proof/source-changes.json').is_file()
log = root / 'logs/06-native-resume-j4.log'
env = os.environ.copy()
env.update(BUN_TOOLCHAIN_LLVM='/usr/lib/llvm-23', BUN_TOOLCHAIN_RUST='/opt/rust',
           BUN_TOOLCHAIN_CARGO='/opt/rust/bin/cargo', BUN_WEBKIT_PATH='/p3/prepared/WebKit',
           BUN_BUILD_PREFETCH_DIR='/p3/prepared/prefetch', CARGO_HOME='/p3/prepared/cargo-home',
           CARGO_NET_OFFLINE='true', npm_config_offline='true', npm_config_audit='false',
           npm_config_fund='false', GIT_SHA='38afee6c84fcf48c0edb2466aa4b27d287a59c30',
           RUSTUP_AUTO_INSTALL='0', CMAKE_BUILD_PARALLEL_LEVEL='4', CARGO_BUILD_JOBS='2')
args = ['/p3/bootstrap-bun', 'scripts/build.ts', '--profile=release-local',
        '--build-dir=build/release-local', '--package-manager=npm', '-j4']
(root / 'phase.json').write_text(json.dumps({'phase': log.name, 'started': time.time(), 'argv': args}) + '\n')
with log.open('x') as stream:
    result = subprocess.run(args, cwd=root / 'prepared/bun', env=env,
                            stdout=stream, stderr=subprocess.STDOUT)
if result.returncode:
    raise SystemExit('Incremental native build failed: ' + str(result.returncode))
(root / 'phase.json').write_text(json.dumps({'phase': 'native build finished', 'finished': time.time()}) + '\n')
