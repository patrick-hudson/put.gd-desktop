"""The race hunt's ThreadSanitizer filter excuses only reports on its exact known list. A report it excused wrongly would hide a put.gd
race without a trace, so the known entries, the ownership rule and the counterexamples found in review are checked."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'lab'))
import tsan_summary  # noqa: E402

QT = '/w/sanitizer-diagnosis/qtbase-everywhere-src-6.4.2/src'


def qt(symbol, n):
    return '    #%d %s %s/corelib/kernel/qobject.cpp:%d (libQt6Core.so.6+0x2c%04x) (BuildId: dca8b7765239849cdf76afcb87d15f42a404adad)' % (
        n, symbol, QT, 100 + n, n)


def own(symbol, n, path=None):
    return '    #%d %s %s:%d (putgd-http-worker+0x41%04x)' % (n, symbol, path or str(ROOT / 'src/storage/worker/HttpUpload.cpp'), 200 + n, n)


def frames(symbols, below=()):
    """Qt frames for `symbols` (a known signature, say), then `below`: (kind, symbol) pairs for the frames under them."""
    lines = [qt(sym, n) for n, sym in enumerate(symbols)]
    for kind, sym in below:
        n = len(lines)
        lines.append(qt(sym, n) if kind == 'qt' else own(sym, n) if kind == 'own' else '    #%d main %s/src/main.cpp:312 (putgd+0x2a%04x)' % (n, ROOT, n))
    return '\n'.join(lines)


def report(first, second, kind='data race'):
    return ('==================\nWARNING: ThreadSanitizer: %s (pid=4242)\n  Atomic read of size 8 at 0x7228000006f0 by thread T1:\n%s\n\n'
            '  Previous write of size 8 at 0x7228000006f0 by main thread (mutexes: write M0):\n%s\n\n'
            '  Thread T1 (tid=4250, running) created by main thread at:\n%s\n\nSUMMARY: ThreadSanitizer: %s\n==================\n'
            % (kind, first, second, own('putgd::HttpUpload::start()', 0), kind))


HANDOFF = tsan_summary.KNOWN_QT_RACES[2]  # the shape whose stacks name QThread::started and postRequest
THREAD = (('qt', 'QThreadPrivate::start(void*)'),)
CALLER = (('qt', 'QNetworkReplyHttpImplPrivate::_q_startOperation()'), ('qt', 'QNetworkAccessManager::get(QNetworkRequest const&)'))


class Classifier(unittest.TestCase):
    def kind(self, first, second, kind='data race'):
        blocks = list(tsan_summary.reports(report(first, second, kind)))  # as the tool reads a tsan.<pid> file
        self.assertEqual(len(blocks), 1)
        self.assertEqual(len(tsan_summary.access_stacks(blocks[0])), 2)
        return tsan_summary.classify(blocks[0])[0]

    def test_every_known_entry_is_excused_as_reported(self):
        for first, second in tsan_summary.KNOWN_QT_RACES:
            self.assertEqual(self.kind(frames(first, THREAD), frames(second, CALLER + (('main', 'main'),))), 'known')

    def test_a_put_gd_frame_above_main_in_either_stack_is_reported(self):
        first, second = HANDOFF
        self.assertEqual(self.kind(frames(first, THREAD + (('own', 'putgd::Worker::run()'),)), frames(second, CALLER)), 'put.gd')
        self.assertEqual(self.kind(frames(first, THREAD), frames(second, CALLER + (('own', 'putgd::HttpUpload::start()'), ('main', 'main')))), 'put.gd')

    def test_frames_below_main_are_not_read(self):
        first, second = HANDOFF
        self.assertEqual(self.kind(frames(first, THREAD), frames(second, CALLER + (('main', 'main'), ('own', 'putgd::below_main()')))), 'known')

    def test_ownership_comes_from_the_module_or_the_namespace_never_a_path(self):
        first, second = HANDOFF
        qt_named_path = own('QNetworkReplyHttpImplPrivate::helper()', 8, path='/home/x/qtbase-patches/src/network/access/helper.cpp')
        self.assertEqual(self.kind(frames(first, THREAD), frames(second, CALLER) + '\n' + qt_named_path), 'put.gd')  # a putgd module
        namespaced = qt('putgd::QtGlue::forward()', 8)  # put.gd code in a Qt-named module is still put.gd's
        self.assertEqual(self.kind(frames(first, THREAD), frames(second, CALLER) + '\n' + namespaced), 'put.gd')

    def test_one_different_frame_or_the_wrong_order_is_not_excused(self):
        first, second = HANDOFF
        changed = first[:3] + ('QObjectPrivate::somethingElse() const',) + first[4:]
        self.assertEqual(self.kind(frames(changed, THREAD), frames(second, CALLER)), 'other')
        self.assertEqual(self.kind(frames(second, CALLER), frames(first, THREAD)), 'other')
        self.assertEqual(self.kind(frames(first[:5], THREAD), frames(second, CALLER)), 'other')  # a frame short at the top
        self.assertEqual(self.kind(frames(first, THREAD), frames(second, CALLER), kind='data race on vptr'), 'other')

    # Counterexamples from review of the stack-walk rule this list replaced; each was excused there.
    def test_review_s1_a_put_gd_frame_with_qtbase_in_its_path_is_put_gd_code(self):
        first, second = HANDOFF
        sneaky = own('putgd::Patched::relay()', 6, path='/home/x/work/qtbase-fork/src/corelib/kernel/qobject.cpp')
        stack = '\n'.join(frames(second, ()).splitlines()[:6] + [sneaky]) + '\n' + frames((), CALLER)
        self.assertEqual(self.kind(frames(first, THREAD), stack), 'put.gd')

    def test_review_s10_a_put_gd_override_of_create_request_is_reported(self):
        first, second = HANDOFF
        below = (('qt', 'QNetworkAccessManager::createRequest(QNetworkAccessManager::Operation, QNetworkRequest const&, QIODevice*)'),
                 ('own', 'putgd::UploadNetwork::createRequest(QNetworkAccessManager::Operation, QNetworkRequest const&, QIODevice*)'),
                 ('qt', 'QNetworkAccessManager::get(QNetworkRequest const&)'), ('main', 'main'))
        self.assertEqual(self.kind(frames(first, THREAD), frames(second, below)), 'put.gd')

    def test_review_s11_qthread_started_alone_is_not_the_hand_off(self):
        unrelated = ('QTimer::start()', 'QMetaObject::activate(QObject*, QMetaObject const*, int, void**)', 'QThread::started(QThread::QPrivateSignal)')
        self.assertEqual(self.kind(frames(unrelated, THREAD), frames(('QTimer::stop()', 'QObject::killTimer(int)'), CALLER)), 'other')

    def test_review_s17_an_extra_own_module_adds_to_the_built_in_ones(self):
        first, second = HANDOFF
        anonymous = own('(anonymous namespace)::UploadTask::run()', 8)  # putgd-http-worker, no putgd:: namespace
        block = list(tsan_summary.reports(report(frames(first, THREAD), frames(second, CALLER) + '\n' + anonymous)))[0]
        self.assertEqual(tsan_summary.classify(block, tsan_summary.own_modules(['test_client']))[0], 'put.gd')
        self.assertTrue(set(tsan_summary.OWN_MODULES) <= set(tsan_summary.own_modules(['test_client'])))


class TsanExit(unittest.TestCase):
    """GCC TSan exits 66 when it reported anything, and also after a fatal error such as an allocator diagnostic."""

    def excused(self, text):
        import tempfile
        import types
        sys.path.insert(0, str(ROOT / 'tools' / 'lab' / 'faults'))
        import action_fuzz
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'tsan.4242').write_text(text)
            resident = action_fuzz.Resident.__new__(action_fuzz.Resident)
            resident.san, resident.options, resident.returncode = Path(folder), types.SimpleNamespace(tsan_prefix='x'), lambda: 66
            return resident.tsan_exit_excused()

    def test_known_reports_alone_excuse_exit_66(self):
        first, second = HANDOFF
        self.assertTrue(self.excused(report(frames(first, THREAD), frames(second, CALLER + (('main', 'main'),)))))

    def test_review_s3_a_fatal_sanitizer_error_is_never_excused(self):
        first, second = HANDOFF
        known = report(frames(first, THREAD), frames(second, CALLER + (('main', 'main'),)))
        fatal = '==4242==ERROR: ThreadSanitizer: requested allocation size 0x7fffffffffff exceeds maximum supported size of 0x10000000000\n'
        self.assertFalse(self.excused(known + fatal))


class Scopes(unittest.TestCase):
    def test_review_s15_every_worker_gets_its_own_unit_named_after_this_loop(self):
        import os
        import run_scope
        units = {run_scope.Scope('stress%d' % index).unit for index in range(2)} | {run_scope.Scope('stress0').unit}
        self.assertEqual(len(units), 3)
        for unit in units:
            self.assertTrue(unit.startswith('putgd-lab-%d-' % os.getpid()) and unit.endswith('.scope'), unit)


if __name__ == '__main__':
    unittest.main()
