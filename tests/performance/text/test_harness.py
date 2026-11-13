import tempfile
import unittest
from pathlib import Path
from latency import measure
from workload import lifecycle
from helper_score import quality_summary
from score import score


class Harness(unittest.TestCase):
    def test_latency_missing_clock_is_not_a_pass(self):
        with self.assertRaises(KeyError): measure({'operationId':'synthetic'})
        with self.assertRaises(ValueError):
            measure(dict(operationId='one', selection_end_ns=9,spawn_ns=8,engine_start_ns=10,
                         engine_ready_ns=11,recognition_end_ns=12,copy_ack_ns=13))

    def test_incomplete_helper_corpus_has_no_quality_table(self):
        row=score(dict(id='clear',**{'class':'terminal'},font_pixels=16),'clear text','clear text')
        for rows,expected in (([row],2),([],0)):
            self.assertEqual(quality_summary(rows,expected),('blocked',{}))
        self.assertEqual(quality_summary([row],1)[0],'measured')

    def test_missing_helper_exits_block(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'events';p.write_text('')
            with self.assertRaises(RuntimeError): lifecycle(p)


if __name__=='__main__': unittest.main()
