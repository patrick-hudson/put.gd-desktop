#pragma once
#include <QtGlobal>
#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace putgd {
// For a child-process modifier, which runs between fork and exec: only
// async-signal-safe system calls, no Qt, allocation, logging or locks.
// Asks the kernel to send `signal` when the starting thread of the parent ends,
// and closes the race where the parent already died. PR_SET_PDEATHSIG follows
// the thread that started the child, so that thread has to outlive it.
// False when the child must not go on (the caller then calls _exit).
#ifdef Q_OS_LINUX
inline bool dieWithParent(pid_t parent, int signal) {
    return ::prctl(PR_SET_PDEATHSIG, signal, 0L, 0L, 0L) == 0 && ::getppid() == parent;
}
#endif
}
