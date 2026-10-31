from pathlib import Path
import subprocess,os,json,hashlib,time
root=Path('/p3');logs=root/'logs';logs.mkdir(exist_ok=True)
phase=root/'phase.json'
def run(args,name,cwd=None,env=None):
 phase.write_text(json.dumps({'phase':name,'started':time.time(),'argv':args})+'\n')
 with (logs/name).open('w') as stream:
  result=subprocess.run(args,cwd=cwd,env=env,stdout=stream,stderr=subprocess.STDOUT)
 if result.returncode:raise SystemExit(name+' failed '+str(result.returncode))
run(['python3','/p3/bundle/assemble_offline.py','--destination','/p3/prepared'],'01-source-assembly.log')
run(['python3','/p3/install_std_vendor.py'],'01a-std-vendor.log')
run(['python3','/p3/rebuild_native.py','--prepared','/p3/prepared','--bun','/p3/bootstrap-bun','--llvm-prefix','/usr/lib/llvm-23','--rust-prefix','/opt/rust','--check-only'],'02-preflight.log')
run(['git','-C','/p3/prepared/WebKit','checkout','--detach','d6375c72affe0f70222cdcb45012b10f0bf3a9d5'],'03-webkit-checkout.log')
run(['python3','/p3/apply_proof.py'],'04-source-modification.log')
env=os.environ.copy();env.update(BUN_TOOLCHAIN_LLVM='/usr/lib/llvm-23',BUN_TOOLCHAIN_RUST='/opt/rust',BUN_TOOLCHAIN_CARGO='/opt/rust/bin/cargo',BUN_WEBKIT_PATH='/p3/prepared/WebKit',BUN_BUILD_PREFETCH_DIR='/p3/prepared/prefetch',CARGO_HOME='/p3/prepared/cargo-home',CARGO_NET_OFFLINE='true',npm_config_offline='true',npm_config_audit='false',npm_config_fund='false',GIT_SHA='38afee6c84fcf48c0edb2466aa4b27d287a59c30',RUSTUP_AUTO_INSTALL='0',CMAKE_BUILD_PARALLEL_LEVEL='2',CARGO_BUILD_JOBS='2')
run(['/p3/bootstrap-bun','scripts/build.ts','--profile=release-local','--build-dir=build/release-local','--package-manager=npm','-j2'],'05-native-build.log',cwd='/p3/prepared/bun',env=env)
phase.write_text(json.dumps({'phase':'native build finished','finished':time.time()})+'\n')
