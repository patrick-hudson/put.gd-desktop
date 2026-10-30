import importlib.util,json,time
from pathlib import Path
r=Path(__file__).resolve().parent
source=Path('/home/patrick/.t3/worktrees/put.gd/t3-c14e8bd4')
spec=importlib.util.spec_from_file_location('candidate',source/'tools/lab/boat/candidate.py')
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
a=json.loads((r/'run-a/ui-catalog/receipt.json').read_text())
failed=json.loads((r/'run-a/ubuntu26.04/receipt.json').read_text())
host,port=a['placement']['sshEndpoint'].rsplit(':',1)
options=c.ssh_options(r/'run-a/ui-catalog/known_hosts');ssh=['ssh',*options,'-p',port,'user@'+host]
out=r/'ubuntu26-recovery';out.mkdir(mode=0o700)
image=failed['containerImage']
recipe=(r/'run-a/ubuntu26.04/recipe.sh').read_text()
full_recipe='set -euo pipefail\nsudo() { if [[ ${1:-} == --preserve-env=* ]]; then shift; fi; \"$@\"; }\nexport -f sudo\n'+recipe+'\ncp build/release/build-identity/build-info.json /out/build-info.json\nbuild/release/putgd-cli --version-json > /out/binary-version.json\ngit status --porcelain > /out/source-status.txt\ntest ! -s /out/source-status.txt\n(dpkg-query -W || rpm -qa) > /out/packages.txt\n'
script=f'''set -euo pipefail
umask 077
mkdir -p /home/user/candidate/recovery-output
cd /home/user/candidate/recovery-source
git fsck --full
if test -d build; then sudo -n mv build /home/user/candidate/recovery-build-before-clean-retry; fi
cd /home/user/candidate/recovery-source
git checkout --detach {a['commit']}
test \"$(git rev-parse HEAD)\" = {a['commit']}
test -z \"$(git status --porcelain)\"
cat > /home/user/candidate/recovery-recipe.sh <<'PUTGD_RECOVERY_RECIPE'
{full_recipe}
PUTGD_RECOVERY_RECIPE
sudo -n nice -n 10 docker run --name putgd-recovery-02818a97bfa24edb --init --network host --shm-size=1g \\
 -e DEBIAN_FRONTEND=noninteractive -e LANG=C.UTF-8 -e LC_ALL=C.UTF-8 \\
 -v /home/user/candidate/recovery-source:/src -v /home/user/candidate/recovery-output:/out \\
 -v /home/user/candidate/recovery-recipe.sh:/recipe.sh:ro -w /src {image} \\
 bash /recipe.sh
sudo -n chown -R user:user /home/user/candidate/recovery-output
'''
(out/'recovery.sh').write_text(script)
receipt={'state':'pending','source':a['commit'],'sandboxId':a['sandboxId'],'runNonce':a['runNonce'],'originalFailure':'npm ECONNRESET during Proton runtime provisioning; original controller result remains failed','scope':'manual Ubuntu 26.04 recovery on retained run-A catalog worker in a separate clean checkout/build; no new sandbox', 'originalFailedSandboxId':failed['sandboxId'], 'placement':a['placement'],'containerImage':image,'startedAt':time.time()}
c.write_json(out/'recovery.json',receipt)
try:
 with (out/'build.log').open('w') as log:c.run([*ssh,'bash -s'],input=script,log=log,timeout=5400)
 # Collect the recovery separately; leave original /output unchanged for the controller.
 c.run([*ssh,'set -eu; cd /home/user/candidate; tar -cf recovery.tar -C recovery-output .; sha256sum recovery.tar'],log=(out/'remote-sha256.txt').open('w'))
 digest=(out/'remote-sha256.txt').read_text().split()[0]
 c.run(['scp',*options,'-P',port,f'user@{host}:/home/user/candidate/recovery.tar',str(out/'recovery.tar')],timeout=900)
 c.restore_archive(out/'recovery.tar',out/'artifacts',digest)
 receipt['identity']=c.verify_identity(out/'artifacts',a['commit'],'ubuntu26.04')
 receipt['files']=c.inventory(out/'artifacts')
 receipt['transportSha256']=digest
 receipt['state']='passed'
except Exception as error:
 receipt.update(state='failed',error=str(error));raise
finally:
 receipt['finishedAt']=time.time();c.write_json(out/'recovery.json',receipt)
print(json.dumps(receipt,indent=2),flush=True)
