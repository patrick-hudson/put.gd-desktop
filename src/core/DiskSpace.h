#pragma once
#include <QtGlobal>

namespace putgd {
// Shared by artifact admission and the transient recording helper.
inline qint64 systemDiskReserve(qint64 total) {
    constexpr qint64 gib = 1LL << 30;
    return qMax(gib, qMin(total > 0 ? total / 20 : 0, 4 * gib));
}
}
