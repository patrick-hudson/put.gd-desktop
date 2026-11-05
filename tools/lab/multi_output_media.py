#!/usr/bin/env python3
"""Decode bounded synthetic recording witnesses with distro GStreamer, never a display.

Runs beside a prepared geometry campaign. Saves eight RGB PPM samples at half-
second intervals; output must be new. Missing introspection/decoder prerequisites
fail the recording case rather than reducing it to a motion-only pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time


def decode(path, output):
    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gst
    Gst.init(None)
    output.mkdir(mode=0o700, exist_ok=False)
    pipeline = Gst.parse_launch('uridecodebin name=source ! videoconvert ! video/x-raw,format=RGB ! '
                                'appsink name=frames sync=false max-buffers=1 drop=false')
    pipeline.get_by_name('source').set_property('uri', Gst.filename_to_uri(str(path.resolve())))
    sink = pipeline.get_by_name('frames')
    samples = []
    deadline = time.monotonic() + 25
    pipeline.set_state(Gst.State.PLAYING)
    try:
        while len(samples) < 8:
            if time.monotonic() > deadline:
                raise RuntimeError('Synthetic clip decode deadline exceeded')
            sample = sink.emit('try-pull-sample', Gst.SECOND // 4)
            if sample is None:
                message = pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR | Gst.MessageType.EOS)
                if message is not None:
                    raise RuntimeError('Clip ended/failed before eight half-second witnesses')
                continue
            buffer = sample.get_buffer()
            if buffer.pts == Gst.CLOCK_TIME_NONE or buffer.pts < len(samples) * Gst.SECOND // 2:
                continue
            caps = sample.get_caps().get_structure(0)
            width, height = caps.get_value('width'), caps.get_value('height')
            stride = (width * 3 + 3) & ~3  # packed RGB system-memory GstVideoInfo alignment
            ok, mapped = buffer.map(Gst.MapFlags.READ)
            if not ok:
                raise RuntimeError('Decoded RGB sample could not be mapped')
            try:
                if len(mapped.data) != stride * height:
                    raise RuntimeError('Unexpected decoded RGB stride/buffer extent')
                pixels = b''.join(mapped.data[y * stride:y * stride + width * 3] for y in range(height))
                frame = output / ('frame-%d.ppm' % len(samples))
                frame.write_bytes(b'P6\n%d %d\n255\n' % (width, height) + pixels)
                samples.append({'file': frame.name, 'width': width, 'height': height, 'ptsNs': buffer.pts,
                                'sha256': hashlib.sha256(frame.read_bytes()).hexdigest()})
            finally:
                buffer.unmap(mapped)
    finally:
        pipeline.set_state(Gst.State.NULL)
    return {'decoder': Gst.version_string(), 'samples': samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.clip, args.output)))


if __name__ == '__main__':
    main()
