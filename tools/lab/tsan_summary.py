#!/usr/bin/env python3
"""Group ThreadSanitizer reports by their racing accesses, and separate the few verified Qt-internal races from everything else.

TSan with log_path writes one file per process (tsan.<pid>). Reports are captured unsuppressed (tests/sanitizer/tsan.supp holds only
the D-Bus lock-order rule), and this lists each distinct report once with how often it occurred, in three parts:

  put.gd     a put.gd frame within the top frames of either racing access: read these first
  qt-known   a known Qt-internal race (KNOWN_QT_RACES, each diagnosed in docs/testing/thread-sanitizer.md) whose two racing accesses
             both start in a Qt library with no put.gd frame in their top frames
  external   everything else: Qt, GLib, GStreamer or the C library with no known race; read it before adding an entry

Only qt-known reports are excused. An entry names a function that must be in the top frames of a racing access, and it applies only
when both racing accesses pass the Qt-only test, so a put.gd frame near the top of either access (put.gd touching a QNetworkReply
that Qt destroys) makes it a put.gd report, never a known one. A thread-creation or mutex stack never matches an entry. The action-sequence fuzzer uses unexplained() to fail a seed on anything but qt-known reports.

  python3 -I tools/lab/tsan_summary.py --source . "$report_dir"

where $report_dir is a folder of tsan.<pid> files, such as a fuzzer seed's <run-dir>/san or a CTest run's TSAN_OPTIONS log_path folder.
"""
import argparse
import collections
from pathlib import Path
import re
import sys

TOP = 6  # frames of each racing access that must hold no put.gd code for a known race to apply

# Functions of the Qt-internal races diagnosed in the race hunt (docs/testing/thread-sanitizer.md). A report is excused only when one of
# these is in the top frames of a racing access and both racing accesses pass the Qt-only test (is_qt_only).
KNOWN_QT_RACES = (
    # QNetworkAccessManager starts its HTTP thread, then connects to that QThread's started() from the calling thread; the new thread reads
    # the connection list with a relaxed load (qobject.cpp maybeSignalConnected) while connect() publishes it under the signal mutex.
    'QThread::started',
    # A QNetworkReply is deleted on its own thread while the access manager's HTTP thread still runs the QObject destructor chain of the
    # delegate (vptr report in ~QNetworkReply / ~QObject).
    'QNetworkReply::~QNetworkReply',
    # QtDBus's own thread dispatches posted events while the main thread runs ~QCoreApplication at exit and clears QCoreApplication::self.
    'QCoreApplication::~QCoreApplication',
    # The same QtDBus thread destroys QDBusPendingCallWatcherHelper (vptr reset) while it finishes a call.
    'QDBusPendingCallWatcherHelper::~QDBusPendingCallWatcherHelper',
    # QtDBus completes a call on its own thread and the calling thread reads the reply after its wait.
    'QDBusConnectionPrivate::processFinishedCall',
    'QDBusConnectionPrivate::sendWithReply',
)


def function(line):
    line = re.sub(r'^\s+#\d+ ', '', line)
    return re.sub(r' (/|\.\./|<null>).*', '', line)[:110]


def is_qt(frame):
    return 'libQt6' in frame or '/qtbase' in frame


def is_own(frame, own_prefixes):
    module = re.search(r'\(([^()+]+)\+0x[0-9a-f]+\)\s*$', frame)
    name = Path(module.group(1)).name if module else ''
    return any(p in frame for p in own_prefixes) or name.startswith(('putgd', 'libputgd'))


def reports(text):
    for block in text.split('==================\n'):
        if 'WARNING: ThreadSanitizer' in block:
            yield block


def access_stacks(block):
    accesses = [s for s in block.split('\n\n') if re.match(r'\s*(WARNING.*\n)?\s*(Write|Read|Previous|Atomic)', s)][:2]
    return [[l for l in s.split('\n') if re.match(r'\s+#\d+ ', l)] for s in accesses]


def classify(block, own_prefixes):
    """'put.gd', 'qt-known' or 'external', and the report's grouping key."""
    kind = re.search(r'WARNING: ThreadSanitizer: (.*?) \(', block)
    stacks = access_stacks(block)
    own = [function(l) for frames in stacks for l in frames[:5] if is_own(l, own_prefixes)]
    key = (kind.group(1) if kind else '?', ' || '.join(function(frames[0]) if frames else '?' for frames in stacks), tuple(sorted(set(own)))[:2])
    if own:
        return 'put.gd', key
    qt_only = len(stacks) == 2 and all(frames and is_qt(frames[0]) and not any(is_own(l, own_prefixes) for l in frames[:TOP]) for frames in stacks)
    if qt_only and kind and kind.group(1) == 'data race':
        top = ' '.join(function(l) for frames in stacks for l in frames[:TOP])
        if any(name in top for name in KNOWN_QT_RACES):
            return 'qt-known', key
    return 'external', key


def own_prefixes_for(source):
    root = Path(source).resolve()
    return (str(root / 'src') + '/', str(root / 'tests') + '/')


def unexplained(path, source):
    """The reports in one tsan.<pid> file that are not a known Qt-internal race."""
    prefixes = own_prefixes_for(source)
    return [block for block in reports(Path(path).read_text(errors='replace')) if classify(block, prefixes)[0] != 'qt-known']


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--source', required=True, help='the repository root; a frame under its src/ or tests/ is put.gd code')
    options = parser.parse_args()
    prefixes = own_prefixes_for(options.source)
    groups = {name: collections.defaultdict(list) for name in ('put.gd', 'external', 'qt-known')}
    for report in sorted(options.folder.glob('tsan.*')):
        for block in reports(report.read_text(errors='replace')):
            part, key = classify(block, prefixes)
            groups[part][key].append(report.name)
    for name in ('put.gd', 'external', 'qt-known'):
        print('=====', name, sum(len(v) for v in groups[name].values()), 'reports')
        for (kind, key, own), files in sorted(groups[name].items(), key=lambda item: -len(item[1])):
            print('%d x %s | %s' % (len(files), kind, key[:200]))
            if own:
                print('      put.gd: %s' % ' ; '.join(own)[:240])
            print('      e.g. %s' % files[0])
    return 0


if __name__ == '__main__':
    sys.exit(main())
