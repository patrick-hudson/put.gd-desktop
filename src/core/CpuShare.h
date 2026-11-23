#pragma once
#include <QFile>
#include <QThread>
#include <algorithm>

// How much of the machine heavy work may use (#1937). By default put.gd takes
// a share of the CPUs and leaves the rest to the desktop; Unhinged mode
// (preferences.unhingedMode) takes every logical CPU. A share is never lower
// with the switch on than with it off.
namespace putgd::cpu {
inline int logicalCpus() { return std::max(1, QThread::idealThreadCount()); }
// Two thirds of the CPUs, at least minimum and at most maximum; with allCores,
// every CPU (and never fewer than the share).
inline int share(int cores, int minimum, int maximum, bool allCores) {
    const int bounded = std::clamp(std::max(1, cores) * 2 / 3, minimum, maximum);
    return allCores ? std::max(cores, bounded) : bounded;
}
inline int share(int minimum, int maximum, bool allCores) { return share(logicalCpus(), minimum, maximum, allCores); }
// MemAvailable from /proc/meminfo in bytes, or -1 when it can't be read.
inline qint64 memoryAvailable() {
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly)) return -1;
    for (const auto& line : file.readAll().split('\n'))
        if (line.startsWith("MemAvailable:")) {
            bool ok = false;
            const qint64 kib = line.mid(13).trimmed().split(' ').value(0).toLongLong(&ok);
            return ok && kib > 0 ? kib * 1024 : -1;
        }
    return -1;
}
// Unhinged mode's memory guard for a pool of parallel workers: one per CPU, but
// never more than half the available memory allows at perWorker bytes each,
// and at least one. Unknown memory (-1) keeps a single worker.
inline int workersWithin(int cores, qint64 available, qint64 perWorker) {
    if (available < 0 || perWorker <= 0) return 1;
    return int(std::clamp<qint64>(available / 2 / perWorker, 1, std::max(1, cores)));
}
}
