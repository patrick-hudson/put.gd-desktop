#pragma once
#include <QFile>
namespace putgd {
// Parent PID from /proc/PID/stat, read after the last ')' because the command
// name may hold spaces or parentheses. Zero when the entry can't be read.
inline qint64 parentProcess(qint64 pid){
 QFile file(QString("/proc/%1/stat").arg(pid));
 if(pid<=0||!file.open(QIODevice::ReadOnly))return 0;
 const auto bytes=file.read(4097);if(bytes.size()>4096)return 0;
 const auto end=bytes.lastIndexOf(')');if(end<0)return 0;
 const auto fields=bytes.mid(end+2).simplified().split(' ');if(fields.size()<2)return 0;
 bool ok=false;const auto parent=fields[1].toLongLong(&ok);
 return ok&&parent>0?parent:0;
}
inline qint64 ancestorProcess(qint64 pid,int depth){
 for(int i=0;i<depth&&pid>0;++i)pid=parentProcess(pid);
 return pid;
}
inline QString processStartTicks(qint64 pid) {
 QFile file(QString("/proc/%1/stat").arg(pid));
 if(pid<=0 || !file.open(QIODevice::ReadOnly)) return {};
 const auto bytes=file.read(4097); if(bytes.size()>4096) return {};
 const auto end=bytes.lastIndexOf(')'); if(end<0) return {};
 const auto fields=bytes.mid(end+2).simplified().split(' '); if(fields.size()<20) return {};
 bool ok=false; const auto ticks=fields[19].toULongLong(&ok);
 return ok && ticks>0?QString::number(ticks):QString{};
}
}
