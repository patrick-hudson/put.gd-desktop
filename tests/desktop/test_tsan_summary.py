"""The race hunt's ThreadSanitizer filter excuses only a diagnosed Qt race with no put.gd frame in either racing stack. A report it
excused wrongly would hide a put.gd race without a trace, so every way a put.gd frame or a misplaced anchor can appear is checked."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'lab'))
import tsan_summary  # noqa: E402

QT = '/w/sanitizer-diagnosis/qtbase-everywhere-src-6.4.2/src'
OWN = tsan_summary.own_prefixes_for(ROOT)


def qt(name, n):
    return '    #%d %s %s/corelib/kernel/qobject.cpp:%d (libQt6Core.so.6+0x2c%04x)' % (n, name, QT, 100 + n, n)


def own(name, n):
    return '    #%d %s %s/src/storage/worker/HttpUpload.cpp:%d (putgd-http-worker+0x41%04x)' % (n, name, ROOT, 200 + n, n)


def put_main(n):
    return '    #%d main %s/src/main.cpp:312 (putgd+0x2a%04x)' % (n, ROOT, n)


def stack(*names, deep_own=None, main=False, entry=None, callers=0):
    """Qt frames for `names`, Qt filler to 12 frames, then optionally a public Qt entry point, `callers` put.gd frames that called it,
    and put.gd's main()."""
    frames = [qt(name, n) for n, name in enumerate(names)]
    frames += [qt('QEventLoop::exec(QFlags<QEventLoop::ProcessEventsFlag>)', n) for n in range(len(frames), 12)]
    if deep_own is not None:
        frames[deep_own] = own('putgd::HttpUpload::readReply(QNetworkReply*)', deep_own)
    if entry:
        frames.append(qt(entry, len(frames)))
    for _ in range(callers):
        frames.append(own('putgd::HttpUpload::start()', len(frames)))
    if main:
        frames.append(put_main(len(frames)))
    return '\n'.join(frames)


GET = 'QNetworkAccessManager::get(QNetworkRequest const&)'



def report(first, second):
    return ('==================\nWARNING: ThreadSanitizer: data race (pid=4242)\n  Write of size 8 at 0x7b0c000a1230 by thread T7:\n%s\n\n'
            '  Previous read of size 8 at 0x7b0c000a1230 by main thread:\n%s\n\n'
            '  Thread T7 (tid=4250, running) created by main thread at:\n%s\n\nSUMMARY: ThreadSanitizer: data race\n==================\n'
            % (first, second, own('putgd::HttpUpload::start()', 0)))


DTOR = ('QNetworkReply::~QNetworkReply()', 'QObject::event(QEvent*)')
OTHER = ('QObject::~QObject()', 'QMetaObject::activate(QObject*, int, int, void**)')


class Classifier(unittest.TestCase):
    def kind(self, first, second):
        blocks = list(tsan_summary.reports(report(first, second)))  # as the tool reads a tsan.<pid> file
        self.assertEqual(len(blocks), 1)
        self.assertEqual(len(tsan_summary.access_stacks(blocks[0])), 2)
        return tsan_summary.classify(blocks[0], OWN)[0]

    def test_a_diagnosed_race_with_no_put_gd_frame_is_excused(self):
        self.assertEqual(self.kind(stack(*DTOR), stack(*OTHER)), 'qt-known')

    def test_a_put_gd_frame_at_any_depth_of_either_stack_is_never_excused(self):
        for depth in range(12):
            self.assertNotEqual(self.kind(stack(*DTOR, deep_own=depth), stack(*OTHER)), 'qt-known', 'first stack, frame %d' % depth)
            self.assertNotEqual(self.kind(stack(*DTOR), stack(*OTHER, deep_own=depth)), 'qt-known', 'second stack, frame %d' % depth)

    # The diagnosed hand-off as m74's live run reported it: the new thread reads in maybeSignalConnected while it emits QThread::started
    # (deeper than the top frames), the main thread writes in addConnection under Qt's postRequest.
    FILL = ('QMetaObject::activate(QObject*, int, int, void**)', 'QObjectPrivate::Connection* QAtomicOps::load()', 'QBasicAtomicPointer::load()', 'f4()', 'f5()')
    READ = ('std::atomic<QObjectPrivate::Connection*>::load() const', 'QObjectPrivate::maybeSignalConnected(unsigned int) const') + FILL + ('QThread::started(QThread::QPrivateSignal)',)
    WRITE = ('std::atomic<QObjectPrivate::Connection*>::atomic()', 'QObjectPrivate::addConnection(int, QObjectPrivate::Connection*)',
             'QObject::connect(QObject const*, char const*, QObject const*, char const*, Qt::ConnectionType)', 'QNetworkReplyHttpImplPrivate::postRequest(QNetworkRequest const&)')

    def test_the_anchor_must_be_in_the_same_stack_as_its_function(self):
        self.assertEqual(self.kind(stack(*self.READ), stack(*self.WRITE)), 'qt-known')
        moved = tuple(n for n in self.READ if not n.startswith('QThread::started'))
        self.assertNotEqual(self.kind(stack(*moved), stack(*(self.WRITE + self.FILL + ('QThread::started(QThread::QPrivateSignal)',)))), 'qt-known')  # only in the other stack
        self.assertNotEqual(self.kind(stack(*moved), stack(*self.WRITE)), 'qt-known')  # no anchor at all

    def test_put_gd_main_at_the_bottom_of_a_stack_does_not_count(self):
        self.assertEqual(self.kind(stack(*self.READ), stack(*self.WRITE, main=True)), 'qt-known')  # a main-thread stack rooted in main()
        self.assertEqual(self.kind(stack(*DTOR, main=True), stack(*OTHER, main=True)), 'qt-known')
        # One put.gd frame directly above main() is put.gd code and is never excused.
        self.assertNotEqual(self.kind(stack(*self.READ), stack(*self.WRITE, deep_own=11, main=True)), 'qt-known')
        self.assertNotEqual(self.kind(stack(*DTOR, deep_own=11, main=True), stack(*OTHER)), 'qt-known')

    def test_put_gd_callers_below_a_listed_entry_point_do_not_count(self):
        self.assertEqual(self.kind(stack(*self.READ), stack(*self.WRITE, entry=GET, callers=3, main=True)), 'qt-known')

    def test_a_put_gd_frame_between_the_racing_access_and_the_entry_point_is_reported(self):
        self.assertNotEqual(self.kind(stack(*self.READ), stack(*self.WRITE, deep_own=6, entry=GET, callers=2, main=True)), 'qt-known')

    def test_a_put_gd_frame_above_any_entry_point_in_the_other_stack_is_reported(self):
        # The writing side stops cleanly at get(); the reading thread has no entry point, and a put.gd frame in it counts at any depth.
        self.assertNotEqual(self.kind(stack(*self.READ, deep_own=10), stack(*self.WRITE, entry=GET, callers=2, main=True)), 'qt-known')

    def test_an_entry_point_off_the_diagnosis_list_does_not_stop_the_walk(self):
        dbus = 'QDBusConnection::call(QDBusMessage const&, QDBus::CallMode, int) const'  # an entry point, but not the hand-off's
        self.assertNotEqual(self.kind(stack(*self.READ), stack(*self.WRITE, entry=dbus, callers=2, main=True)), 'qt-known')

    def test_an_unknown_qt_race_is_not_excused(self):
        self.assertEqual(self.kind(stack('QTimer::start()'), stack('QTimer::stop()')), 'external')


if __name__ == '__main__':
    unittest.main()
