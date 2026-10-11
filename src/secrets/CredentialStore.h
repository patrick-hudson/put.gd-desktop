#pragma once
#include "core/Error.h"
#include <QObject>
#include <QMutex>
#include <QHash>
#include <functional>
#include <optional>
namespace putgd {
struct Credentials {
    QString accessKey, secretKey, sessionToken;
    quint64 generation = 0;
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
