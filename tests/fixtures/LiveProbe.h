#pragma once
#include "app/OperationService.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QThread>
#include <cstdio>
#include <unistd.h>
namespace putgd::test::probe {
// Helpers shared by the opt-in upload probes in tests/performance (live B2 and S3,
// the local S3 measurements). The probes differ in the worker they watch and in how
// long they wait for one reply, so both are parameters.

// One JSON line on stdout, flushed.
inline void report(const QJsonObject& line){printf("%s\n",QJsonDocument(line).toJson(QJsonDocument::Compact).constData());fflush(stdout);}
// Runs the event loop until `done` holds; false when `timeoutMs` passes first.
inline bool spin(const std::function<bool()>& done,qint64 timeoutMs){
 QElapsedTimer clock;
 clock.start();
 while(!done()) {
  if(clock.elapsed()>timeoutMs)return false;
  QCoreApplication::processEvents(QEventLoop::AllEvents,50);
  QThread::msleep(20);
 }
 return true;
}
// Starts an asynchronous call and spins for its reply; `probe_timeout` when none comes in `timeoutMs`.
inline std::pair<Error,QJsonObject> call(const std::function<void(JsonReply)>& start,qint64 timeoutMs){
 bool done=false;
 Error error;
 QJsonObject result;
 start([&](Error e,QJsonObject r){error=e;result=r;done=true;});
 if(!spin([&]{return done;},timeoutMs))return {{"probe_timeout","No reply in time",{}},{}};
 return {error,result};
}
// The first `field` line of a /proc status file, in KiB (0 when it cannot be read).
inline qint64 statusKiB(const QString& path,const QByteArray& field){
 QFile file(path);
 if(!file.open(QIODevice::ReadOnly))return 0;
 for(const auto& line:file.readAll().split('\n'))
  if(line.startsWith(field))return line.mid(field.size()).trimmed().split(' ').first().toLongLong();
 return 0;
}
inline qint64 rssKiB(qint64 pid){return statusKiB(QString("/proc/%1/status").arg(pid),"VmRSS:");}
// The children of this process whose name (the kernel's 15-character comm, e.g.
// "putgd-b2-worke") is `comm`.
inline QList<qint64> workers(const QByteArray& comm){
 QList<qint64> found;
 for(const auto& entry:QDir("/proc").entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
  QFile stat("/proc/"+entry+"/stat");
  if(!stat.open(QIODevice::ReadOnly))continue;
  const auto fields=stat.readAll();
  const auto close=fields.lastIndexOf(')');
  if(fields.left(close).contains("("+comm)&&fields.mid(close+2).split(' ').value(1).toLongLong()==getpid())found.append(entry.toLongLong());
 }
 return found;
}
}
