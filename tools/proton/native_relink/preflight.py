#!/usr/bin/env python3
"""Explicit local-source rebuild. Never install or download a toolchain."""
import argparse,json,os,pathlib,re,shutil,subprocess,tomllib
P=argparse.ArgumentParser();P.add_argument('--prepared',type=pathlib.Path,required=True);P.add_argument('--bun',type=pathlib.Path,required=True);P.add_argument('--llvm-prefix',type=pathlib.Path,required=True);P.add_argument('--rust-prefix',type=pathlib.Path,required=True);P.add_argument('--check-only',action='store_true');a=P.parse_args()
R=pathlib.Path(__file__).resolve().parent;D=a.prepared.resolve();B=D/'bun';errors=[];evidence={}
pin=tomllib.loads((R/'rust-nightly-2026-09-15.toml').read_text())
def run(label,path,args):
    if not path.is_file() or not os.access(path,os.X_OK): errors.append(label+' missing: '+str(path));return ''
    if path.resolve().name=='rustup': errors.append(label+' is a rustup proxy; a pre-existing direct compiler is required');return ''
    p=subprocess.run([str(path),*args],capture_output=True,text=True,cwd='/tmp',timeout=20)
    out=(p.stdout+p.stderr).strip();evidence[label]=dict(path=str(path),returncode=p.returncode,output=out)
    if p.returncode:errors.append(label+' version query failed')
    return out
for tool in ['clang','clang++','ld.lld','llvm-ar','llvm-ranlib']:
    out=run(tool,a.llvm_prefix/'bin'/tool,['--version'])
    if out and not re.search(r'\b23\.1\.\d+\b',out):errors.append(tool+' must be LLVM23.1.x')
out=run('rustc',a.rust_prefix/'bin/rustc',['-vV'])
if out and 'commit-hash: '+pin['pkg']['rustc']['git_commit_hash'] not in out:errors.append('rustc does not match nightly-2026-09-15 commit')
out=run('cargo',a.rust_prefix/'bin/cargo',['-vV'])
if out and 'commit-hash: 7941be6fb416b4cd9666aef7b858dfea25587a8c' not in out:errors.append('cargo does not match nightly-2026-09-15 commit')
for tool,args in [('cmake',['--version']),('ninja',['--version']),('ruby',['--version']),('perl',['--version']),('python3',['--version']),('git',['--version']),('npm',['--version']),('nasm',['--version']),('pkg-config',['--atleast-version=70.1','icu-uc','icu-i18n'])]:
    path=shutil.which(tool)
    if not path: errors.append(tool+' missing');continue
    out=run(tool,pathlib.Path(path),args)
    if tool=='cmake' and out:
        m=re.search(r'(\d+)\.(\d+)',out)
        if not m or tuple(map(int,m.groups()))<(3,24):errors.append('cmake>=3.24 required')
out=run('bootstrap-bun',a.bun,['--revision'])
if out and out!='1.4.4+38afee6c8':errors.append('bootstrap Bun identity must be1.4.4+38afee6c8')
if not (D/'assembly.json').exists():errors.append('prepared source assembly missing')
std_lock=a.rust_prefix/'lib/rustlib/src/rust/library/Cargo.lock'
import hashlib
if not std_lock.is_file() or hashlib.sha256(std_lock.read_bytes()).hexdigest()!='75848db58a70444bfb62c649b103d19c5d92fede325eb0c8c0e5442848448669':errors.append('pinned rust-src Cargo.lock missing or changed')

report=dict(ok=not errors,nativeBuildAttempted=False,toolchainInstalled=False,errors=errors,evidence=evidence)
print(json.dumps(report,indent=2))
if errors:raise SystemExit(2)
if a.check_only:raise SystemExit(0)
# Every input comes from the verified delivered bundle or pre-installed distro
# tools. Local WebKit forces source compilation instead of using a prebuilt JSC.
subprocess.run(['git','-C',str(D/'WebKit'),'checkout','--detach','d6375c72affe0f70222cdcb45012b10f0bf3a9d5'],check=True)
env=os.environ.copy();env.update(BUN_TOOLCHAIN_LLVM=str(a.llvm_prefix.resolve()),BUN_TOOLCHAIN_RUST=str(a.rust_prefix.resolve()),BUN_TOOLCHAIN_CARGO=str((a.rust_prefix/'bin/cargo').resolve()),BUN_WEBKIT_PATH=str(D/'WebKit'),BUN_BUILD_PREFETCH_DIR=str(D/'prefetch'),CARGO_HOME=str(D/'cargo-home'),CARGO_NET_OFFLINE='true',npm_config_offline='true',npm_config_audit='false',npm_config_fund='false',GIT_SHA='38afee6c84fcf48c0edb2466aa4b27d287a59c30',RUSTUP_AUTO_INSTALL='0')
env['PATH']=str(a.llvm_prefix/'bin')+os.pathsep+str(a.rust_prefix/'bin')+os.pathsep+env['PATH']
subprocess.run([str(a.bun.resolve()),'scripts/build.ts','--profile=release-local','--build-dir=build/release-local','--package-manager=npm'],cwd=B,env=env,check=True)
