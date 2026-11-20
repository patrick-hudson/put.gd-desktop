"""The Hyprland IPC and labwc pixel witnesses against recorded replies and synthetic frames."""
import random
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe  # noqa: E402

CLIENT = dict(mapped=True, hidden=False, floating=False, fullscreen=0, focusHistoryID=1, monitor=0, stableId='5a1')
MONITORS = [{'id': 0, 'name': 'Virtual-1', 'activeWorkspace': {'id': 1}, 'specialWorkspace': {'id': 0}}]
# Omarchy 4.5's own layer surfaces: the bar and the notification column span the output.
LAYERS = {'Virtual-1': {'levels': {
    '0': [{'address': '0xb0', 'x': 0, 'y': 0, 'w': 1280, 'h': 800, 'namespace': 'omarchy-background', 'pid': 905}],
    '1': [],
    '2': [{'address': '0xb2', 'x': 0, 'y': 0, 'w': 1280, 'h': 800, 'namespace': 'omarchy-bar', 'pid': 905}],
    '3': [{'address': '0xb3', 'x': 881, 'y': 0, 'w': 399, 'h': 800, 'namespace': 'omarchy-notifications', 'pid': 905}]}}}


def client(address, title, at, size, **extra):
    return dict(CLIENT, address=address, title=title, at=at, size=size, pid=extra.pop('pid', 100),
                **{'class': extra.pop('wm_class', 'putgd-ui'), 'workspace': {'id': extra.pop('workspace', 1)}}, **extra)


class HyprlandWitness(unittest.TestCase):
    def document(self, clients, active='0xa1', layers=LAYERS):
        return probe.hyprland_parse(clients, MONITORS, layers, {'address': active})

    def test_parse_keeps_geometry_pid_focus_and_hides_other_workspaces(self):
        windows = self.document([client('0xa1', 'put.gd', [10, 20], [300, 200]),
                                 client('0xa2', 'away', [0, 0], [50, 50], workspace=3)])['windows']
        main = next(w for w in windows if w['id'] == '0xa1')
        self.assertEqual((main['frame'], main['content'], main['pid'], main['focus']),
                         ([10, 20, 300, 200], [10, 20, 300, 200], 100, True))
        self.assertTrue(next(w for w in windows if w['id'] == '0xa2')['minimized'])
        bar = next(w for w in windows if w['id'] == '0xb2')
        self.assertEqual((bar['layer'], bar['wm_class'], bar['title']), ('top', 'omarchy-bar', ''))

    def test_drawing_order_layers_floating_fullscreen(self):
        clients = [client('0xa1', 'tiled', [0, 0], [600, 600], focusHistoryID=0),
                   client('0xa2', 'old float', [100, 100], [200, 200], floating=True, focusHistoryID=3),
                   client('0xa3', 'new float', [150, 150], [200, 200], floating=True, focusHistoryID=1)]
        document = self.document(clients, layers={})
        self.assertEqual(probe.hyprland_pick((160, 160), document)['window']['id'], '0xa3')
        self.assertEqual(probe.hyprland_pick((110, 110), document)['window']['id'], '0xa2')
        self.assertEqual(probe.hyprland_pick((10, 10), document)['window']['id'], '0xa1')
        self.assertIsNone(probe.hyprland_pick((900, 700), document)['window'])
        full = self.document(clients + [client('0xa4', 'cover', [0, 0], [1280, 800], fullscreen=2)], layers={})
        self.assertEqual(probe.hyprland_pick((160, 160), full)['window']['id'], '0xa4')

    def test_spanning_shell_layers_are_on_top_and_background_is_desktop(self):
        document = self.document([client('0xa1', 'put.gd', [0, 0], [600, 600])])
        self.assertEqual(probe.hyprland_pick((10, 10), document)['window']['wm_class'], 'omarchy-bar')
        self.assertEqual(probe.hyprland_pick((900, 10), document)['window']['wm_class'], 'omarchy-notifications')
        empty = self.document([], layers={'Virtual-1': {'levels': {'0': LAYERS['Virtual-1']['levels']['0']}}})
        self.assertIsNone(probe.hyprland_pick((10, 10), empty)['window'])

    def test_shell_layer_over_target_is_decided_by_the_targets_pixels(self):
        document = self.document([client('0xa1', 'put.gd', [0, 0], [600, 600])])
        target = {'window_id': '0xa1', 'pid': 100}
        window = (600, 600, bytes(600 * 600 * 3))
        with patch.object(probe, 'hyprland_windows', return_value=document), \
             patch.object(probe, 'owned', return_value=False), \
             patch.object(probe, 'grim', side_effect=lambda i=None: window if i else (1280, 800, bytes(1280 * 800 * 3))):
            self.assertTrue(probe.hyprland_foreground(target, (10, 10))['ok'])
        # A card over the point only: the rest of the window is its own pixels.
        card = bytearray(1280 * 800 * 3)
        for row in range(0, 40):
            card[row * 1280 * 3:(row * 1280 + 40) * 3] = b'\x01' * 120
        with patch.object(probe, 'hyprland_windows', return_value=document), \
             patch.object(probe, 'owned', return_value=False), \
             patch.object(probe, 'time') as clock, \
             patch.object(probe, 'grim', side_effect=lambda i=None: window if i else (1280, 800, bytes(card))):
            clock.sleep = lambda _: None
            result = probe.hyprland_foreground(target, (10, 10))
            self.assertFalse(result['ok'])
            self.assertIn('omarchy-bar', result['obscuring_window'])
        # A see-through window never matches: decided by the reserved strip and the card count.
        for reserved, cards, ok in ((False, 0, True), (True, 0, False), (False, 2, False), (False, None, False)):
            with self.subTest(reserved=reserved, cards=cards), \
                 patch.object(probe, 'hyprland_windows', return_value=document), \
                 patch.object(probe, 'owned', return_value=False), \
                 patch.object(probe, 'time') as clock, \
                 patch.object(probe, 'hyprland_reserved', return_value=reserved), \
                 patch.object(probe, 'quickshell_cards', return_value=cards), \
                 patch.object(probe, 'grim', side_effect=lambda i=None: window if i else (1280, 800, b'\x01' * (1280 * 800 * 3))):
                clock.sleep = lambda _: None
                self.assertEqual(probe.hyprland_foreground(target, (10, 10))['ok'], ok)

    def test_another_window_on_top_is_named_without_pixels(self):
        document = self.document([client('0xa1', 'put.gd', [0, 0], [600, 600]),
                                  client('0xa2', 'Firefox', [0, 0], [300, 300], floating=True, wm_class='firefox')],
                                 layers={})
        with patch.object(probe, 'hyprland_windows', return_value=document), \
             patch.object(probe, 'grim', side_effect=AssertionError('no capture needed')):
            result = probe.hyprland_foreground({'window_id': '0xa1', 'pid': 100}, (10, 10))
        self.assertEqual((result['ok'], result['obscuring_window']), (False, 'Firefox'))

    def test_unknown_pids_and_shell_layers_are_never_signalled(self):
        clients = [client('0xa1', 'fixture', [0, 0], [1280, 800], fullscreen=2, pid=-1),
                   client('0xa2', 'nag', [10, 10], [300, 200], floating=True, pid=4321, wm_class='nag')]
        layers = {'Virtual-1': {'levels': {'3': [{'address': '0xb3', 'x': 0, 'y': 0, 'w': 1280, 'h': 800,
                                                  'namespace': 'wallpaper', 'pid': -1}]}}}
        document = self.document(clients, layers=layers)
        self.assertEqual([w['pid'] for w in document['windows']], [4321, None, None])
        sent, starts = [], iter(['777'] * 2 + [None] * 50)
        with patch.object(probe, 'hyprland_windows', return_value=document), \
             patch.object(probe, 'close_notifications', return_value=1), \
             patch.object(probe, 'owned', return_value=False), \
             patch.object(probe, 'process_name', side_effect=lambda pid: {4321: 'nag'}.get(pid)), \
             patch.object(probe, 'process_start', side_effect=lambda pid: next(starts)), \
             patch.object(probe.os, 'pidfd_open', side_effect=lambda pid: 100 + pid), \
             patch.object(probe.os, 'close'), \
             patch.object(probe.signal, 'pidfd_send_signal', side_effect=lambda fd, number: sent.append((fd - 100, number))), \
             patch.object(probe.os, 'kill', side_effect=AssertionError('kill by pid')):
            result = probe.clear_screen({'compositor': {'kind': 'hyprctl'}, 'above': '0xa1', 'obscuring': '0xa2', 'shell': []})
        # Only the named nag process, through its own descriptor; KILL to an exited process is a no-op.
        self.assertEqual(sent, [(4321, probe.signal.SIGTERM), (4321, probe.signal.SIGKILL)])
        self.assertEqual(result['stopped'], [{'title': 'nag', 'process': 'nag'}])
        with patch.object(probe.os, 'pidfd_open', side_effect=AssertionError('opened')):
            probe.stop_windows([{'pid': p} for p in (-1, 0, 1, None, True)])

    def test_signals_go_only_to_the_process_whose_start_was_read(self):
        sent = []
        starts = {4321: iter(['777', '999'])}  # the pid was reused between the read and the descriptor
        with patch.object(probe, 'process_start', side_effect=lambda pid: next(starts[pid])), \
             patch.object(probe.os, 'pidfd_open', return_value=104), patch.object(probe.os, 'close') as close, \
             patch.object(probe.signal, 'pidfd_send_signal', side_effect=lambda fd, number: sent.append(number)):
            probe.stop_windows([{'pid': 4321}])
        self.assertEqual(sent, [])
        close.assert_called_once_with(104)

    def test_activate_refuses_a_malformed_address(self):
        with self.assertRaises(RuntimeError):
            probe.hyprland_activate('0xa1; reload')


def noise(width, height, seed):
    generator = random.Random(seed)
    return bytes(generator.randrange(256) for _ in range(width * height * 3))


def paste(screen, window, x, y):
    sw, sh, sb = screen
    ww, wh, wb = window
    data = bytearray(sb)
    for r in range(wh):
        data[((y + r) * sw + x) * 3:((y + r) * sw + x + ww) * 3] = wb[r * ww * 3:(r + 1) * ww * 3]
    return sw, sh, bytes(data)


class PixelWitness(unittest.TestCase):
    def setUp(self):
        self.window = (80, 50, noise(80, 50, 1))
        self.screen = paste((320, 200, bytes(320 * 200 * 3)), self.window, 113, 47)

    def test_ppm(self):
        self.assertEqual(probe.parse_ppm(b'P6\n2 1\n255\n' + b'\x01' * 6), (2, 1, b'\x01' * 6))
        with self.assertRaises(RuntimeError):
            probe.parse_ppm(b'P6\n2 1\n255\n' + b'\x01' * 5)

    def test_window_is_found_exactly_where_its_pixels_are(self):
        self.assertEqual(probe.locate_pixels(self.window, self.screen), {'rect': [113, 47, 80, 50], 'visible': 1.0})

    def test_partly_covered_window_still_found_and_hit_test_follows_the_cover(self):
        sw, sh, sb = paste(self.screen, (40, 50, b'\xff' * (40 * 50 * 3)), 113, 47)
        located = probe.locate_pixels(self.window, (sw, sh, sb))
        self.assertEqual(located['rect'], [113, 47, 80, 50])
        self.assertLess(located['visible'], 1.0)
        self.assertFalse(probe.pixels_at(self.window, (sw, sh, sb), located['rect'], (120, 60)))
        self.assertTrue(probe.pixels_at(self.window, (sw, sh, sb), located['rect'], (180, 80)))

    def test_a_shadowed_capture_keeps_only_the_exact_content_box(self):
        shadowed = paste((120, 90, bytes(120 * 90 * 3)), self.window, 17, 13)
        self.assertEqual(probe.content_box(shadowed, 80, 50), self.window)
        self.assertIsNone(probe.content_box(shadowed, 82, 50))
        self.assertIsNone(probe.content_box((120, 90, bytes(120 * 90 * 3)), 80, 50))

    def test_a_dialog_is_the_one_change_over_its_parent(self):
        dialog = paste(self.screen, (30, 20, b'\x07' * (30 * 20 * 3)), 140, 60)
        self.assertEqual(probe.difference_box(self.window, dialog, [113, 47, 80, 50]), [140, 60, 30, 20])
        self.assertIsNone(probe.difference_box(self.window, self.screen, [113, 47, 80, 50]))
        edge = paste(self.screen, (30, 20, b'\x07' * (30 * 20 * 3)), 180, 80)  # partly past the parent
        self.assertEqual(probe.difference_box(self.window, edge, [113, 47, 80, 50]), [180, 80, 13, 17])

    def test_a_cursor_at_the_point_is_not_a_cover(self):
        rect = [113, 47, 80, 50]
        self.assertTrue(probe.pixels_at(self.window, self.screen, rect, (150, 70)))
        # Hyprland's software cursor at the point is not a cover; in a cursor-free capture
        # (grim's default, labwc) the same pixels are one, and so is a corner at the point.
        cursor = paste(self.screen, (12, 20, b'\x10' * (12 * 20 * 3)), 150, 70)
        self.assertTrue(probe.pixels_at(self.window, cursor, rect, (150, 70), cursor=True))
        self.assertFalse(probe.pixels_at(self.window, cursor, rect, (150, 70)))
        corner = paste(self.screen, (2, 2, b'\x10' * (2 * 2 * 3)), 150, 70)
        self.assertFalse(probe.pixels_at(self.window, corner, rect, (150, 70)))
        card = paste(self.screen, (30, 30, b'\x10' * (30 * 30 * 3)), 140, 60)
        self.assertFalse(probe.pixels_at(self.window, card, rect, (150, 70), cursor=True))
        self.assertFalse(probe.pixels_at(self.window, self.screen, rect, (400, 70)))

    def test_flat_missing_and_repeated_windows_are_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'no distinctive pixels'):
            probe.locate_pixels((80, 50, bytes(80 * 50 * 3)), self.screen)
        with self.assertRaisesRegex(RuntimeError, 'no distinctive pixels|not found'):
            probe.locate_pixels((80, 50, noise(80, 50, 2)), self.screen)
        twice = paste(self.screen, self.window, 0, 140)
        with self.assertRaisesRegex(RuntimeError, 'ambiguous'):
            probe.locate_pixels(self.window, twice)
        with self.assertRaisesRegex(RuntimeError, 'does not fit'):
            probe.locate_pixels((400, 10, noise(400, 10, 3)), self.screen)

    def test_app_id_must_name_the_process(self):
        with patch.object(probe, 'executable', return_value=['dolphin']):
            self.assertTrue(probe.app_id_names('org.kde.dolphin', 1))
            self.assertFalse(probe.app_id_names('firefox', 1))
            self.assertFalse(probe.app_id_names('', 1))
        import os
        import subprocess
        kiosk = subprocess.Popen(['sleep', '30'], env=dict(os.environ, MOZ_APP_REMOTINGNAME='putgdsurfacexyz'))
        try:
            self.assertTrue(probe.app_id_names('putgdsurfacexyz', kiosk.pid))
            self.assertFalse(probe.app_id_names('putgdsurfaceother', kiosk.pid))
        finally:
            kiosk.kill()
            kiosk.wait()

    def test_output_rectangles_are_logical(self):
        outputs = [{'name': 'Virtual-1', 'enabled': True, 'position': {'x': 0, 'y': 0}, 'scale': 2.0, 'transform': 'normal',
                    'modes': [{'width': 2560, 'height': 1600, 'current': True}]},
                   {'name': 'Virtual-2', 'enabled': True, 'position': {'x': 1280, 'y': 0}, 'scale': 1.0, 'transform': '90',
                    'modes': [{'width': 1024, 'height': 768, 'current': True}]},
                   {'name': 'off', 'enabled': False, 'position': {'x': 0, 'y': 0}, 'modes': []}]
        self.assertEqual(probe.wlr_output_rects(outputs), [[0, 0, 1280, 800], [1280, 0, 768, 1024]])

    def test_selector_target_refuses_points_outside_and_showing_shell_overlays(self):
        target = {'window_id': 'layer:7:Capture region', 'x': 0, 'y': 0, 'width': 320, 'height': 200}
        compositor = {'kind': 'toplevel-pixels', 'overlay_shells': ['budgie-daemon']}
        with patch.object(probe, 'shell_overlays_showing', return_value=[]):
            self.assertTrue(probe.toplevel_pixels_foreground(compositor, target, (10, 10))['ok'])
            self.assertFalse(probe.toplevel_pixels_foreground(compositor, target, (400, 10))['ok'])
        with patch.object(probe, 'shell_overlays_showing', return_value=['budgie-daemon']):
            self.assertFalse(probe.toplevel_pixels_foreground(compositor, target, (10, 10))['ok'])



class DesktopSpecificMime(unittest.TestCase):
    def test_existing_desktop_list_gets_the_handler_and_keeps_the_rest(self):
        import tempfile
        from tools.lab.surface_driver import browser
        with tempfile.TemporaryDirectory() as root, patch.dict('os.environ', XDG_CURRENT_DESKTOP='Hyprland'):
            listed = Path(root) / 'hyprland-mimeapps.list'
            listed.write_text('[Default Applications]\nx-scheme-handler/http=chromium.desktop\ntext/plain=nvim.desktop\n'
                              '[Added Associations]\nimage/png=imv.desktop\n')
            browser._desktop_specific_default(root, 'x-scheme-handler/http', 'putgd-lab.desktop')
            browser._desktop_specific_default(root, 'x-scheme-handler/https', 'putgd-lab.desktop')
            self.assertEqual(listed.read_text(), '[Default Applications]\nx-scheme-handler/http=putgd-lab.desktop\n'
                             'text/plain=nvim.desktop\nx-scheme-handler/https=putgd-lab.desktop\n'
                             '[Added Associations]\nimage/png=imv.desktop\n')
            browser._desktop_specific_default(root, 'x-scheme-handler/http', 'other.desktop')
            self.assertFalse((Path(root) / 'mimeapps.list').exists())


if __name__ == '__main__':
    unittest.main()
