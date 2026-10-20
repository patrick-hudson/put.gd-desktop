"""Pause A/V driver contracts: the split at the pause and the step/shift arithmetic; no desktop or build is used."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import av_pause_run as driver

TIMES = [0.5 + i for i in range(16)]  # flashes once a second; the pause sits at 8.0 s of clip time


def offsets(before, after):
    return [before if t < 8.0 else after for t in TIMES]


class Step(unittest.TestCase):
    def test_flat_sync_has_no_step(self):
        self.assertEqual(driver.capture_step(TIMES, offsets(20.0, 20.0), 8.0), 0.0)

    def test_a_real_step_is_reported(self):
        self.assertEqual(driver.capture_step(TIMES, offsets(20.0, 32.0), 8.0), 12.0)
        self.assertEqual(driver.capture_step(TIMES, offsets(20.0, 9.4), 8.0), -10.6)

    def test_raw_step_keeps_whole_frames(self):
        self.assertEqual(driver.capture_step(TIMES, offsets(20.0, 37.0), 8.0, raw=True), 17.0)
        # Past half a frame the nearest whole frame wins: 17 ms reads as -16.3.
        self.assertEqual(driver.capture_step(TIMES, offsets(20.0, 37.0), 8.0), -16.3)

    def test_whole_frames_are_removed(self):
        # The 30 fps marker and capture can slip by exactly one frame anywhere.
        self.assertEqual(driver.capture_step(TIMES, offsets(20.0, 53.3), 8.0), 0.0)
        self.assertEqual(driver.capture_step(TIMES, offsets(20.0, -13.0), 8.0), 0.3)

    def test_pairs_next_to_the_pause_are_ignored(self):
        times = TIMES + [7.9, 8.1]
        values = offsets(20.0, 20.0) + [500.0, -500.0]
        order = sorted(range(len(times)), key=times.__getitem__)
        self.assertEqual(driver.capture_step([times[i] for i in order], [values[i] for i in order], 8.0), 0.0)

    def test_too_few_pairs_give_no_step(self):
        self.assertIsNone(driver.capture_step([1.0, 9.0, 10.0, 11.0], [0, 0, 0, 0], 8.0))
        self.assertIsNone(driver.capture_step([1.0, 2.0, 9.0, 10.0], [0, 0, 0, 0], 8.0))


class ExportShift(unittest.TestCase):
    def test_shift_after_the_pause(self):
        working = {'pairTimesS': TIMES, 'offsetsMs': offsets(20.0, 21.0)}
        exported = {'pairTimesS': TIMES, 'offsetsMs': offsets(20.0, 11.0)}
        self.assertEqual(driver.export_shift(working, exported, 8.0), -10.0)

    def test_no_pairs_after_the_pause(self):
        working = {'pairTimesS': [1.0, 2.0], 'offsetsMs': [0.0, 0.0]}
        self.assertIsNone(driver.export_shift(working, working, 8.0))


if __name__ == '__main__':
    unittest.main()
