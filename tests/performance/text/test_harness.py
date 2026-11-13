import tempfile
import unittest
from pathlib import Path
from latency import measure
from workload import lifecycle, validate_socket


class Harness(unittest.TestCase):
    def test_latency_missing_clock_is_not_a_pass(self):
        with self.assertRaises(KeyError): measure({'operationId':'synthetic'})
        with self.assertRaises(ValueError):
            measure(dict(operationId='one', selection_end_ns=9,spawn_ns=8,engine_start_ns=10,
                         engine_ready_ns=11,recognition_end_ns=12,copy_ack_ns=13))

    def test_separate_phases(self):
        m=measure(dict(operationId='one',selection_end_ns=0,spawn_ns=1_000_000,engine_start_ns=3_000_000,
                       engine_ready_ns=8_000_000,recognition_end_ns=18_000_000,copy_ack_ns=19_000_000))
        self.assertEqual([m[k] for k in ('spawn_ms','engine_start_ms','recognition_ms','copy_ms','total_ms')], [3,5,10,1,19])

    def test_missing_helper_exits_block(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'events';p.write_text('')
            with self.assertRaises(RuntimeError): lifecycle(p)

    def test_overlong_socket_rejected_before_connect(self):
        with self.assertRaisesRegex(RuntimeError,'107'):
            validate_socket(Path('/tmp')/('x'*100)/'putgd/session.sock',1)


if __name__=='__main__': unittest.main()
