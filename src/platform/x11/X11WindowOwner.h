#pragma once
#include <QCoreApplication>
#include <QFile>
namespace putgd::x11 {
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
// Window freezes run in the resident. Every process it started directly owns
// put.gd windows, including the Edit & upload editor and the recording helper,
// which never register. _NET_WM_PID is client-set, so a client that lies about
// it can only get its own window refused; put.gd's Qt windows set it truthfully.
inline bool startedByResident(qint64 pid){return pid>0&&parentProcess(pid)==QCoreApplication::applicationPid();}
}
