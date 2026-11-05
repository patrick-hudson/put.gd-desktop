#pragma once
#include <QJsonValue>
#include <QString>
#include <QLocale>
#include <cmath>

namespace putgd::ByteSize {
inline constexpr qint64 MiB = 1024 * 1024;
inline constexpr qint64 GiB = 1024 * MiB;

// Binary units have terminating decimal expansions. Keep every digit for editable limits.
inline QString exact(qint64 bytes, qint64 unit = MiB) {
    QString result = QString::number(bytes / unit);
    qint64 remainder = bytes % unit;
    if (remainder) {
        result += '.';
        while (remainder) {
            remainder *= 10;
            result += QChar('0' + int(remainder / unit));
            remainder %= unit;
        }
    }
    return result;
}

inline QJsonValue fromDecimal(const QString& text, qint64 unit = MiB) {
    const auto value = text.trimmed();
    QString digits;
    int decimals = 0;
    bool point = false;
    for (auto c : value) {
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        if (c < '0' || c > '9') return text;
        digits += c;
        if (point) ++decimals;
    }
    if (digits.isEmpty()) return text;

    // Multiply decimal digits by the unit before removing the decimal part. No floating-point rounding.
    qint64 carry = 0;
    for (qsizetype i = digits.size(); i > 0; --i) {
        const auto n = digits[i - 1].digitValue() * unit + carry;
        digits[i - 1] = QChar('0' + int(n % 10));
        carry = n / 10;
    }
    if (carry) digits = QString::number(carry) + digits;
    if (decimals) {
        if (digits.size() < decimals) digits = digits.rightJustified(decimals, '0');
        for (qsizetype i = digits.size() - decimals; i < digits.size(); ++i) {
            if (digits[i] != '0') return text;
        }
        digits.chop(decimals);
    }
    if (digits.isEmpty()) digits = "0";
    bool ok = false;
    const auto bytes = digits.toLongLong(&ok);
    return ok ? QJsonValue(bytes) : QJsonValue(text);
}

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
    if (bytes < 1024) return QString("%1 B").arg(bytes);
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
