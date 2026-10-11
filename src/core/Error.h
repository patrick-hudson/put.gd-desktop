#pragma once
#include <QJsonObject>
#include <QString>

namespace putgd {
struct Error {
    QString code;
    QString message;
    QJsonObject details;
    explicit operator bool() const { return !code.isEmpty(); }
    QJsonObject toJson() const {
        return {{QStringLiteral("code"), code}, {QStringLiteral("message"), message},
                {QStringLiteral("details"), details}};
    }
};
struct Result {
    Error error;
    bool changed = false;
    explicit operator bool() const { return !error; }
};
}
