#pragma once
#include "core/ParentDeath.h"
#include <QProcess>

namespace putgd {
// Call immediately before start(), on the UI main thread. These helpers must
// not outlive the UI even when it exits without running QObject destructors.
inline void terminateWithParent(QProcess& process) {
#ifdef Q_OS_LINUX
    const auto parent = ::getpid();
    process.setChildProcessModifier([parent] {
        // Qt runs this only in the child, between fork and exec. Use only
        // low-level system calls: no Qt, allocation, logging or locks here.
        if (!dieWithParent(parent, SIGTERM))
            ::_exit(127); // Also close the race where the parent already died.
    });
#else
    Q_UNUSED(process);
#endif
}
}
