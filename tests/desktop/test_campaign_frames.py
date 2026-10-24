#!/usr/bin/env python3
"""Whole-frame privacy negatives; exact bytes delivered to image savers checked."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'performance'))
import campaign_frames as frames
import physical_campaign as campaign
from bench_ui_process import EvidenceError


def frame(width, height):
    return bytearray(bytes((100, 90, 80, 0)) * (width * height))


def notification(raw, width, x, y, w, h):
    for row in range(y, y + h):
        begin = (row * width + x) * 4
        raw[begin:begin + w * 4] = bytes((33, 77, 211, 0)) * w


class FramePrivacyTests(unittest.TestCase):
    def test_99_percent_cover_with_ten_thousand_notification_pixels_never_saves(self):
        raw = frame(1000, 1000)
        notification(raw, 1000, 100, 100, 100, 100)
        saver = mock.Mock()
        with self.assertRaisesRegex(EvidenceError, 'unattributed_pixels'):
            frames.save_cover('not-written.png', 1000, 1000, bytes(raw), saver=saver)
        saver.assert_not_called()

    def test_small_panel_is_not_a_tolerated_percentage(self):
        raw = frame(100, 100)
        notification(raw, 100, 0, 0, 100, 1)
        saver = mock.Mock()
        with self.assertRaisesRegex(EvidenceError, 'unattributed_pixels'):
            frames.save_cover('not-written.png', 100, 100, bytes(raw), saver=saver)
        saver.assert_not_called()

    def test_cover_encoder_receives_only_exact_verified_rgb(self):
        saver = mock.Mock()
        frames.save_cover('cover.png', 80, 60, bytes(frame(80, 60)), saver=saver)
        saver.assert_called_once_with('cover.png', 80, 60, frames.BASE * (80 * 60))

    def selector(self):
        raw = frame(100, 100)
        rgba = bytearray(100 * 100 * 4)
        expected = bytearray(frames.BASE * (100 * 100))
        for y, value in ((20, 0), (21, 255)):
            for x in range(10, 90):
                index = y * 100 + x
                rgba[index * 4:index * 4 + 4] = bytes((value, value, value, 255))
                raw[index * 4:index * 4 + 4] = bytes((value, value, value, 0))
                expected[index * 3:index * 3 + 3] = bytes((value, value, value))
        return raw, [{'x': 0, 'y': 0, 'width': 100, 'height': 100, 'rgba': bytes(rgba)}], bytes(expected)

    def test_capture_encoder_never_receives_unattributed_notification_or_panel_bytes(self):
        raw, overlays, expected = self.selector()
        notification(raw, 100, 0, 0, 100, 1)
        notification(raw, 100, 60, 60, 10, 10)
        saver = mock.Mock()
        proof = frames.save_selector('selector.png', 100, 100, bytes(raw), overlays, None, saver=saver)
        saver.assert_called_once_with('selector.png', 100, 100, expected)
        self.assertTrue(proof['allOtherPixelsReplacedWithSyntheticBackground'])
        self.assertFalse(proof['rawFramebufferRetained'])

    def test_notification_obscuring_verified_selector_refuses_save(self):
        raw, overlays, _ = self.selector()
        notification(raw, 100, 30, 20, 10, 2)
        saver = mock.Mock()
        with self.assertRaisesRegex(EvidenceError, 'selector_pixels_occluded'):
            frames.save_selector('not-written.png', 100, 100, bytes(raw), overlays, None, saver=saver)
        saver.assert_not_called()

    def test_capture_failure_path_does_not_persist_a_99_percent_cover(self):
        raw = frame(1000, 1000)
        notification(raw, 1000, 0, 0, 1000, 10)
        client = SimpleNamespace(frame=lambda: (1000, 1000, bytes(raw)), close=lambda: None)
        with tempfile.TemporaryDirectory() as directory:
            run = campaign.Campaign.__new__(campaign.Campaign)
            run.root = Path(directory)
            run.family = run.resident = None
            run.cover = mock.Mock()
            run.cover.poll.return_value = None
            run.args = SimpleNamespace(vnc_port=5900)
            with mock.patch.dict(sys.modules, {'rfb': SimpleNamespace(RFB=lambda port: client)}), \
                 mock.patch.object(frames, 'save_rgb') as saver:
                with self.assertRaisesRegex(EvidenceError, 'unattributed_pixels'):
                    run.visual('failed-workload')
                saver.assert_not_called()
            self.assertEqual(list(run.root.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
