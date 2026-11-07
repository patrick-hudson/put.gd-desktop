"""Protocol contract for the physical-desktop input agent; no uinput device is opened."""
import io, json, socket, sys, threading, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import host_input


class FakeDevice:
    def __init__(self):
        self.batches = []
    def emit(self, events):
        self.batches.append(events)


class BrokenDevice(FakeDevice):
    def emit(self, events):
        self.batches.append(events)
        raise OSError(19, 'No such device')


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
        # The client ended with them pressed, so the agent releases them.
        self.assertEqual(keys, [[(1, 29, 1), (1, 42, 1), (1, 6, 1)], [(1, 6, 0), (1, 29, 0), (1, 42, 0)]])
        self.assertEqual(pointer, [[(3, 0, 3072), (3, 1, 32767), (1, 0x110, 1)], [(1, 0x110, 0)]])

    def test_released_input_is_not_released_again(self):
        _, keys, pointer = self.run_agent(
            {'execute': 'input-send-event', 'id': 1, 'arguments': {'events': [
                {'type': 'btn', 'data': {'button': 'left', 'down': True}},
                {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'alt'}}}]}},
            {'execute': 'input-send-event', 'id': 2, 'arguments': {'events': [
                {'type': 'btn', 'data': {'button': 'left', 'down': False}},
                {'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': 'alt'}}}]}})
        self.assertEqual(keys, [[(1, 56, 1)], [(1, 56, 0)]])
        self.assertEqual(pointer, [[(1, 0x110, 1)], [(1, 0x110, 0)]])

    def test_a_client_that_vanishes_mid_drag_leaves_the_agent_serving(self):
        keyboard, tablet = FakeDevice(), FakeDevice()
        press = {'execute': 'input-send-event', 'arguments': {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': True}}]}}
        move = {'execute': 'input-send-event', 'arguments': {'events': [{'type': 'abs', 'data': {'axis': 'x', 'value': 9}}]}}
        server, client = socket.socketpair()
        client.settimeout(5)  # a silent agent fails the test instead of hanging it
        agent = threading.Thread(target=host_input.serve_connection, args=(server, keyboard, tablet), daemon=True)
        agent.start()
        replies = client.makefile('rb')
        client.sendall(json.dumps(press).encode() + b'\n')
        replies.readline(); replies.readline()  # greeting, press acknowledged
        # Dies mid-drag: one more request, then gone without reading its reply.
        client.sendall(json.dumps(move).encode() + b'\n' * 4096)
        replies.close(); client.close()
        agent.join(5)
        self.assertFalse(agent.is_alive())
        self.assertEqual(tablet.batches[0], [(1, 0x110, 1)])
        self.assertEqual(tablet.batches[-1], [(1, 0x110, 0)])
        # The next client is served normally.
        server, client = socket.socketpair()
        client.settimeout(5)
        client.sendall(json.dumps({'execute': 'qmp_capabilities', 'id': 7}).encode() + b'\n')
        client.shutdown(socket.SHUT_WR)
        host_input.serve_connection(server, keyboard, tablet)
        lines = [json.loads(line) for line in client.makefile('rb')]
        client.close()
        self.assertIn('QMP', lines[0])
        self.assertEqual(lines[1], {'return': {}, 'id': 7})

    def test_a_client_gone_before_the_greeting_is_dropped_quietly(self):
        server, client = socket.socketpair()
        client.close()
        host_input.serve_connection(server, FakeDevice(), FakeDevice())

    def test_a_failed_keyboard_release_still_releases_the_button(self):
        class ReleaseFails(FakeDevice):
            def emit(self, events):
                self.batches.append(events)
                if events[0][2] == 0:
                    raise OSError(19, 'No such device')
        keyboard, tablet = ReleaseFails(), FakeDevice()
        stream = Stream([{'execute': 'input-send-event', 'id': 1, 'arguments': {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': True}},
            {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'alt'}}}]}}])
        with self.assertRaises(host_input.DeviceError):
            host_input.handle(stream, keyboard, tablet)
        self.assertEqual(keyboard.batches[-1], [(1, 56, 0)])
        self.assertEqual(tablet.batches[-1], [(1, 0x110, 0)])

    def test_a_device_failure_stops_the_agent_even_when_closing_the_client_fails(self):
        class ClosesBroken(Stream):
            def close(self):
                super().close()
                raise BrokenPipeError(32, 'Broken pipe')
        class Connection:
            def __init__(self, stream): self.stream = stream
            def __enter__(self): return self
            def __exit__(self, *_): return False
            def makefile(self, mode): return self.stream
        stream = ClosesBroken([{'execute': 'input-send-event', 'arguments': {'events': [
            {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'alt'}}}]}}])
        with self.assertRaises(host_input.DeviceError):
            host_input.serve_connection(Connection(stream), BrokenDevice(), FakeDevice())

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
