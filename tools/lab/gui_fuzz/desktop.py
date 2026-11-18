"""What differs between desktops: the fence, the session processes, pointer acceleration, monitor churn, clipboard, portal windows.

The walk, the watchdog and the actor ask the Desktop; nothing else in the package names a compositor.
"""
import os
import subprocess
import time

from gui_fuzz.world import run


def host_env():
    """The session's environment for desktop settings tools: put.gd's private XDG dirs would point gsettings at a different dconf."""
    env = dict(os.environ)
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME'):
        env.pop(name, None)
    return env


def host_run(args, timeout=15):
    try:
        done = subprocess.run(args, capture_output=True, text=True, timeout=timeout, env=host_env())
        return done.returncode, done.stdout, done.stderr
    except (OSError, subprocess.SubprocessError) as error:
        return -1, '', str(error)


class Desktop:
    name = ''
    session_procs = ()  # losing the first ends the session; the rest are restartable shells
    restartable = ()
    layer_kinds = frozenset()
    portal_classes = frozenset()
    shell_focus_classes = ('',)
    crash_viewers = ()
    tray_prefix = 'org.kde.StatusNotifierItem.putgd'
    edge = 8
    shell_label = 'the compositor'
    session_env_process = ''
    journal_args = ()  # journalctl selectors for the shell's own log

    def make_fence(self):
        raise NotImplementedError

    def flatten_acceleration(self, dev):
        return None

    def churn_outputs(self, walker):
        """One monitor change: hotplug or scale."""

    def restore_outputs(self, walker):
        """Back to two monitors side by side."""

    def set_clipboard(self, walker, kind, payload):
        """kind: text, image, uris, clear. payload: bytes."""

    def shell_restart_check(self, now):
        """Return a problem text once the restarted shell should have put.gd back, or None."""
        return None

    fence = None

    def close_window(self, window):
        """Close a window put.gd did not open (an app the desktop started for an Open action). False when this desktop cannot."""
        close = getattr(self.fence, 'close', None)
        if not close:
            return False
        try:
            close(window['id'])
            return True
        except Exception:
            return False


class Kde(Desktop):
    name = 'kde'
    session_procs = ('kwin_wayland', 'plasmashell')
    restartable = ('plasmashell',)
    layer_kinds = frozenset({'KWin::LayerShellV1Window'})
    portal_classes = frozenset({'org.freedesktop.impl.portal.desktop.kde'})
    shell_focus_classes = ('plasmashell', 'org.kde.plasmashell', '')
    crash_viewers = ('org.kde.drkonqi',)
    shell_label = 'KWin'
    session_env_process = 'plasmashell'

    def make_fence(self):
        from gui_fuzz.fence import Fence
        return Fence()

    def flatten_acceleration(self, dev):
        return dev.flatten_acceleration()

    def churn_outputs(self, walker):
        a, rng = walker.a, walker.rng
        names = [o['name'] for o in walker.w.outputs()]
        choice = rng.randint(0, 3)
        if choice == 0 and 'Virtual-2' in names or (choice == 1 and 'Virtual-2' not in names):
            a.sh('kscreen-doctor', 'output.Virtual-2.disable')
        elif choice == 1:
            a.sh('kscreen-doctor', 'output.Virtual-2.enable')
        elif choice == 2:
            a.sh('kscreen-doctor', 'output.Virtual-1.scale.%s' % rng.choice(['1', '1.25', '1.5', '2']))
        else:
            a.sh('kscreen-doctor', 'output.Virtual-2.enable')
            a.sh('kscreen-doctor', 'output.Virtual-2.scale.%s' % rng.choice(['1', '1.5']))
        a.sleep(1.5)
        self.close_gap(walker)

    def close_gap(self, walker):
        """Keep the two outputs side by side: a gap between them stops the pointer at the edge of the first."""
        outs = {o['name']: o['geom'] for o in walker.w.outputs()}
        if 'Virtual-1' in outs and 'Virtual-2' in outs and outs['Virtual-2'][0] != outs['Virtual-1'][2]:
            walker.a.sh('kscreen-doctor', 'output.Virtual-2.position.%d,0' % outs['Virtual-1'][2])
            walker.a.sleep(1.0)

    def restore_outputs(self, walker):
        names = [o['name'] for o in walker.w.outputs()]
        if 'Virtual-2' not in names:
            walker.a.sh('kscreen-doctor', 'output.Virtual-2.enable')
            walker.a.sleep(1.5)
        self.close_gap(walker)

    def set_clipboard(self, walker, kind, payload):
        if kind == 'text':
            walker.a.sh('busctl', '--user', 'call', 'org.kde.klipper', '/klipper', 'org.kde.klipper.klipper', 'setClipboardContents', 's', payload.decode() or 'x')
        elif kind == 'image':
            walker.log.write({'op': 'note', 'action': 'wl-copy image'})
            _wl_copy(['--type', 'image/png'], payload)
        elif kind == 'uris':
            walker.log.write({'op': 'note', 'action': 'wl-copy uri-list'})
            _wl_copy(['--type', 'text/uri-list'], payload)
        else:
            walker.a.sh('wl-copy', '--clear')

    def shell_restart_check(self, now):
        rc, out, _ = run(['busctl', '--user', 'get-property', 'org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher',
                          'org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems'], 8)
        if 'putgd' not in out:
            return 'put.gd is not among the registered tray items 30 s after plasmashell restarted: ' + out[:200]
        return None


def _wl_copy(args, payload):
    subprocess.run(['wl-copy'] + args, input=payload, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)


class Gnome(Desktop):
    name = 'gnome'
    session_procs = ('gnome-shell',)
    restartable = ()
    layer_kinds = frozenset({'ShellOverlay'})
    # the GTK file chooser of xdg-desktop-portal-gnome, which put.gd opens without a parent when the toolkit has no handle to export
    portal_classes = frozenset({'org.gnome.Nautilus', 'xdg-desktop-portal-gnome', 'org.freedesktop.impl.portal.desktop.gnome', 'org.gnome.Shell.Extensions'})
    shell_focus_classes = ('', 'gjs')  # Shell actors take no Meta focus; Desktop Icons NG is a desktop window of class gjs
    crash_viewers = ('apport', 'Apport')
    shell_label = 'Mutter'
    session_env_process = 'gnome-shell'
    journal_args = ('-t', 'gnome-shell')

    def make_fence(self):
        from gui_fuzz.fence_gnome import GnomeFence
        self.fence = GnomeFence()
        return self.fence

    def flatten_acceleration(self, dev):
        host_run(['gsettings', 'set', 'org.gnome.desktop.peripherals.mouse', 'accel-profile', 'flat'])
        host_run(['gsettings', 'set', 'org.gnome.desktop.peripherals.mouse', 'speed', '0'])
        return 'gsettings'

    def layout(self, walker, heads, scale1='1', scale2='1'):
        """gdctl: Virtual-1 primary at 0,0, Virtual-2 to its right."""
        args = ['gdctl', 'set', '--logical-monitor', '--primary', '--monitor', 'Virtual-1', '--scale', scale1]
        if heads > 1:
            args += ['--logical-monitor', '--monitor', 'Virtual-2', '--right-of', 'Virtual-1', '--scale', scale2]
        walker.log.write({'op': 'sh', 'a': args})
        host_run(args, 20)
        walker.a.sleep(1.5)

    def churn_outputs(self, walker):
        rng = walker.rng
        heads = len(walker.w.outputs())
        choice = rng.randint(0, 3)
        if choice == 0:
            self.layout(walker, 1 if heads > 1 else 2)
        elif choice == 1:
            self.layout(walker, heads, scale1=rng.choice(['1', '1.25', '1.5', '2']))
        elif choice == 2:
            self.layout(walker, 2, scale2=rng.choice(['1', '1.5']))
        else:
            self.layout(walker, 2)

    def restore_outputs(self, walker):
        outs = walker.w.outputs()
        if len(outs) < 2 or outs[0]['scale'] != 1:
            self.layout(walker, 2)

    def set_clipboard(self, walker, kind, payload):
        walker.log.write({'op': 'note', 'action': 'clipboard ' + kind})
        mime = {'image': 'image/png', 'uris': 'text/uri-list'}.get(kind, kind)
        try:
            self.fence.clipboard(mime, payload)
        except Exception as error:  # the Shell may be busy: the walk goes on
            walker.log.write({'op': 'harness_error', 'action': 'clipboard', 'error': repr(error)[:160]})


class Cinnamon(Desktop):
    """Cinnamon (Muffin) on X11: the window tree from the X server, xrandr for monitors, xclip for the clipboard."""
    name = 'cinnamon'
    session_procs = ('cinnamon',)
    restartable = ('cinnamon',)
    layer_kinds = frozenset({'X11Overlay'})
    portal_classes = frozenset({'xdg-desktop-portal-gtk', 'xdg-desktop-portal-xapp'})
    shell_focus_classes = ('', 'nemo-desktop', 'cinnamon')
    crash_viewers = ('apport', 'mintreport')
    shell_label = 'the X server'
    session_env_process = 'cinnamon'
    journal_args = ('-t', 'cinnamon')

    def make_fence(self):
        from gui_fuzz.fence_x11 import X11Fence
        self.fence = X11Fence()
        return self.fence

    def flatten_acceleration(self, dev):
        for schema in ('org.cinnamon.desktop.peripherals.mouse',):
            host_run(['gsettings', 'set', schema, 'accel-profile', 'flat'])
            host_run(['gsettings', 'set', schema, 'speed', '0'])
        return 'gsettings'

    def xrandr(self, walker, *args):
        walker.log.write({'op': 'sh', 'a': ['xrandr'] + list(args)})
        return host_run(['xrandr'] + list(args), 15)

    def modes(self, name):
        rc, out, _ = host_run(['xrandr'], 10)
        found, on = [], False
        for line in out.splitlines():
            if not line.startswith(' '):
                on = line.startswith(name + ' ')
            elif on:
                found.append(line.split()[0])
        return found

    def churn_outputs(self, walker):
        rng = walker.rng
        names = [o['name'] for o in walker.w.outputs()]
        choice = rng.randint(0, 3)
        if choice == 0 and 'Virtual-2' in names or (choice == 1 and 'Virtual-2' not in names):
            self.xrandr(walker, '--output', 'Virtual-2', '--off')
        elif choice == 1:
            self.xrandr(walker, '--output', 'Virtual-2', '--auto', '--right-of', 'Virtual-1')
        elif choice == 2:
            modes = self.modes('Virtual-1')
            if modes:
                self.xrandr(walker, '--output', 'Virtual-1', '--mode', rng.choice(modes))
        else:
            self.xrandr(walker, '--output', 'Virtual-2', '--auto', '--right-of', 'Virtual-1')
            modes = self.modes('Virtual-2')
            if modes:
                self.xrandr(walker, '--output', 'Virtual-2', '--mode', rng.choice(modes))
        walker.a.sleep(1.5)

    def restore_outputs(self, walker):
        outs = walker.w.outputs()
        if len(outs) < 2:
            self.xrandr(walker, '--output', 'Virtual-2', '--auto', '--right-of', 'Virtual-1')
            walker.a.sleep(1.5)

    def set_clipboard(self, walker, kind, payload):
        walker.log.write({'op': 'note', 'action': 'xclip ' + kind})
        mime = {'text': 'UTF8_STRING', 'image': 'image/png', 'uris': 'text/uri-list'}.get(kind)
        try:
            if kind == 'clear':
                subprocess.run(['xclip', '-selection', 'clipboard', '-i', '/dev/null'], timeout=10, env=host_env(), capture_output=True)
            else:
                subprocess.run(['xclip', '-selection', 'clipboard', '-t', mime, '-i'], input=payload, timeout=10, env=host_env(), capture_output=True)
        except (OSError, subprocess.SubprocessError) as error:
            walker.log.write({'op': 'harness_error', 'action': 'clipboard', 'error': repr(error)[:160]})


DESKTOPS = {'kde': Kde, 'gnome': Gnome, 'cinnamon': Cinnamon}


def detect():
    current = os.environ.get('XDG_CURRENT_DESKTOP', '').upper()
    if 'GNOME' in current:
        return 'gnome'
    if 'CINNAMON' in current:
        return 'cinnamon'
    if 'KDE' in current:
        return 'kde'
    return 'kde'


def make(name=None):
    name = name or detect()
    return DESKTOPS[name]()
