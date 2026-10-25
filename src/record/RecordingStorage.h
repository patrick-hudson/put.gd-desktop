#pragma once
#include "core/DiskSpace.h"
#include <QJsonObject>
#include <limits>

namespace putgd::record {
// Sample the working clip's filesystem, even while paused. The extra headroom
// covers queued media and muxer finalization; the two copies are an estimate,
// not a reservation against other writers or a promise about export size.
struct RecordingStorage {
    static constexpr qint64 headroomBytes = 64LL << 20;
    static constexpr int pollMs = 250;
    qint64 freeBytes = -1, totalBytes = -1, clipBytes = 0;
    qint64 reserveBytes() const { return systemDiskReserve(totalBytes); }
    qint64 availableBytes() const { return freeBytes < 0 ? -1 : qMax<qint64>(0, freeBytes - reserveBytes()); }
    qint64 requiredBytes() const {
        const auto copy = qMax(headroomBytes, clipBytes);
        const auto maximum = std::numeric_limits<qint64>::max();
        return copy > (maximum - headroomBytes) / 2 ? maximum : 2 * copy + headroomBytes;
    }
    bool low() const { return freeBytes >= 0 && availableBytes() <= requiredBytes(); }
    QJsonObject toJson() const {
        return {{"freeBytes", freeBytes}, {"reserveBytes", reserveBytes()}, {"availableBytes", availableBytes()},
                {"requiredBytes", requiredBytes()}, {"finalizationReserveBytes", headroomBytes}, {"clipBytes", clipBytes}};
    }
};
}
