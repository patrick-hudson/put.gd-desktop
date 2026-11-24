"""cover_player.py's parent watch (#1925); no display or GTK is needed."""
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import cover_player

# A player-shaped child: it records its parent first, as play() does, runs only the real watch
# (polled every 100 ms) and keeps stdin open, like a player whose driver never wrote `quit`.
PLAYER = '''import os, sys, time
parent = os.getppid()
sys.path.insert(0, %r)
import cover_player
done, checks = [], []
cover_player.watch_parent(lambda every, check: checks.append(check), lambda: done.append(1), parent, every_ms=100)
print('ready', flush=True)
while not done and checks[0]():
    time.sleep(0.1)
''' % str(Path(cover_player.__file__).parent)


def running(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rpartition(')')[2].split()[0] != 'Z'
    except OSError:
        return False


class ParentWatch(unittest.TestCase):
    def test_quits_once_the_starting_process_exits_and_not_before(self):
        parents, checks, quits = [4242], [], []
        cover_player.watch_parent(lambda every, check: checks.append((every, check)),
                                  lambda: quits.append(True), 4242, getppid=lambda: parents[0])
        (every, check), = checks
        self.assertEqual(every, 1000)
        self.assertTrue(check())
        self.assertEqual(quits, [])
        parents[0] = 1  # the driver died; init (or a subreaper) adopted the player
        self.assertFalse(check())
        self.assertEqual(quits, [True])

    def test_a_parent_that_exited_during_startup_is_caught_inside_the_main_loop(self):
        # Gtk.main_quit() before Gtk.main() does nothing, so the watch must not quit early.
        checks, quits = [], []
        cover_player.watch_parent(lambda every, check: checks.append(check), lambda: quits.append(True), 4242,
                                  getppid=lambda: 1)
        self.assertEqual(quits, [])
        self.assertFalse(checks[0]())
        self.assertEqual(quits, [True])

    def test_a_player_whose_driver_is_killed_ends_within_seconds(self):
        driver = subprocess.Popen([sys.executable, '-c',
                                   'import subprocess, sys, time\n'
                                   'p = subprocess.Popen([sys.executable, "-c", %r], stdin=subprocess.PIPE, '
                                   'stdout=subprocess.PIPE, text=True)\n'
                                   'p.stdout.readline()\n'
                                   'print(p.pid, flush=True)\n'
                                   'time.sleep(60)\n' % PLAYER], stdout=subprocess.PIPE, text=True)
        pid = int(driver.stdout.readline())
        driver.stdout.close()
        try:
            self.assertTrue(running(pid))
            driver.kill()
            driver.wait(10)
            deadline = time.monotonic() + 5
            while running(pid) and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(running(pid), 'the player outlived its driver')
        finally:
            if running(pid):
                os.kill(pid, 15)


if __name__ == '__main__':
    unittest.main()
