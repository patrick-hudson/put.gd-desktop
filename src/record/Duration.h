#pragma once
#include <QString>

namespace putgd::record {
// Transport and recording bar share tenths; mask timing keeps hundredths.
inline QString durationText(qint64 us) {
    const qint64 tenths = us / 100000;
    return QStringLiteral("%1:%2.%3").arg(tenths / 600).arg((tenths / 10) % 60, 2, 10, QChar('0')).arg(tenths % 10);
}
}
