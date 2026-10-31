"""tools/lab/fluidity.py: the frame-number strip survives a round trip through synthetic frames (#6)."""
import importlib.util
import pathlib
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
TOOL = HERE.parent.parent / 'tools/lab/fluidity.py'


@pytest.fixture(scope='module')
def fluidity():
    spec = importlib.util.spec_from_file_location('fluidity', TOOL)
    module = importlib.util.module_from_spec(spec)
    sys.modules['fluidity'] = module
    spec.loader.exec_module(module)
    return module


def frame_with_strip(fluidity, counter, width=640, height=360, x0=24, y0=24, cell=24, background=30):
    """A GRAY8 frame as the fixture draws it: reference black, reference white, Gray-coded bits."""
    data = bytearray([background]) * (width * height)
    code = fluidity.gray(counter)
    for index in range(fluidity.STRIP_CELLS):
        value = 0 if index == 0 else 255 if index == 1 else (255 if (code >> (index - 2)) & 1 else 0)
        for y in range(y0, y0 + cell):
            start = y * width + x0 + index * cell
            data[start:start + cell] = bytes([value]) * cell
    return width, height, width, bytes(data)


def test_gray_code_round_trip(fluidity):
    for n in (0, 1, 2, 3, 255, 256, 4095, 65535):
        assert fluidity.ungray(fluidity.gray(n)) == n
    # Consecutive frame numbers differ in exactly one bit, so one misread cell costs one step.
    for n in range(0, 2000):
        assert bin(fluidity.gray(n) ^ fluidity.gray(n + 1)).count('1') == 1


@pytest.mark.parametrize('counter', [0, 1, 7, 1000, 32768, 65535])
def test_strip_is_found_and_read(fluidity, counter):
    width, height, stride, data = frame_with_strip(fluidity, counter)
    strip = fluidity.find_strip(width, height, stride, data)
    assert strip == (24, 24, 24)
    assert fluidity.read_counter(width, stride, data, strip) == counter


def test_strip_is_found_at_an_offset_and_other_cell_size(fluidity):
    width, height, stride, data = frame_with_strip(fluidity, 4242, width=800, height=600, x0=137, y0=61, cell=16)
    strip = fluidity.find_strip(width, height, stride, data)
    assert strip == (137, 61, 16)
    assert fluidity.read_counter(width, stride, data, strip) == 4242


def test_frame_without_strip_is_unreadable(fluidity):
    width, height = 320, 200
    data = bytes([90]) * (width * height)
    assert fluidity.find_strip(width, height, width, data) is None
    # A strip whose references do not contrast (a blurred or scaled clip) reads as unreadable, not as a number.
    width, height, stride, data = frame_with_strip(fluidity, 5)
    assert fluidity.read_counter(width, stride, data, (24, 24, 24)) == 5
    flat = bytearray(data)
    for y in range(24, 48):
        flat[y * width + 48:y * width + 72] = bytes([40]) * 24
    assert fluidity.read_counter(width, stride, bytes(flat), (24, 24, 24)) is None


def test_analysis_counts_repeats_and_skips(fluidity, tmp_path, monkeypatch):
    # A recorder at 60 fps that captured display frames 1,2,3,3,4,6,7: one repeat, one skipped frame.
    counters = [1, 2, 3, 3, 4, 6, 7]
    frames = [(i * 16_666_667, *frame_with_strip(fluidity, c)) for i, c in enumerate(counters)]
    monkeypatch.setattr(fluidity, 'decode_frames', lambda path: iter(frames))
    out = tmp_path / 'report.json'
    assert fluidity.main(['analyze', 'clip.mkv', '--fps', '60', '--json', str(out)]) == 0
    import json
    report = json.loads(out.read_text())
    assert report['frames'] == 7
    assert report['distinctSourceFrames'] == 6
    assert report['pairs'] == {'repeat': 1, 'fluid': 4, 'skip1': 1, 'skip2plus': 0, 'backwards': 0}
    assert report['longestStallFrames'] == 1 and report['longestStallMs'] == pytest.approx(16.7, abs=0.1)
    assert report['longestSkipFrames'] == 1
    assert report['ptsGapMs']['offGrid'] == 0
    assert report['unreadable'] == 0


def test_ticks_report_late_frames(fluidity, tmp_path):
    log = tmp_path / 'ticks.log'
    times = [0]
    for i in range(1, 121):
        times.append(times[-1] + (50_000 if i == 60 else 16_667))
    log.write_text('# covered\n' + ''.join(f'{i} {t}\n' for i, t in enumerate(times)))
    out = tmp_path / 'ticks.json'
    assert fluidity.main(['ticks', str(log), '--hz', '60', '--json', str(out)]) == 0
    import json
    report = json.loads(out.read_text())
    assert report['frames'] == 121 and report['lateFrames'] == 1 and report['missedFrames'] == 2
    assert report['maxMs'] == 50.0
