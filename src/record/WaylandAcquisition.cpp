#include "WaylandAcquisition.h"
#include <QFileInfo>
#include <algorithm>

namespace putgd::record {
QString pipewireSocket(const QProcessEnvironment& env) {
    const QString remote = env.value("PIPEWIRE_REMOTE", "pipewire-0");
    for (const QString& dir : {env.value("PIPEWIRE_RUNTIME_DIR"), env.value("XDG_RUNTIME_DIR"), env.value("USERPROFILE")})
        if (!dir.isEmpty() && QFileInfo(dir + "/" + remote).exists()) return dir + "/" + remote;
    return {};
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
qint64 smoothedFrameTime(qint64 arrivalRunning, qint64 ageNs, FrameSmoothing& smoothing, qint64 previous) {
    qint64 running = arrivalRunning;
    if (ageNs >= 0) {
        if (smoothing.meanAgeNs < 0) smoothing.meanAgeNs = ageNs;
        else smoothing.meanAgeNs += (ageNs - smoothing.meanAgeNs) / kFrameSmoothingDivisor;
        running = arrivalRunning - (ageNs - smoothing.meanAgeNs);
    }
    running = std::max<qint64>(0, running);
    if (previous >= 0 && running <= previous) running = previous + 1;
    return running;
}
}
