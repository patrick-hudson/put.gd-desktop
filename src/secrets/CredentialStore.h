#pragma once
#include "core/Error.h"
#include <QObject>
#include <QMutex>
#include <QHash>
#include <QMap>
#include <functional>
#include <optional>
namespace putgd {
// Named values of one destination (#1361): how many one entry may hold, and their
// combined size in characters, names included. Setup and the store apply the same two.
inline constexpr int maximumNamedValues = 160;
inline constexpr qsizetype maximumNamedValueChars = 131072;
struct Credentials {
    QString accessKey, secretKey, sessionToken;
    quint64 generation = 0;
    // A custom HTTP uploader's secret header, parameter and argument values by their
    // credential-store key (#1361); such an entry has no access or secret key.
    QMap<QString, QString> values = {};
};
// Only explicit setup calls perform secret-store IO. Request workers read this cache.
class CredentialStore : public QObject {
public:
    explicit CredentialStore(QObject* parent = nullptr);
    void setSession(const QString& profileId, Credentials credentials);
    void forget(const QString& profileId, const QJsonObject& reference);
    std::optional<Credentials> cached(const QString& profileId, const QJsonObject& reference) const;
    void unlock(const QString& profileId, const QJsonObject& reference, std::function<void(Error)>);
    void save(const QString& profileId, const QJsonObject& reference, Credentials, std::function<void(Error)>);
private:
    static QString cacheKey(const QString&, const QJsonObject&);
    void remember(const QString&, Credentials);
    mutable QMutex mutex_;
    QHash<QString, Credentials> cache_;
    quint64 generation_ = 0;
};
}
