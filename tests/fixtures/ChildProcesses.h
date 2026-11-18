#pragma once
// The live child processes of `parent` (this test by default) whose name starts with a
// prefix (as /proc shows it, at most 15 characters), for checking that a helper did not
// outlive its work.
#include <QDir>
#include <QFile>
#include <QList>
#include <unistd.h>
namespace putgd::test {
inline QList<qint64> childProcesses(const QByteArray& name, qint64 parent = qint64(::getpid())) {
    QList<qint64> found;
    for (const auto& entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const auto pid = entry.toLongLong(&numeric);
        if (!numeric) continue;
        QFile file("/proc/" + entry + "/stat");
        if (!file.open(QIODevice::ReadOnly)) continue;
        // "pid (comm) state ppid ...": the name may hold spaces and parentheses.
        const auto stat = file.readAll();
        const auto open = stat.indexOf('('), close = stat.lastIndexOf(')');
        if (open < 0 || close < open) continue;
        const auto fields = stat.mid(close + 2).split(' ');
        if (fields.size() < 2 || fields[0] == "Z" || fields[1].toLongLong() != parent) continue;
        if (stat.mid(open + 1, close - open - 1).startsWith(name.left(15))) found.append(pid);
    }
    return found;
}
// Whether a process still runs (a zombie does not).
inline bool processAlive(qint64 pid) {
    QFile file("/proc/" + QString::number(pid) + "/stat");
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto stat = file.readAll();
    const auto close = stat.lastIndexOf(')');
    return close >= 0 && stat.mid(close + 2, 1) != "Z";
}
}
