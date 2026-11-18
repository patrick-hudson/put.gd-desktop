#!/usr/bin/env python3
"""Group ThreadSanitizer reports by their racing accesses, and separate the few verified Qt-internal races from everything else.

TSan with log_path writes one file per process (tsan.<pid>). Reports are captured unsuppressed (tests/sanitizer/tsan.supp holds only
the D-Bus lock-order rule), and this lists each distinct report once with how often it occurred, in three parts:

  put.gd     a put.gd frame within the top frames of either racing access: read these first
  qt-known   a diagnosed Qt-internal race (KNOWN_QT_RACES, each diagnosed in docs/testing/thread-sanitizer.md) that passes the rule below
  external   everything else: Qt, GLib, GStreamer or the C library with no known race; read it before adding an entry

Only qt-known reports are excused, under one rule. For each of the two racing access stacks, read from the top (the racing access)
down: every frame must be Qt or the C, C++ or TSan runtime until the walk reaches the bottom of the stack, main(), or one of the
public Qt entry points listed for that diagnosis. Below that point anything may follow, put.gd callers included: they only called a
public Qt function. A put.gd frame (or any other library's) above that point, at any depth, means the report is shown. The diagnosed
function must be in the top frames of one stack, and its anchor, when it has one, in that same stack above its stopping point. An
entry point that is not on the diagnosis's own list does not stop the walk. Thread-creation, mutex and allocation stacks are not
read. The action-sequence fuzzer uses unexplained() to fail a seed on anything but qt-known reports.

  python3 -I tools/lab/tsan_summary.py --source . "$report_dir"

where $report_dir is a folder of tsan.<pid> files, such as a fuzzer seed's <run-dir>/san or a CTest run's TSAN_OPTIONS log_path folder.
"""
import argparse
import collections
from pathlib import Path
import re
import sys

TOP = 6  # frames of a racing access in which a diagnosed function must appear

# Public Qt calls put.gd makes. A walk down a racing stack stops at one of these only when the diagnosis lists it: put.gd frames below it
# called Qt and are not part of the race. Each list is the smallest one the diagnosis needs.
QNAM_REQUESTS = ('QNetworkAccessManager::get', 'QNetworkAccessManager::post', 'QNetworkAccessManager::put', 'QNetworkAccessManager::head',
                 'QNetworkAccessManager::deleteResource', 'QNetworkAccessManager::sendCustomRequest', 'QNetworkAccessManager::createRequest')
DBUS_CALLS = ('QDBusConnection::call', 'QDBusConnection::callWithCallback', 'QDBusConnection::asyncCall', 'QDBusAbstractInterface::call',
              'QDBusAbstractInterface::callWithArgumentList', 'QDBusAbstractInterface::asyncCall', 'QDBusAbstractInterface::asyncCallWithArgumentList')

# The Qt-internal races diagnosed in the race hunt (docs/testing/thread-sanitizer.md): what must be in the stack ((function, anchor)
# pairs, the anchor in the same stack) and the public entry points that end the walk.
KNOWN_QT_RACES = (
    {   # QNetworkAccessManager starts its HTTP thread, then connects to that QThread's started() from the calling thread (postRequest); the
        # new thread reads the connection list with a relaxed load (qobject.cpp maybeSignalConnected) while it emits started() and
        # connect() publishes the list under the signal mutex. The calling side enters Qt through a request call.
        'name': 'QNetworkAccessManager QThread::started hand-off',
        'match': (('QThread::started', None), ('QObjectPrivate::maybeSignalConnected', 'QThread::started')),
        'entries': QNAM_REQUESTS,
    },
    {   # A QNetworkReply is deleted on its own thread while the access manager's HTTP thread still runs the QObject destructor chain of the
        # delegate (vptr report in ~QNetworkReply / ~QObject). The reply comes from a request call.
        'name': 'QNetworkReply destructor chain',
        'match': (('QNetworkReply::~QNetworkReply', None),),
        'entries': QNAM_REQUESTS,
    },
    {   # QtDBus's own thread dispatches posted events while the main thread runs ~QCoreApplication at exit and clears
        # QCoreApplication::self. The main-thread side is the destructor run from main(); no entry point is needed.
        'name': 'QtDBus thread against ~QCoreApplication',
        'match': (('QCoreApplication::~QCoreApplication', None),),
        'entries': (),
    },
    {   # The QtDBus thread destroys QDBusPendingCallWatcherHelper (vptr reset) while it finishes a call put.gd made.
        'name': 'QDBusPendingCallWatcherHelper reset',
        'match': (('QDBusPendingCallWatcherHelper::~QDBusPendingCallWatcherHelper', None),),
        'entries': DBUS_CALLS,
    },
    {   # QtDBus completes a call on its own thread and the calling thread reads the reply in sendWithReply after its wait.
        'name': 'QtDBus pending-call bookkeeping',
        'match': (('QDBusConnectionPrivate::processFinishedCall', None), ('QDBusConnectionPrivate::sendWithReply', None)),
        'entries': DBUS_CALLS,
    },
)

RUNTIME = ('libc.so', 'libstdc++.so', 'libtsan.so', 'libgcc_s.so', 'ld-linux', 'libm.so', 'libpthread.so')


def function(line):
    line = re.sub(r'^\s+#\d+ ', '', line)
    return re.sub(r' (/|\.\./|<null>).*', '', line)[:110]


def module(frame):
    found = re.search(r'\(([^()\s+]+)\+0x[0-9a-f]+\)', frame)
    return Path(found.group(1)).name if found else ''


def is_qt(frame):
    return module(frame).startswith('libQt6') or '/qtbase' in frame


def is_runtime(frame):
    return module(frame).startswith(RUNTIME)


def is_own(frame, own_prefixes):
    return any(p in frame for p in own_prefixes) or module(frame).startswith(('putgd', 'libputgd'))


def name_of(frame):
    """The function's qualified name, without arguments or a return type."""
    text = function(frame)
    text = text.split('(')[0].strip()
    return text.split(' ')[-1]


def above_stop(frames, entries):
    """The frames from the racing access down to the first stop (main(), one of `entries`, or the bottom), or None when a frame on the
    way is neither Qt nor the C, C++ or TSan runtime."""
    above = []
    for frame in frames:
        name = name_of(frame)
        if name == 'main' or name in entries:
            return above
        if not (is_qt(frame) or is_runtime(frame)):
            return None
        above.append(frame)
    return above


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
    if len(stacks) != 2 or not all(stacks) or not kind or kind.group(1) != 'data race':
        return 'external', key
    for race in KNOWN_QT_RACES:
        walks = [above_stop(frames, race['entries']) for frames in stacks]
        if any(walk is None or not walk for walk in walks):
            continue  # a frame that is not Qt above the stopping point, or a stack that stops before any Qt frame
        for walk in walks:
            top = ' '.join(function(l) for l in walk[:TOP])
            whole = ' '.join(function(l) for l in walk)
            if any(name in top and (anchor is None or anchor in whole) for name, anchor in race['match']):
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
