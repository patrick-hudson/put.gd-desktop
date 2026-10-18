#include "DebugLog.h"
#include <QDateTime>
#include <QFile>
#include <QProcessEnvironment>
#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
namespace putgd::record {
void debugLog(const QString& line) {
    static const QString path = QProcessEnvironment::systemEnvironment().value("PUTGD_RECORD_DEBUG_LOG");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) return;
    file.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + " " + line + "\n").toUtf8());
}
namespace {
char crashPath[4096] = {};
void crashHandler(int signal) {
    const int fd = ::open(crashPath, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd >= 0) {
        char header[64];
        const int n = std::snprintf(header, sizeof header, "crash: signal %d\n", signal);
        if (n > 0) { const auto ignored = ::write(fd, header, size_t(n)); (void)ignored; }
        void* frames[64];
        const int count = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, count, fd);
        ::close(fd);
    }
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}
}
void installCrashLog() {
    const auto path = QProcessEnvironment::systemEnvironment().value("PUTGD_RECORD_DEBUG_LOG").toUtf8();
    if (path.isEmpty() || path.size() >= int(sizeof crashPath)) return;
    std::memcpy(crashPath, path.constData(), size_t(path.size()) + 1);
    for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE}) std::signal(s, crashHandler);
}
}
