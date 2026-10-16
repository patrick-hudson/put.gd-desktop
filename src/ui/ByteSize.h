#pragma once
#include <QJsonValue>
#include <QString>

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
    const auto suffix = bytes >= GiB ? QString(" GiB") : QString(" MiB");
    auto whole = bytes / unit;
    auto hundredths = ((bytes % unit) * 100 + unit / 2) / unit;
    if (bytes > 0 && !whole && !hundredths) return "<0.01 MiB";
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
}
