#pragma once
#include <QProcess>
#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace putgd {
// Call immediately before start(), on the UI main thread. These helpers must
// not outlive the UI even when it exits without running QObject destructors.
inline void terminateWithParent(QProcess& process) {
#ifdef Q_OS_LINUX
    const auto parent = ::getpid();
    process.setChildProcessModifier([parent] {
        // Qt runs this only in the child, between fork and exec. Use only
        // low-level system calls: no Qt, allocation, logging or locks here.
        if (::prctl(PR_SET_PDEATHSIG, SIGTERM, 0L, 0L, 0L) != 0 || ::getppid() != parent)
            ::_exit(127); // Also close the race where the parent already died.
    });
#else
    Q_UNUSED(process);
#endif
}
}
