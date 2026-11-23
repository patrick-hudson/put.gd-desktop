#pragma once
#include "Error.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

namespace putgd {
// History's poster frame for a stored video (#112), made by the transient
// `putgd-record --poster` helper so the resident never links a media library.
// The file is opened here and handed over as the child's standard input; the
// child gets bounded address space, CPU time and file descriptors, no files to
// write, low priority, and is killed at the deadline. Its PNG output is capped
// at the preview budget and checked before use. Synchronous: call it from a
// background worker, never the resident event loop.
class PosterJob final {
public:
 using Cancel=std::shared_ptr<std::atomic_bool>;
 // allCores (Unhinged mode, #1937): the child decodes on every CPU at normal
 // priority, with CPU time for that many threads; its memory bound is the same.
 explicit PosterJob(QString helperPath={},int timeoutMs=10000,bool allCores=false);
 static QString defaultHelper();
 // The child's arguments, CPU-time limit in seconds and niceness.
 static QStringList arguments(qint64 maximumPixels,bool allCores);
 static int cpuSeconds(int timeoutMs,bool allCores,int cores);
 static int niceness(bool allCores);
 // A PNG whose long edge is at most 256 px, or empty with error set.
 QByteArray render(const QString& file,qint64 maximumPixels,qint64 budget,Error&,Cancel={}) const;
private:
 QString helperPath_;
 int timeoutMs_;
 bool allCores_=false;
};
}
