"""Reboot a lab guest through its active desktop session so Shell/extension code reloads.

SSH sessions are not the active seat session, so polkit refuses `systemctl reboot`
there. Ask the desktop's own session manager, then prove the session process restarted."""
import sys, time
sys.path.insert(0, 'tools/lab'); import lab
run = sys.argv[1]
profile = lab.load_profile(lab.state(run)['profile'])
process = profile['session_process']
def started():
    return lab.ssh(run, f'ps -o lstart= -C {process} | head -1\n', timeout=10).strip()
before = started()
request = {'gnome-shell': 'gnome-session-quit --reboot --no-prompt',
           'plasmashell': 'qdbus6 org.kde.Shutdown /Shutdown org.kde.Shutdown.logoutAndReboot || qdbus org.kde.Shutdown /Shutdown org.kde.Shutdown.logoutAndReboot',
           'cinnamon': 'cinnamon-session-quit --reboot --no-prompt'}[process]
try:
    lab.guest_session(run, f'nohup sh -c "sleep 1; {request}" >/dev/null 2>&1 &\n')
except Exception:
    pass
deadline = time.monotonic() + 600
while True:
    time.sleep(5)
    try:
        now = started()
        if now and now != before:
            break
    except Exception:
        pass
    if time.monotonic() > deadline:
        raise SystemExit('desktop session did not restart after the reboot request')
time.sleep(15)
print(f'{process} restarted: before={before!r} after={now!r}')
print(lab.guest_session(run, '''echo session=$XDG_SESSION_TYPE desktop=$XDG_CURRENT_DESKTOP
command -v gnome-extensions >/dev/null && gnome-extensions info putgd@put.gd | grep -E "Path|State" || true
dpkg-query -W putgd
''').strip())
