# guest-up.sh NAME PROFILE GOLDEN DEB  (run inside ssh -A; private key stays on the workstation)
set -u
name=$1 profile=$2 golden=$3 deb=$4
cd ~/work/put.gd/src || exit 1
run=$PWD/work/lab-runs/$name
test ! -e "$run" || { echo run-exists; exit 1; }
mkdir -p ~/sd726/out
uptime; test -r /dev/kvm -a -w /dev/kvm && echo kvm-rw
python3 -c 'import os,json;s=os.statvfs(".");print(json.dumps(dict(availableGiB=round(s.f_bavail*s.f_frsize/2**30,1),inodes=s.f_favail)))'
free -g | sed -n 2p
python3 tools/diagnostics/check_socket_paths.py --socket "$run/qmp.sock" "$run/vnc.sock" >/dev/null && echo socket-paths-ok
test -n "${SSH_AUTH_SOCK:-}" && ssh-add -l | awk '{print "agent-keys", NR}' | tail -1
timeout 600 python3 tools/lab/lab.py launch --profile $profile --golden "$golden" --run "$run" --bootstrap-key ~/snapshot-bootstrap/identity.pub --known-hosts ~/snapshot-bootstrap/known_hosts --acceleration kvm --timeout 240 > ~/sd726/out/$name-launch.json 2> ~/sd726/out/$name-launch.err; echo launch=$?
python3 -c "import json;d=json.load(open('$HOME/sd726/out/$name-launch.json'));print(d.get('status'),d.get('display'),d.get('id'))" 2>/dev/null || tail -5 ~/sd726/out/$name-launch.err
python3 tools/lab/storage_preflight.py --run "$run" --output ~/sd726/out/$name-preflight.json > /dev/null 2>&1; echo storage_preflight=$?
timeout 900 python3 tools/lab/lab.py install --run "$run" --deb "$deb" > ~/sd726/out/$name-install.json 2> ~/sd726/out/$name-install.err; echo install=$?
tail -c 300 ~/sd726/out/$name-install.json; tail -3 ~/sd726/out/$name-install.err
