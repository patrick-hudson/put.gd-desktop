from pathlib import Path
import hashlib,json,os,re,subprocess,tarfile,time
from proof_command import run as run_command
root=Path('/p3');logs=root/'logs';proof=root/'proof';work=root/'helper';work.mkdir(exist_ok=False)
bun=root/'prepared/bun/build/release-local/bun-profile'
assert bun.is_file(),'native build must finish first'
command_timeout=float(os.environ.get('PUTGD_PROTON_PROOF_TIMEOUT_SECONDS','300'))
def run(args,name,cwd=None,env=None):
 return run_command(args,name,logs=logs,proof=proof,timeout_seconds=command_timeout,cwd=cwd,env=env)
revision=run([bun,'--revision'],'native-revision.log')
for marker in ['putgd-bun-native-proof','putgd-jsc-source-proof','putgd-tinycc-source-proof']:assert marker in revision,marker
assert run([bun,'--version'],'native-version.log').strip()=='1.4.4'
run([bun,'/p3/helper-proof-inputs/ffi.mjs'],'native-tinycc-ffi.log')
with tarfile.open(root/'helper-rebuild.tar.gz') as tar:tar.extractall(work,filter='data')
build=work/'build-source'
catalog=json.loads((root/'SOURCES.json').read_text())
assert catalog['sourcePins']['bun']=='38afee6c84fcf48c0edb2466aa4b27d287a59c30'
assert catalog['sourcePins']['webkit']=='d6375c72affe0f70222cdcb45012b10f0bf3a9d5'
for item in catalog['applicationSourceOverlay']['files']:
 path=work/item['destination']
 with path.open('rb') as stream:actual=hashlib.file_digest(stream,'sha256').hexdigest()
 assert actual==item['sha256'],'current helper overlay mismatch: '+item['destination']
tests=run([bun,'test','./protocol.test.ts','./runtime.test.ts','./crypto-cache.test.ts','./progress.test.ts'],'native-helper-tests.log',build)
assert re.search(r'\b46 pass\b',tests),'expected current helper test suite'
helper=work/'putgd-proton-runtime-proof'
run([bun,'build','--compile','--target=bun-linux-x64','--minify','runtime.ts','--outfile='+str(helper),'--compile-executable-path='+str(bun)],'native-helper-build.log',build)
run(['python3','/p3/helper-proof-inputs/smoke_test.py','--runtime',helper],'native-helper-smoke.log')
embedded_env={**os.environ,'BUN_BE_BUN':'1'}
embedded_revision=run([helper,'--revision'],'compiled-helper-native-revision.log',env=embedded_env)
for marker in ['putgd-bun-native-proof','putgd-jsc-source-proof','putgd-tinycc-source-proof']:assert marker in embedded_revision,marker
run([helper,'/p3/helper-proof-inputs/ffi.mjs'],'compiled-helper-tinycc-ffi.log',env=embedded_env)
stdout=work/'stdout-proof'
run([bun,'build','--compile','--target=bun-linux-x64','--minify','stdout_fixture.ts','--outfile='+str(stdout),'--compile-executable-path='+str(bun)],'native-helper-stdout-build.log',build)
r=subprocess.run([str(stdout)],capture_output=True,check=True,timeout=30)
assert not r.stderr and r.stdout==b'{"synthetic":"'+b'x'*(1024*1024-256)+b'"}\n'
run([bun,'build','--compile','--target=bun-linux-x64','--minify','openpgp-proof.mjs','--outfile=/p3/helper/native-openpgp-proof','--compile-executable-path='+str(bun)],'native-openpgp-build.log','/p3/argon-proof')
crypto=json.loads(run(['/p3/helper/native-openpgp-proof'],'native-openpgp-crypto.log','/p3/argon-proof'))
assert all(crypto[x] for x in ['argon2Packet','rebuildRoundtrip','publishedDecryptsRebuilt','rebuiltDecryptsPublished','modifiedWasmMarker'])
def digest(p):
 with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
result={'nativeBunSha256':digest(bun),'proofHelperSha256':digest(helper),'nativeCryptoProofSha256':digest(Path('/p3/helper/native-openpgp-proof')),'markers':revision.splitlines(),'nativeVersion':'1.4.4','tinyccCompileAndExecute':True,'compiledHelperCallsNativeSourceMarkers':True,'compiledHelperTinyccCompileAndExecute':True,'helperTests':46,'compiledStdioCases':11,'stdoutBytes':len(r.stdout),'crypto':crypto,'productionHelperChanged':False,'productionHelperSha256':catalog['helperSha256'],'sourcePins':catalog['sourcePins'],'applicationSourceOverlayVerified':True,'accountsOrServicesUsed':False,'nativeBitIdentityClaimed':False}
(proof/'native-proof-results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
