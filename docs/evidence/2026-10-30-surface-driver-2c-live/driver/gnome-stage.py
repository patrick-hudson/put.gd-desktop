"""Stage the lab-only GNOME witness into this overlay (never a golden), reboot, enable extensions."""
import subprocess, sys, time
from pathlib import Path
sys.path.insert(0, '.')
from tools.lab import lab
run = sys.argv[1]
src = Path('tools/lab/surface_driver/compositor')
def guest(script, timeout=60):
    return subprocess.run(lab.ssh_args(run)+['sh', '-s'], input=script, text=True, capture_output=True, timeout=timeout)
files = ''.join("cat > /tmp/sd726w/%s <<'SDW'\n%s\nSDW\n" % (name, (src/'gnome-witness'/name).read_text()) for name in ('extension.js', 'metadata.json'))
print(guest('mkdir -p /tmp/sd726w\n'+files+"cat > /tmp/sd726w/stage.sh <<'SDW'\n"+(src/'stage-gnome-witness.sh').read_text()+"\nSDW\nsh /tmp/sd726w/stage.sh /tmp/sd726w\n").stdout)
print(subprocess.run(['python3', 'tools/lab/guest_reboot.py', run], text=True, capture_output=True, timeout=600).stdout[-800:])
time.sleep(20)
env = 'export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus\n'
print(guest(env+"gnome-extensions list --enabled; gnome-extensions list | grep -i put; for u in putgd-lab-witness@put.gd $(gnome-extensions list | grep -i 'putgd\\|put.gd' | grep -v lab-witness); do gnome-extensions enable $u; done; sleep 2; for u in $(gnome-extensions list | grep -i 'put'); do gnome-extensions info $u | grep -E 'UUID|State'; done").stdout)
