#include "WaylandAcquisition.h"
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>

namespace putgd::record {
QString pipewireSocket(const QProcessEnvironment& env) {
    // Unset or empty: the default remote, as libpipewire reads it.
    QString remote = env.value("PIPEWIRE_REMOTE");
    if (remote.isEmpty()) remote = QStringLiteral("pipewire-0");
    for (const QString& dir : {env.value("PIPEWIRE_RUNTIME_DIR"), env.value("XDG_RUNTIME_DIR"), env.value("USERPROFILE")})
        if (!dir.isEmpty() && QFileInfo(dir + "/" + remote).exists()) return dir + "/" + remote;
    return {};
}
PipeWireConnection::~PipeWireConnection() { if (fd_ >= 0) ::close(fd_); }
bool PipeWireConnection::open(const QProcessEnvironment& env, QString* failure) {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    const QString path = pipewireSocket(env);
    if (path.isEmpty()) { if (failure) *failure = QStringLiteral("no PipeWire socket in the session"); return false; }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray name = QFile::encodeName(path);
    if (size_t(name.size()) >= sizeof(address.sun_path)) { if (failure) *failure = QStringLiteral("the PipeWire socket path is too long"); return false; }
    std::memcpy(address.sun_path, name.constData(), size_t(name.size()));
    // As libpipewire connects: a non-blocking stream socket, closed on exec.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { if (failure) *failure = QString::fromLocal8Bit(std::strerror(errno)); return false; }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        if (failure) *failure = QStringLiteral("%1: %2").arg(path, QString::fromLocal8Bit(std::strerror(errno)));
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}
QString pipewireSourceDescription(const QString& element, quint32 node, int fd, bool doTimestamp) {
    return QStringLiteral("%1 name=src %2path=%3 min-buffers=1 %4keepalive-time=1000 resend-last=true")
        .arg(element, fd >= 0 ? QStringLiteral("fd=%1 ").arg(fd) : QString())
        .arg(node)
        .arg(doTimestamp ? QStringLiteral("do-timestamp=true ") : QString());
}
FrameTime pipewireFrameTime(qint64 framePts, qint64 monotonicNow, qint64 runningNow, qint64 previous) {
    FrameTime result{runningNow, -1};
    const qint64 age = monotonicNow - framePts;
    if (framePts >= 0 && age >= -kFutureSlackNs && age <= kMaxFrameAgeNs) {
        result.ageNs = std::max<qint64>(0, age);
        result.running = runningNow - result.ageNs;
    }
    result.running = std::max<qint64>(0, result.running);
    if (previous >= 0 && result.running <= previous) result.running = previous + 1;
    return result;
}
qint64 lockedFrameTime(qint64 arrivalRunning, qint64 previous, const FrameLock& lock, bool ownFrame) {
    const qint64 arrival = std::max<qint64>(0, arrivalRunning);
    if (previous < 0 || lock.periodNs <= 0) return previous >= 0 && arrival <= previous ? previous + 1 : arrival;
    const qint64 slot = previous + lock.periodNs;
    if (arrival < slot - (ownFrame ? lock.ownEarlyNs : lock.toleranceNs)) return -1;
    if (arrival > slot + lock.toleranceNs) return arrival;
    return slot;
}
CropMargins cropMargins(const QSize& frame, const QRect& region) {
    if (frame.isEmpty() || region.isEmpty() || !QRect(QPoint(0, 0), frame).contains(region)) return {};
    return {region.x(), region.y(), frame.width() - region.x() - region.width(), frame.height() - region.y() - region.height()};
}
QSize windowCanvas(const QSize& logical, qreal scale) {
    const qreal factor = scale > 0 ? scale : 1.0;
    const auto even = [factor](int value) { return std::max(2, int(std::lround(value * factor)) & ~1); };
    return logical.isEmpty() ? QSize() : QSize(even(logical.width()), even(logical.height()));
}
}
