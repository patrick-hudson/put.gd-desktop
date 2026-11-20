"""The Hyprland IPC and labwc pixel witnesses against recorded replies and synthetic frames."""
import random
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
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

    def test_a_window_rule_position_must_show_the_window(self):
        rules = {'window_rules': [{'app_id': 'putgd-ui', 'title': 'Delete', 'position': [113, 47]}]}
        placed = probe.place_pixels(rules, 'Delete', 'putgd-ui', self.window, self.screen)
        self.assertEqual((placed['rect'], placed['by']), ([113, 47, 80, 50], 'window rule'))
        moved = {'window_rules': [{'app_id': 'putgd-ui', 'title': 'Delete', 'position': [10, 10]}]}
        with self.assertRaisesRegex(RuntimeError, 'window-rule position'):
            probe.place_pixels(moved, 'Delete', 'putgd-ui', self.window, self.screen)
        # A rule for another app or title does not apply; without one a dialog is refused.
        self.assertEqual(probe.place_pixels(moved, 'Delete', 'other', self.window, self.screen)['by'], 'pixels')
        with self.assertRaisesRegex(RuntimeError, 'no lab window rule'):
            probe.place_pixels({}, 'Delete', 'putgd-ui', self.window, self.screen, rule_only=True)

    def test_a_flat_window_is_placed_only_where_it_fills_the_one_output(self):
        flat = (320, 200, b'\x20' * (320 * 200 * 3))
        with patch.object(probe, 'wlr_outputs', return_value=[[0, 0, 320, 200]]):
            with self.assertRaisesRegex(RuntimeError, 'no distinctive pixels.*nor does it fill the one output'):
                probe.place_pixels({}, 'Mozilla Firefox', 'kiosk', flat, self.screen, fullscreen=True)
            # Only an active fullscreen toplevel: flat pixels alone prove no position.
            with self.assertRaisesRegex(RuntimeError, 'no distinctive pixels'):
                probe.place_pixels({}, 'Mozilla Firefox', 'kiosk', flat, flat)
            placed = probe.place_pixels({}, 'Mozilla Firefox', 'kiosk', flat, flat, fullscreen=True)
        self.assertEqual((placed['rect'], placed['by']), ([0, 0, 320, 200], 'output'))
        # A second enabled output, even of another size, leaves the fallback unproven.
        with patch.object(probe, 'wlr_outputs', return_value=[[0, 0, 320, 200], [320, 0, 640, 480]]):
            with self.assertRaisesRegex(RuntimeError, r'nor does it fill the one output \(2 enabled'):
                probe.place_pixels({}, 'Mozilla Firefox', 'kiosk', flat, flat, fullscreen=True)

    def test_the_display_crop_falls_back_to_grim_when_vnc_or_the_request_limit_fails(self):
        from tools.lab.surface_driver.context import DriverContext
        notes = []
        ctx = SimpleNamespace(profile={'compositor': {'kind': 'toplevel-pixels'}, 'screens': [{}]}, note=notes.append,
                              DISPLAY_BYTES=DriverContext.DISPLAY_BYTES, target=SimpleNamespace(physical=None))
        query = {'geometry': {'x': 0, 'y': 0, 'width': 4, 'height': 4, 'window_id': 'w'}, 'point': [1, 1]}
        ctx.target.display_image = lambda head: None
        self.assertEqual(DriverContext._with_display(ctx, query), query)
        self.assertEqual(len(notes), 1)
        # Only the VNC read itself falls back: a failed endpoint or lease check still stops the run.
        from tools.lab.surface_driver import target
        from tools.lab.surface_driver.api import RecipeBlocked
        for error in (TimeoutError('frame'), RuntimeError('connection closed'), ConnectionRefusedError()):
            vnc = SimpleNamespace(physical=None, info={'vnc_display': 20}, heartbeat=lambda force: None)
            with patch.object(target.rfb, 'RFB', side_effect=error):
                self.assertIsNone(target.Target.display_image(vnc))
        def refused():
            raise RecipeBlocked('QMP endpoint changed')
        vnc = SimpleNamespace(physical=None, info={'vnc_display': 20}, heartbeat=lambda force: None, _verify_endpoints=refused)
        with patch.object(target.rfb, 'RFB') as connect, self.assertRaisesRegex(RecipeBlocked, 'QMP endpoint changed'):
            target.Target.display_image(vnc)
        connect.assert_not_called()
        def lease(force):
            raise OSError('lease')
        vnc = SimpleNamespace(physical=None, info={'vnc_display': 20}, heartbeat=lease)
        with patch.object(target.rfb, 'RFB'), self.assertRaises(RecipeBlocked):
            target.Target.display_image(vnc)
        # The whole serialized request is measured: a crop that fits alone but not with the rest is dropped.
        sent = {}
        def probe_(name, q, source, timeout):
            sent.clear()
            sent.update(q)
            return {'ok': True, 'result': {}}
        ctx = SimpleNamespace(profile={}, note=notes.append, _check_time=lambda: None, _deadline=float('inf'),
                              PROBE_REQUEST_BYTES=DriverContext.PROBE_REQUEST_BYTES, _observed=lambda value: None,
                              target=SimpleNamespace(probe=probe_))
        DriverContext._probe(ctx, 'foreground', dict(query, display={'rgb': 'x' * (1024 * 1024)}))
        self.assertNotIn('display', sent)
        DriverContext._probe(ctx, 'foreground', dict(query, display={'rgb': 'x' * 1024}))
        self.assertIn('display', sent)

    def test_a_placement_retry_places_only_a_fresh_capture_of_the_window_size(self):
        first, odd, fresh = (80, 40, b'\x01' * 9600), (79, 40, b'\x02' * 9480), (80, 40, b'\x03' * 9600)
        captures = iter([first, odd, fresh])
        placed = []
        def place(compositor, title, app_id, image, screen, fullscreen=False):
            placed.append(image)
            if len(placed) == 1:
                raise RuntimeError('still covered')
            return {'rect': [5, 6, 80, 40]}
        placement = probe.WaylandPlacement({'kind': 'toplevel-pixels'})
        placement._windows = [dict(id='k', title='Kiosk', wm_class='kiosk', focus=True)]
        with patch.object(probe, 'grim', side_effect=lambda *a: next(captures) if a else (8, 8, b'')), \
             patch.object(probe, 'app_id_names', return_value=True), patch.object(probe, 'place_pixels', side_effect=place), \
             patch.object(probe, 'compositor_windows', return_value={'windows': placement._windows}), \
             patch.object(probe.time, 'sleep'):
            self.assertEqual(placement._pixel_toplevel(7, 'Kiosk', [0, 0, 80, 40])['frame'], [5, 6, 80, 40])
        self.assertEqual(placed, [first, fresh])  # the 79x40 capture placed nothing

    def test_the_toplevel_list_reports_fullscreen_only_for_a_window_it_names_once(self):
        module = SimpleNamespace(socket_path=lambda: 'x', snapshot=lambda _: {
            'active': {'identifier': 'k'},
            'ext': [{'identifier': 'k', 'app_id': 'firefox', 'title': 'Kiosk'},
                    {'identifier': 'a', 'app_id': 'putgd-ui', 'title': 'History'},
                    {'identifier': 'b', 'app_id': 'putgd-ui', 'title': 'History'}],
            'wlr': [{'app_id': 'firefox', 'title': 'Kiosk', 'fullscreen': True},
                    {'app_id': 'putgd-ui', 'title': 'History', 'fullscreen': True},
                    {'app_id': 'putgd-ui', 'title': 'History', 'fullscreen': False}]})
        with patch.object(probe, 'toplevels_module', return_value=module):
            windows = {w['id']: w for w in probe.toplevel_pixel_windows({})['windows']}
        self.assertEqual([(i, windows[i]['fullscreen'], windows[i]['focus']) for i in 'kab'],
                         [('k', True, True), ('a', False, False), ('b', False, False)])

    def test_a_dialog_shows_its_pixels_in_its_parents_capture_corner(self):
        # labwc draws the dialog into its parent's capture at the origin and captures it alone as 1x1.
        dialog = (30, 20, noise(30, 20, 5))
        screen = paste(self.screen, dialog, 140, 60)
        parent = paste(self.window, dialog, 0, 0)
        windows = {'windows': [{'id': 'p', 'wm_class': 'putgd-ui'}, {'id': 'd', 'wm_class': 'putgd-ui'},
                               {'id': 'o', 'wm_class': 'other'}]}
        captures = {'p': parent, 'd': (1, 1, b'\0\0\0'), 'o': self.window}
        rules = {'window_rules': [{'app_id': 'putgd-ui', 'title': 'Delete', 'position': [140, 60]}]}
        with patch.object(probe, 'toplevel_pixel_windows', return_value=windows), \
             patch.object(probe, 'grim', side_effect=lambda identifier=None: captures[identifier]), \
             patch.object(probe, 'app_id_names', side_effect=lambda app_id, pid: app_id == 'putgd-ui'):
            parent_id, image, rect = probe.dialog_place(rules, 7, 'Delete', windows['windows'][1], (30, 20), screen)
            self.assertEqual((parent_id, image, rect), ('p', dialog, [140, 60, 30, 20]))
            with self.assertRaisesRegex(RuntimeError, '0 of its process'):
                probe.dialog_place(rules, 7, 'Delete', windows['windows'][1], (30, 20), self.screen)
            with self.assertRaisesRegex(RuntimeError, 'no lab window rule'):
                probe.dialog_place({}, 7, 'Delete', windows['windows'][1], (30, 20), screen)
            # A flat corner is no dialog, even where the outputs show the same flat colour.
            captures['p'] = paste(self.window, (30, 20, b'\x07' * (30 * 20 * 3)), 0, 0)
            flat = paste(self.screen, (30, 20, b'\x07' * (30 * 20 * 3)), 140, 60)
            with self.assertRaisesRegex(RuntimeError, 'detail in the corner of p does not show'):
                probe.dialog_place(rules, 7, 'Delete', windows['windows'][1], (30, 20), flat)
            # Mostly flat with a little detail: the flat cells alone matching is not enough.
            spot = paste((30, 20, b'\x07' * (30 * 20 * 3)), (6, 2, noise(6, 2, 9)), 12, 8)
            captures['p'] = paste(self.window, spot, 0, 0)
            self.assertGreaterEqual(probe.pixel_share(spot, flat, 140, 60), probe.KNOWN_SHARE)
            with self.assertRaisesRegex(RuntimeError, 'detail in the corner of p does not show'):
                probe.dialog_place(rules, 7, 'Delete', windows['windows'][1], (30, 20), flat)

    def test_short_flat_cells_are_no_detail(self):
        # 400 wide: the last cell of each row is 16 pixels, too short for compression to call flat.
        flat = (400, 200, b'\x30' * (400 * 200 * 3))
        self.assertEqual(probe.pixel_share(flat, flat, 0, 0, detailed=True), 0.0)

    def test_the_display_snapshot_stands_for_the_outputs_over_the_target(self):
        import base64
        import zlib
        crop = probe.crop(self.screen, 113, 47, 80, 50)
        display = {'size': [320, 200], 'rect': [113, 47, 80, 50], 'rgb': base64.b64encode(zlib.compress(crop[2])).decode()}
        screen = probe.display_screen(display)
        self.assertEqual(probe.crop(screen, 113, 47, 80, 50), self.window)
        self.assertTrue(probe.pixels_at(self.window, screen, [113, 47, 80, 50], (150, 70)))
        with self.assertRaisesRegex(RuntimeError, 'does not match'):
            probe.display_screen(dict(display, rect=[300, 47, 80, 50]))

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
