#pragma once
#include <QLocale>
#include <QString>
#include <cmath>

namespace putgd::DisplaySize {
inline constexpr qint64 MiB = 1024 * 1024;
inline constexpr qint64 GiB = 1024 * MiB;
inline QString summary(qint64 bytes) {
    const auto unit = bytes >= GiB ? GiB : MiB;
    const auto suffix = bytes >= GiB ? QString(" GB") : QString(" MB");
    auto whole = bytes / unit;
    auto hundredths = ((bytes % unit) * 100 + unit / 2) / unit;
    if (bytes > 0 && !whole && !hundredths) return "<0.01 MB";
    if (hundredths == 100) {
        ++whole;
        hundredths = 0;
    }
    QString result = QString::number(whole);
    if (hundredths) {
        auto fraction = QString::number(hundredths).rightJustified(2, '0');
        while (fraction.endsWith('0')) fraction.chop(1);
        result += '.' + fraction;
    }
    return result + suffix;
}

// Traditional display labels, with the same 1024-based arithmetic everywhere.
inline QString fileSummary(qint64 bytes) {
    if (bytes < MiB) return QString("%1 KB").arg(QString::number(double(bytes) / 1024, 'f', bytes < 10 * 1024 ? 1 : 0));
    return summary(bytes);
}

inline QString localized(qint64 bytes, int precision = 1, const QLocale& locale = QLocale()) {
    return locale.formattedDataSize(bytes, precision, QLocale::DataSizeTraditionalFormat);
}
inline QString admissionLimit(qint64 bytes) {
    constexpr double mib = 1024.0 * 1024, gib = mib * 1024, tib = gib * 1024;
    if (bytes >= tib) return QString::number(bytes / tib, 'f', 1) + " TB";
    if (bytes >= gib) return QString::number(bytes / gib, 'f', 1) + " GB";
    return QString::number(qMax(1.0, std::ceil(bytes / mib)), 'f', 0) + " MB";
}
inline QString labels(QString text) {
    return text.replace(" KiB", " KB").replace(" MiB", " MB").replace(" GiB", " GB").replace(" TiB", " TB");
}
}
