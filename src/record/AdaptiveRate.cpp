#include "AdaptiveRate.h"
#include <QFile>
#include <QList>
#include <algorithm>
#include <unistd.h>

namespace putgd::record {
AdaptiveRate::AdaptiveRate(int configuredFps, bool enabled, bool fixedRateSource)
    : configured_(configuredFps), current_(configuredFps), enabled_(enabled), fixedRateSource_(fixedRateSource) {}

int AdaptiveRate::lower(int fps) {
    if (fps <= floorFps) return fps;
    return std::max(floorFps, fps / 2);
}

QString AdaptiveRate::judge(const Window& window) const {
    if (window.durationNs <= 0 || current_ <= 0) return {};
    const double seconds = double(window.durationNs) / 1e9;
    if (window.sourceFrames > 0 && window.leaked >= 3 && double(window.leaked) > leakedShare * double(window.sourceFrames)) return QStringLiteral("encoder");
    // ximagesrc keeps grabbing at the configured rate; the gate behind it keeps
    // the current one, so the grabs are measured against the configured rate.
    if (fixedRateSource_ && double(window.sourceFrames) < captureShare * configured_ * seconds) return QStringLiteral("capture");
    if (window.systemBusy >= busyShare && window.recorderCores >= recorderCoresBusy) return QStringLiteral("cpu");
    return {};
}

bool AdaptiveRate::observe(const Window& window) {
    if (!enabled_ || lower(current_) == current_) return false;
    if (++windows_ <= warmupWindows) return false;
    const auto why = judge(window);
    if (why.isEmpty()) { bad_ = 0; return false; }
    if (++bad_ < badWindows) return false;
    current_ = lower(current_);
    reason_ = why;
    windows_ = bad_ = 0;
    return true;
}

CpuSample readCpu() {
    CpuSample sample;
    QFile stat(QStringLiteral("/proc/stat"));
    if (stat.open(QIODevice::ReadOnly)) {
        // cpu user nice system idle iowait irq softirq steal ...
        const auto fields = stat.readLine().simplified().split(' ');
        if (fields.size() >= 6 && fields[0] == "cpu") {
            qint64 total = 0;
            for (int i = 1; i < fields.size() && i <= 8; ++i) total += fields[i].toLongLong();
            sample.totalTicks = total;
            sample.idleTicks = fields[4].toLongLong() + fields[5].toLongLong();
        }
    }
    QFile self(QStringLiteral("/proc/self/stat"));
    if (self.open(QIODevice::ReadOnly)) {
        // The command name may hold spaces; utime and stime are fields 14 and 15.
        const auto line = self.readAll();
        const auto close = line.lastIndexOf(')');
        if (close > 0) {
            const auto fields = line.mid(close + 2).simplified().split(' ');
            if (fields.size() > 12) sample.processTicks = fields[11].toLongLong() + fields[12].toLongLong();
        }
    }
    return sample;
}

void cpuShares(const CpuSample& before, const CpuSample& after, qint64 elapsedNs, double& systemBusy, double& processCores) {
    systemBusy = processCores = -1;
    if (before.totalTicks >= 0 && after.totalTicks > before.totalTicks) {
        const double total = double(after.totalTicks - before.totalTicks);
        const double idle = double(std::max<qint64>(0, after.idleTicks - before.idleTicks));
        systemBusy = std::clamp(1.0 - idle / total, 0.0, 1.0);
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    if (before.processTicks >= 0 && after.processTicks >= before.processTicks && elapsedNs > 0 && ticks > 0)
        processCores = double(after.processTicks - before.processTicks) / double(ticks) / (double(elapsedNs) / 1e9);
}
}
