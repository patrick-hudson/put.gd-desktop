#!/usr/bin/env python3
"""Group ThreadSanitizer reports by their racing accesses, and separate the few verified Qt-internal races from everything else.

TSan with log_path writes one file per process (tsan.<pid>). Reports are captured unsuppressed (tests/sanitizer/tsan.supp holds only
the D-Bus lock-order rule), and this lists each distinct report once with how often it occurred, in three parts:

  put.gd     a put.gd frame within the top frames of either racing access: read these first
  qt-known   a known Qt-internal race (KNOWN_QT_RACES, each diagnosed in docs/testing/thread-sanitizer.md): both racing access stacks
             start in a Qt library and hold no put.gd frame at all, in any of their frames
  external   everything else: Qt, GLib, GStreamer or the C library with no known race; read it before adding an entry

Only qt-known reports are excused, under one strict rule: neither racing access stack contains a put.gd frame anywhere (so a report
whose main-thread stack ends in put.gd's main() is not excused), both start in Qt, and an entry's function is in the top frames of
one of the two stacks, with its anchor (the diagnosed Qt caller), when it has one, in that same stack. Any other report counts,
including every report that involves put.gd at any depth. Thread-creation, mutex and allocation stacks are not read. The
action-sequence fuzzer uses unexplained() to fail a seed on anything but qt-known reports.

  python3 -I tools/lab/tsan_summary.py --source . "$report_dir"

where $report_dir is a folder of tsan.<pid> files, such as a fuzzer seed's <run-dir>/san or a CTest run's TSAN_OPTIONS log_path folder.
"""
import argparse
import collections
from pathlib import Path
import re
import sys

TOP = 6  # frames of a racing access in which a known function must appear

# The Qt-internal races diagnosed in the race hunt (docs/testing/thread-sanitizer.md), as (function, anchor). A report is excused only
# when neither racing access stack holds a put.gd frame, the function is in the top frames of one stack, and the anchor (when given) is
# in that same stack. The anchor ties a generic Qt function to the one Qt caller the diagnosis covers.
KNOWN_QT_RACES = (
    # QNetworkAccessManager starts its HTTP thread, then connects to that QThread's started() from the calling thread; the new thread reads
    # the connection list with a relaxed load (qobject.cpp maybeSignalConnected) while connect() publishes it under the signal mutex.
    ('QThread::started', None),
    ('QObjectPrivate::maybeSignalConnected', 'QNetworkReplyHttpImplPrivate::postRequest'),
    # A QNetworkReply is deleted on its own thread while the access manager's HTTP thread still runs the QObject destructor chain of the
    # delegate (vptr report in ~QNetworkReply / ~QObject).
    ('QNetworkReply::~QNetworkReply', None),
    # QtDBus's own thread dispatches posted events while the main thread runs ~QCoreApplication at exit and clears QCoreApplication::self.
    ('QCoreApplication::~QCoreApplication', None),
    # The same QtDBus thread destroys QDBusPendingCallWatcherHelper (vptr reset) while it finishes a call.
    ('QDBusPendingCallWatcherHelper::~QDBusPendingCallWatcherHelper', None),
    # QtDBus completes a call on its own thread and the calling thread reads the reply after its wait.
    ('QDBusConnectionPrivate::processFinishedCall', None),
    ('QDBusConnectionPrivate::sendWithReply', None),
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
    # Strict: no put.gd frame anywhere in either racing access stack (every frame, not only the top ones), both start in Qt, and a known
    # function sits in the top frames of one stack whose own frames also hold its anchor (the diagnosed Qt caller), when it has one.
    clean = len(stacks) == 2 and all(frames and is_qt(frames[0]) and not any(is_own(l, own_prefixes) for l in frames) for frames in stacks)
    if clean and kind and kind.group(1) == 'data race':
        for frames in stacks:
            top = ' '.join(function(l) for l in frames[:TOP])
            whole = ' '.join(function(l) for l in frames)
            if any(name in top and (anchor is None or anchor in whole) for name, anchor in KNOWN_QT_RACES):
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
