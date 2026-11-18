#!/usr/bin/env python3
"""Group ThreadSanitizer reports, and excuse only the Qt-internal races on an exact, checked-in known list.

TSan with log_path writes one file per process (tsan.<pid>). Reports are captured unsuppressed (tests/sanitizer/tsan.supp holds only
the D-Bus lock-order rule). This lists each distinct report once with how often it occurred, in three parts:

  put.gd     a put.gd frame in either racing access stack: read these first
  known      a report on the known list below (KNOWN_QT_RACES): excused
  other      everything else, Qt, GLib or library internals included; read it, and add it to the list only after diagnosing it

A report is excused only when all of this holds:
  - it is a data race with two racing access stacks;
  - the top SIGNATURE_DEPTH frames of the first racing stack, and of the second, are exactly the symbols of one known entry, in that
    order (a stack shorter than that must match entirely);
  - neither stack has a put.gd frame above main(): a frame whose module is a put.gd binary (putgd, putgd-*, libputgd*) or whose
    symbol is in the putgd namespace.
Symbols are the demangled names TSan prints, with arguments, never paths. Anything else is shown. Thread-creation, mutex and
allocation stacks are not read.

To add an entry: diagnose the report and write the diagnosis down in docs/testing/thread-sanitizer.md, then print its signature with
--signatures and copy it into KNOWN_QT_RACES with a comment that names the diagnosis. The action-sequence fuzzer uses unexplained()
to fail a seed on every report that is not on the list.

  python3 -I tools/lab/tsan_summary.py "$report_dir"                 # $report_dir: a folder of tsan.<pid> files (a seed's san/)
  python3 -I tools/lab/tsan_summary.py --signatures "$report_dir"    # the signature of every report that is not excused
"""
import argparse
import collections
from pathlib import Path
import re
import sys

SIGNATURE_DEPTH = 6
OWN_MODULES = ('putgd', 'libputgd')  # module (binary or library) name prefixes of put.gd's own code
OWN_NAMESPACE = 'putgd::'

# The diagnosed Qt-internal races, as (top frames of the first racing stack, top frames of the second), copied from real reports with
# --signatures. Each entry names its diagnosis.
KNOWN_QT_RACES = (
    # QNetworkAccessManager hand-off (docs/testing/thread-sanitizer.md): the access manager starts its HTTP thread and then connects to
    # that QThread's started() from the calling thread (QNetworkReplyHttpImplPrivate::postRequest); the new thread reads the connection
    # list with a relaxed load in maybeSignalConnected while connect() publishes it under the signal mutex. Three report shapes, seen
    # on m74 (Qt 6.4.2, GCC 13 TSan) on 2026-11-18.
    ((
        'std::__atomic_base<QObjectPrivate::SignalVector*>::load(std::memory_order) const',
        'std::atomic<QObjectPrivate::SignalVector*>::load(std::memory_order) const',
        'QObjectPrivate::SignalVector* QAtomicOps<QObjectPrivate::SignalVector*>::loadRelaxed<QObjectPrivate::SignalVector*>(std::atomic<QObjectPrivate::SignalVector*> const&)',
        'QBasicAtomicPointer<QObjectPrivate::SignalVector>::loadRelaxed() const',
        'QObjectPrivate::maybeSignalConnected(unsigned int) const',
        'void doActivate<false>(QObject*, int, void**)',
    ), (
        'std::__atomic_base<QObjectPrivate::SignalVector*>::__atomic_base(QObjectPrivate::SignalVector*)',
        'std::atomic<QObjectPrivate::SignalVector*>::atomic(QObjectPrivate::SignalVector*)',
        'QBasicAtomicPointer<QObjectPrivate::SignalVector>::QBasicAtomicPointer(QObjectPrivate::SignalVector*)',
        'QAtomicPointer<QObjectPrivate::SignalVector>::QAtomicPointer(QObjectPrivate::SignalVector*)',
        'QObjectPrivate::ConnectionData::ConnectionData()',
        'QObjectPrivate::ensureConnectionData()',
    )),
    ((
        'std::__atomic_base<QObjectPrivate::Connection*>::load(std::memory_order) const',
        'std::atomic<QObjectPrivate::Connection*>::load(std::memory_order) const',
        'QObjectPrivate::Connection* QAtomicOps<QObjectPrivate::Connection*>::loadAcquire<QObjectPrivate::Connection*>(std::atomic<QObjectPrivate::Connection*> const&)',
        'QBasicAtomicPointer<QObjectPrivate::Connection>::loadAcquire() const',
        'QObjectPrivate::maybeSignalConnected(unsigned int) const',
        'void doActivate<false>(QObject*, int, void**)',
    ), (
        'std::__atomic_base<QObjectPrivate::Connection*>::__atomic_base(QObjectPrivate::Connection*)',
        'std::atomic<QObjectPrivate::Connection*>::atomic(QObjectPrivate::Connection*)',
        'QBasicAtomicPointer<QObjectPrivate::Connection>::QBasicAtomicPointer(QObjectPrivate::Connection*)',
        'QAtomicPointer<QObjectPrivate::Connection>::QAtomicPointer(QObjectPrivate::Connection*)',
        'QObjectPrivate::ConnectionList::ConnectionList()',
        'QObjectPrivate::ConnectionData::resizeSignalVector(unsigned int)',
    )),
    ((
        'QObjectPrivate::SignalVector::count() const',
        'QObjectPrivate::ConnectionData::signalVectorCount() const',
        'QObjectPrivate::maybeSignalConnected(unsigned int) const',
        'void doActivate<false>(QObject*, int, void**)',
        'QMetaObject::activate(QObject*, QMetaObject const*, int, void**)',
        'QThread::started(QThread::QPrivateSignal)',
    ), (
        'QObjectPrivate::ConnectionData::resizeSignalVector(unsigned int)',
        'QObjectPrivate::addConnection(int, QObjectPrivate::Connection*)',
        'QMetaObjectPrivate::connect(QObject const*, int, QMetaObject const*, QObject const*, int, QMetaObject const*, int, int*)',
        'QObject::connect(QObject const*, char const*, QObject const*, char const*, Qt::ConnectionType)',
        'QNetworkReplyHttpImplPrivate::postRequest(QNetworkRequest const&)',
        'QNetworkReplyHttpImplPrivate::_q_startOperation()',
    )),
)

FRAME = re.compile(r'^\s+#\d+ ')


def symbol(line):
    """The demangled symbol of a frame line, with its arguments: no frame number, source location, module or build ID."""
    text = FRAME.sub('', line.rstrip())
    text = re.sub(r'\s+\(BuildId: [0-9a-f]+\)$', '', text)
    text = re.sub(r'\s+\([^()\s]+\+0x[0-9a-f]+\)$', '', text)
    return re.sub(r'\s+(/|\.\./)\S*$', '', text)


def module(line):
    found = re.search(r'\(([^()\s]+)\+0x[0-9a-f]+\)(\s+\(BuildId: [0-9a-f]+\))?\s*$', line.rstrip())
    return Path(found.group(1)).name if found else ''


def is_own(line, own_modules=OWN_MODULES):
    return module(line).startswith(tuple(own_modules)) or OWN_NAMESPACE in symbol(line)


def reports(text):
    for block in text.split('==================\n'):
        if 'WARNING: ThreadSanitizer' in block:
            yield block


def access_stacks(block):
    accesses = [s for s in block.split('\n\n') if re.match(r'\s*(WARNING.*\n)?\s*(Write|Read|Previous|Atomic)', s)][:2]
    return [[l for l in s.split('\n') if FRAME.match(l)] for s in accesses]


def above_main(frames):
    for index, line in enumerate(frames):
        if symbol(line) == 'main' or symbol(line).startswith('main('):
            return frames[:index]
    return frames


def signature(frames):
    return tuple(symbol(l) for l in frames[:SIGNATURE_DEPTH])


def classify(block, own_modules=OWN_MODULES):
    """'put.gd', 'known' or 'other', and the report's grouping key."""
    kind = re.search(r'WARNING: ThreadSanitizer: (.*?) \(', block)
    stacks = access_stacks(block)
    key = (kind.group(1) if kind else '?', ' || '.join(symbol(frames[0])[:110] if frames else '?' for frames in stacks))
    if any(is_own(l, own_modules) for frames in stacks for l in above_main(frames)):
        return 'put.gd', key
    if kind and kind.group(1) == 'data race' and len(stacks) == 2 and all(stacks):
        if (signature(stacks[0]), signature(stacks[1])) in KNOWN_QT_RACES:
            return 'known', key
    return 'other', key


def own_modules(extra=()):
    """The built-in put.gd module prefixes plus any extra ones: an extra prefix adds to them, never replaces them."""
    return OWN_MODULES + tuple(m for m in extra if m not in OWN_MODULES)


def unexplained(path, own_modules=OWN_MODULES):
    """The reports in one tsan.<pid> file that are not on the known list."""
    return [block for block in reports(Path(path).read_text(errors='replace')) if classify(block, own_modules)[0] != 'known']


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--own-module', action='append', default=[], help='another module name prefix of put.gd code, added to %s' % ', '.join(OWN_MODULES))
    parser.add_argument('--signatures', action='store_true', help='print the signature of every report that is not excused')
    options = parser.parse_args()
    own = own_modules(options.own_module)
    groups = {name: collections.defaultdict(list) for name in ('put.gd', 'other', 'known')}
    for report in sorted(options.folder.glob('tsan.*')):
        for block in reports(report.read_text(errors='replace')):
            part, key = classify(block, own)
            groups[part][key].append(report.name)
            if options.signatures and part != 'known':
                stacks = access_stacks(block)
                if len(stacks) == 2:
                    print('%s\n%r\n' % (report.name, (signature(stacks[0]), signature(stacks[1]))))
    if options.signatures:
        return 0
    for name in ('put.gd', 'other', 'known'):
        print('=====', name, sum(len(v) for v in groups[name].values()), 'reports')
        for (kind, key), files in sorted(groups[name].items(), key=lambda item: -len(item[1])):
            print('%d x %s | %s' % (len(files), kind, key[:200]))
            print('      e.g. %s' % files[0])
    return 0


if __name__ == '__main__':
    sys.exit(main())
