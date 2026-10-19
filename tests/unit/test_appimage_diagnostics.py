#!/usr/bin/env python3
"""AppImage lab-diagnostics check with a synthetic helper; the real image runs in build-appimage.sh."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("appimage_diagnostics", ROOT / "tools/releases/appimage_diagnostics.py")
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)

# Answers like putgd-record; PG_FAIL="<option> <suffix>" fails that option on that clip.
FAKE_HELPER = r'''#!PYTHON
import json, os, sys
from pathlib import Path
arguments = sys.argv[1:]
with open(os.environ["PG_LOG"], "a") as log:
    log.write(json.dumps({"argv": arguments, "appdir": os.environ.get("APPDIR"),
                          "display": os.environ.get("DISPLAY"), "library": os.environ.get("LD_LIBRARY_PATH")}) + "\n")
option, clip = arguments[0], arguments[1]
failure = os.environ.get("PG_FAIL", "").split()
if failure and failure[0] == option and clip.endswith(failure[1]):
    sys.stderr.write("recording_export_failed: Cannot open the working clip\n")
    sys.exit(1)
if option in ("--av-marker", "--export"):
    Path(arguments[2] if option == "--export" else clip).write_bytes(b"clip")
    if option == "--export":
        print("encoder=fake audio=fake frames=120 size=1280x720 durationMs=4000 bytes=4 wallMs=1 speed=1.00x")
elif option == "--motion":
    print("motion=" + os.environ.get("PG_MOTION", "41.500") + " compared=39 samples=40 durationMs=4000 size=1280x720")
elif option == "--preview":
    Path(arguments[3]).write_bytes(b"png")
    print("frame 1280x720 at 1000000 us")
elif option == "--av-sync":
    print(json.dumps({"pairs": 4, "medianMs": float(os.environ.get("PG_OFFSET", "0")), "spreadMs": 0.0}))
'''


class DiagnosticsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pgdiagt-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.appdir = self.root / "AppDir"
        helper = self.appdir / "usr/bin/putgd-record"
        helper.parent.mkdir(parents=True)
        helper.write_text(FAKE_HELPER.replace("PYTHON", sys.executable))
        helper.chmod(0o755)
        self.log = self.root / "calls.jsonl"
        self.saved = dict(os.environ)
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(self.saved)))
        os.environ.update(PG_LOG=str(self.log), DISPLAY=":99", GST_PLUGIN_PATH="/host", LD_LIBRARY_PATH="/host/lib")

    def check(self):
        work = self.root / "work"
        work.mkdir(exist_ok=True)
        return diagnostics.check(self.appdir, work)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_every_export_format_is_read_by_each_diagnostic(self):
        result = self.check()
        self.assertEqual(set(result["clips"]), {"mkv", "mp4", "webm", "gif"})
        calls = self.calls()
        exports = [call["argv"] for call in calls if call["argv"][0] == "--export"]
        self.assertEqual([Path(argv[2]).name for argv in exports],
                         ["copy.mp4", "copy.webm", "copy.gif"])
        self.assertEqual([argv[3:] for argv in exports], [["--codec", "h264"], ["--codec", "vp9"], []])
        for option in ("--motion", "--preview"):
            self.assertEqual(len([call for call in calls if call["argv"][0] == option]), 4)
        synced = [Path(call["argv"][1]).suffix for call in calls if call["argv"][0] == "--av-sync"]
        self.assertEqual(sorted(synced), [".mkv", ".mp4", ".webm"])
        self.assertNotIn("avSync", result["clips"]["gif"])
        self.assertEqual(result["clips"]["webm"]["preview"], "1280x720")
        for call in calls:
            self.assertEqual(call["appdir"], str(self.appdir))
            self.assertIsNone(call["display"])
            self.assertIsNone(call["library"])

    def test_unreadable_copy_is_rejected_with_the_helper_error(self):
        os.environ["PG_FAIL"] = "--motion .mp4"
        with self.assertRaisesRegex(ValueError, r"--motion failed on .*copy\.mp4: recording_export_failed"):
            self.check()

    def test_marker_encoder_failure_is_rejected(self):
        os.environ["PG_FAIL"] = "--av-marker .mkv"
        with self.assertRaisesRegex(ValueError, "--av-marker failed"):
            self.check()

    def test_copy_without_motion_is_rejected(self):
        os.environ["PG_MOTION"] = "0.004"
        with self.assertRaisesRegex(ValueError, "saw no marker flash"):
            self.check()

    def test_misread_sync_is_rejected(self):
        os.environ["PG_OFFSET"] = "120"
        with self.assertRaisesRegex(ValueError, "--av-sync misread"):
            self.check()


class MediaContractTest(unittest.TestCase):
    def test_pinned_ffmpeg_reads_every_diagnosed_format(self):
        configure = json.loads((ROOT / "packaging/appimage/media.json").read_text())["ffmpeg"]["configure"]
        enabled = {}
        for argument in configure:
            if argument.startswith("--enable-") and "=" in argument:
                kind, names = argument[len("--enable-"):].split("=", 1)
                enabled.setdefault(kind, set()).update(names.split(","))
        self.assertLessEqual({"matroska", "mov", "gif"}, enabled["demuxer"])
        # libvpx (already bundled for VP9 export) decodes VP9; FFmpeg's own VP9 decoder costs 1.5 MB.
        self.assertLessEqual({"h264", "libvpx_vp9", "aac", "opus", "gif", "mjpeg", "pcm_s16le"}, enabled["decoder"])
        self.assertFalse({"vp9", "hevc"} & enabled["decoder"])
        self.assertLessEqual({"mjpeg", "pcm_s16le"}, enabled["encoder"])
        self.assertIn("appimage_diagnostics.py", (ROOT / "tools/releases/build-appimage.sh").read_text())


if __name__ == "__main__":
    unittest.main()
