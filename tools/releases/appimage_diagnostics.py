#!/usr/bin/env python3
"""Run the AppImage's own putgd-record lab diagnostics on every export format.

Headless, with nothing captured: the relocated helper writes its synthetic A/V
marker clip (MJPEG and PCM in Matroska), exports it the way the review does
(MP4 with H.264, WebM with VP9, GIF), and `--motion`, `--preview` and, for the
formats with sound, `--av-sync` must read each copy (docs/recording.md, "Lab
diagnostics"). Not read: H.265 MP4 (FFmpeg's HEVC decoder would add 0.34 MiB
to the image) and WebP (FFmpeg 7.1 has no animated WebP decoder).
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

MARKER_SECONDS = 4
# Samples sit at (2i+1)/2n of the clip: with 40 over 4 s the first lands at
# 50 ms, inside the marker's first 100 ms flash, so a readable copy has motion.
MOTION_SAMPLES = 40
MIN_MOTION = 1.0
# One frame at the marker's 30 fps; the marker itself measures 0 ms.
MAX_OFFSET_MS = 34.0
EXPORTS = (("mp4", "h264", True), ("webm", "vp9", True), ("gif", None, False))
MOTION = re.compile(r"^motion=([0-9.]+) compared=(\d+) ")
PREVIEW = re.compile(r"^frame (\d+)x(\d+) at ")


def helper_environment(appdir: Path, cache: Path) -> dict:
    """As documented for the lab: only APPDIR, no display and no library path,
    so the image's own RUNPATHs must resolve the pinned FFmpeg and its codecs."""
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "LD_LIBRARY_PATH")
                   and not key.startswith("GST_")}
    environment.update(APPDIR=str(appdir), XDG_CACHE_HOME=str(cache))
    return environment


def run(helper: Path, environment: dict, *arguments) -> str:
    result = subprocess.run([str(helper), *map(str, arguments)], env=environment, capture_output=True,
                            text=True, timeout=300)
    if result.returncode:
        raise ValueError("putgd-record " + " ".join(map(str, arguments[:1])) + " failed on " + str(arguments[1]) +
                         ": " + (result.stderr.strip() or "exit " + str(result.returncode)))
    return result.stdout.strip()


def motion(output: str, name: str) -> float:
    match = MOTION.match(output)
    if not match or int(match.group(2)) < 1:
        raise ValueError("putgd-record --motion compared no frames of " + name + ": " + output)
    value = float(match.group(1))
    if value < MIN_MOTION:
        raise ValueError("putgd-record --motion saw no marker flash in " + name + ": " + output)
    return value


def av_sync(output: str, name: str) -> dict:
    result = json.loads(output)
    if result.get("pairs", 0) < MARKER_SECONDS - 1 or abs(result.get("medianMs", 1e9)) > MAX_OFFSET_MS:
        raise ValueError("putgd-record --av-sync misread " + name + ": " + output)
    return {key: result[key] for key in ("pairs", "medianMs", "spreadMs")}


def check(appdir: Path, work: Path) -> dict:
    helper = appdir / "usr/bin/putgd-record"
    environment = helper_environment(appdir, work / "cache")
    marker = work / "marker.mkv"
    run(helper, environment, "--av-marker", marker, MARKER_SECONDS)
    clips = {"mkv": {"path": marker, "export": None, "sound": True}}
    for extension, codec, sound in EXPORTS:
        path = work / ("copy." + extension)
        export = run(helper, environment, "--export", marker, path, *(("--codec", codec) if codec else ()))
        clips[extension] = {"path": path, "export": export, "sound": sound}
    results = {}
    for name, clip in clips.items():
        entry = {"export": clip["export"],
                 "motion": motion(run(helper, environment, "--motion", clip["path"], MOTION_SAMPLES), name)}
        picture = work / (name + "-preview.png")
        preview = PREVIEW.match(run(helper, environment, "--preview", clip["path"], 1000000, picture))
        if not preview or not picture.is_file():
            raise ValueError("putgd-record --preview wrote no frame of " + name)
        entry["preview"] = preview.group(1) + "x" + preview.group(2)
        if clip["sound"]:
            entry["avSync"] = av_sync(run(helper, environment, "--av-sync", clip["path"]), name)
        results[name] = entry
    return {"markerSeconds": MARKER_SECONDS, "clips": results}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True, help="Deployed AppDir (or an extracted squashfs-root)")
    parser.add_argument("--output", type=Path, help="Write the results here as JSON")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pgdiag-") as directory:
        result = check(args.appdir.resolve(), Path(directory))
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
