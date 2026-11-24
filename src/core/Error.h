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
    // A save wrote the file. With an error beside it, the write went through and only
    // reading the configuration back failed: what was saved is on disk.
    bool written = false;
    explicit operator bool() const { return !error; }
};
}
