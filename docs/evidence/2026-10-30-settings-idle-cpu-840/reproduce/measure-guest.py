#!/usr/bin/env python3
import argparse,json,pathlib,sys,hashlib
sys.path.insert(0,'/home/user/lab-driver/tools/lab')
import lab
p=argparse.ArgumentParser();p.add_argument('run',type=pathlib.Path);p.add_argument('deb',type=pathlib.Path);p.add_argument('label');p.add_argument('--platform',required=True);p.add_argument('--sha256',required=True);a=p.parse_args();assert hashlib.sha256(a.deb.read_bytes()).hexdigest()==a.sha256,'Package transfer is incomplete or mismatched';out=pathlib.Path('/home/user/840-'+a.label);out.mkdir(mode=0o700)
install=lab.install(a.run,deb=a.deb);(out/'install.json').write_text(json.dumps(install,indent=2));(out/'package.json').write_bytes((a.run/'package.json').read_bytes())
lab.put(a.run,pathlib.Path('/home/user/840-profile.py'),'840-profile.py')
lab.put(a.run,pathlib.Path('/home/user/840-check_socket_paths.py'),'840-check_socket_paths.py')
profile='/tmp/s840'+a.label.replace('-','')
text=lab.guest_session(a.run,f'python3 ~/840-profile.py /usr/bin {profile} --seconds 60 --views settings --platform {a.platform} --accessibility\n',timeout=100)
(out/'cpu.log').write_text(text)
lab.get(a.run,profile+'/receipt.json',out/'receipt.json');lab.get(a.run,profile+'/app.log',out/'app.log');lab.get(a.run,profile+'/socket-preflight.json',out/'socket-preflight.json')
# Collect only version/desktop identities; no environment or app configuration.
versions=lab.ssh(a.run,". /etc/os-release; printf '%s %s\\n' \"$ID\" \"$VERSION_ID\"; gnome-shell --version 2>/dev/null || cinnamon --version; dpkg-query -W libqt6core6t64 libqt6core6 2>/dev/null || true\n")
(out/'desktop-versions.txt').write_text(versions)
print(text,flush=True)
