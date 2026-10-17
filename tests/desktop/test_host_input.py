"""Protocol contract for the physical-desktop input agent; no uinput device is opened."""
import io, json, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import host_input


class FakeDevice:
    def __init__(self):
        self.batches = []
    def emit(self, events):
        self.batches.append(events)


class Stream(io.BytesIO):
    def __init__(self, requests):
        super().__init__(b''.join(json.dumps(r).encode() + b'\n' for r in requests))
        self.replies = []
    def write(self, data):
        self.replies.append(json.loads(data))
    def flush(self):
        pass


class HostInputTests(unittest.TestCase):
    def run_agent(self, *requests):
        keyboard, tablet, stream = FakeDevice(), FakeDevice(), Stream(requests)
        host_input.handle(stream, keyboard, tablet)
        return stream.replies, keyboard.batches, tablet.batches

    def test_runner_shapes_map_to_evdev(self):
        replies, keys, pointer = self.run_agent(
            {'execute': 'qmp_capabilities', 'id': 1},
            {'execute': 'input-send-event', 'id': 2, 'arguments': {'events': [
                {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'ctrl'}}},
                {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'shift'}}},
                {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': '5'}}}]}},
            {'execute': 'input-send-event', 'id': 3, 'arguments': {'events': [
                {'type': 'abs', 'data': {'axis': 'x', 'value': 3072}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': 32767}},
                {'type': 'btn', 'data': {'button': 'left', 'down': True}}]}})
        self.assertIn('QMP', replies[0])
        self.assertEqual([r.get('id') for r in replies[1:]], [1, 2, 3])
        self.assertTrue(all('return' in r for r in replies[1:]))
        self.assertEqual(keys, [[(1, 29, 1), (1, 42, 1), (1, 6, 1)]])
        self.assertEqual(pointer, [[(3, 0, 3072), (3, 1, 32767), (1, 0x110, 1)]])

    def test_rejects_anything_outside_the_runner_subset(self):
        bad = [
            {'execute': 'screendump', 'id': 1, 'arguments': {'filename': '/tmp/x'}},
            {'execute': 'input-send-event', 'id': 2, 'arguments': {'events': [{'type': 'key', 'data': {'down': True, 'key': {'type': 'number', 'data': 1}}}]}},
            {'execute': 'input-send-event', 'id': 3, 'arguments': {'events': [{'type': 'abs', 'data': {'axis': 'x', 'value': 40000}}]}},
            {'execute': 'input-send-event', 'id': 4, 'arguments': {'events': [{'type': 'rel', 'data': {'axis': 'x', 'value': 1}}]}},
            {'execute': 'input-send-event', 'id': 5, 'arguments': {'events': []}},
            {'execute': 'input-send-event', 'id': 6, 'arguments': {'events': [{'type': 'btn', 'data': {'button': 'extra', 'down': True}}]}},
        ]
        replies, keys, pointer = self.run_agent(*bad)
        self.assertEqual([r['id'] for r in replies[1:]], [1, 2, 3, 4, 5, 6])
        self.assertTrue(all('error' in r for r in replies[1:]))
        self.assertEqual(keys + pointer, [])

    def test_every_qcode_the_runner_and_drivers_use_is_mapped(self):
        for name in ['ctrl', 'shift', 'alt', 'ret', 'esc', 'delete', 'spc', 'v', 'z', 't', 'o', 'k', '5', 'f9', 'f12', 'meta_l']:
            self.assertIn(name, host_input.QCODES)
        self.assertEqual(len(set(host_input.QCODES.values())), len(host_input.QCODES))


if __name__ == '__main__':
    unittest.main()
