import tempfile
import unittest
from pathlib import Path
from latency import measure
from workload import lifecycle


class Harness(unittest.TestCase):
    def test_latency_missing_clock_is_not_a_pass(self):
        with self.assertRaises(KeyError): measure({'operationId':'synthetic'})
        with self.assertRaises(ValueError):
            measure(dict(operationId='one', selection_end_ns=9,spawn_ns=8,engine_start_ns=10,
                         engine_ready_ns=11,recognition_end_ns=12,copy_ack_ns=13))

    def test_missing_helper_exits_block(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'events';p.write_text('')
            with self.assertRaises(RuntimeError): lifecycle(p)


if __name__=='__main__': unittest.main()
