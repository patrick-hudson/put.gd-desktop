#pragma once
#include <QCoreApplication>
#include "platform/ProcessStat.h"
namespace putgd::x11 {
// Window freezes run in the resident. Every process it started directly owns
// put.gd windows, including the Edit & upload editor and the recording helper,
// which never register. _NET_WM_PID is client-set, so a client that lies about
// it can only get its own window refused; put.gd's Qt windows set it truthfully.
// Direct children are the supervised UI, editor and recorder (issue 103).
constexpr int residentAncestorDepth=1;
inline bool startedByResident(qint64 pid){return pid>0&&ancestorProcess(pid,residentAncestorDepth)==QCoreApplication::applicationPid();}
}
