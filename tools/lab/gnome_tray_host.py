"""The GNOME tray host the lab expects, and the check that it is serving (stdlib only).

Stock Fedora GNOME has no tray host. A Fedora user who wants put.gd's tray installs and enables
Fedora's AppIndicator extension, which serves org.kde.StatusNotifierWatcher (docs/desktops/gnome.md).
The nightly's disposable Fedora GNOME guest installs it (nightly_desktop_guest.py), and
desktop_lock.py refuses a physical GNOME builder's desktop without it, so GNOME rows run with a tray
unless a test asks for the no-tray path on purpose.
"""

TRAY_HOST_PACKAGE = 'gnome-shell-extension-appindicator'
TRAY_HOST_EXTENSION = 'appindicatorsupport@rgcjonas.gmail.com'
TRAY_HOST_ENABLE = f'gnome-extensions enable {TRAY_HOST_EXTENSION}'


def probe_source(enable=False, wait=0):
    """Python source to run in the desktop session. It prints one JSON line and exits 0 only when
    the extension is enabled and active and a StatusNotifierWatcher has a registered host.
    With `enable`, it first enables the extension (persistently, in org.gnome.shell
    enabled-extensions); it waits up to `wait` seconds for the host to appear."""
    enabling = "subprocess.run(['gnome-extensions','enable',uuid],check=True,capture_output=True)\n" if enable else ''
    return (f'''import dbus,json,os,subprocess,time
uuid={TRAY_HOST_EXTENSION!r}
plain=dict(os.environ,LC_ALL='C')  # gnome-extensions translates its Enabled/State labels
{enabling}bus=dbus.SessionBus()
deadline=time.monotonic()+{float(wait)!r}
while True:
 registered=False
 try:
  watcher=bus.get_object('org.kde.StatusNotifierWatcher','/StatusNotifierWatcher')
  registered=bool(dbus.Interface(watcher,'org.freedesktop.DBus.Properties').Get(
   'org.kde.StatusNotifierWatcher','IsStatusNotifierHostRegistered'))
 except dbus.DBusException:pass
 shell=bool(bus.name_has_owner('org.gnome.Shell'))
 info=subprocess.run(['gnome-extensions','info',uuid],capture_output=True,text=True,env=plain)
 fields=dict(line.strip().partition(': ')[::2] for line in info.stdout.splitlines() if ': ' in line)
 installed,enabled,state=shell and info.returncode==0,fields.get('Enabled')=='Yes',fields.get('State')
 ready=registered and enabled and state=='ACTIVE'
 if ready or time.monotonic()>=deadline:break
 time.sleep(.25)
print(json.dumps(dict(extension=uuid,shell=shell,installed=installed,enabled=enabled,state=state,hostRegistered=registered,
 info=[line.strip() for line in info.stdout.splitlines() if line.strip()])))
raise SystemExit(0 if ready else 1)
''')


# A re-login ends the desktop session, so its fix first takes the desktop lock.
RELOGIN = ('re-login while holding the desktop lock (acquire with --no-tray-host "repairing the tray host", '
           'then sudo -n systemctl restart gdm)')


def missing(report):
    """What a failed probe report says is wrong, and the one-line fix."""
    if report.get('error'):
        return (f'the tray host check could not run ({report["error"]})',
                'install python3-dbus and check that the desktop session is logged in')
    if not report.get('shell', True):
        return ('GNOME Shell is not running on the desktop session bus', 'log in to the GNOME desktop: ' + RELOGIN)
    if not report.get('installed'):
        return (f'{TRAY_HOST_EXTENSION} is not installed',
                f'sudo dnf install {TRAY_HOST_PACKAGE}, {TRAY_HOST_ENABLE}, then {RELOGIN}')
    if not report.get('enabled'):
        return (f'{TRAY_HOST_EXTENSION} is not enabled',
                f'{TRAY_HOST_ENABLE} in the desktop session; if it stays INACTIVE, {RELOGIN}')
    if report.get('state') != 'ACTIVE':
        return (f'{TRAY_HOST_EXTENSION} is enabled but {report.get("state") or "not loaded"}',
                f'unlock the screen; if that is not it, {RELOGIN}')
    return ('no StatusNotifierWatcher has a registered host on the session bus',
            f'gnome-extensions disable and enable {TRAY_HOST_EXTENSION}; if that does not help, {RELOGIN}')
