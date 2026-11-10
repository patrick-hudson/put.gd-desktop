#include "CredentialStore.h"
#include <qt6keychain/keychain.h>
#include <QJsonDocument>
#include <QMutexLocker>
namespace putgd {
namespace {
Error storeError(QKeychain::Job* job) {
    if (!job->error()) return {};
    // Backend error strings can contain service details; expose stable categories only.
    const auto denied = job->error() == QKeychain::AccessDenied || job->error() == QKeychain::AccessDeniedByUser;
    return {denied ? "credential_access_denied" : "credential_store_unavailable",
            "Unlock the OS secret store explicitly or select session credentials", {}};
}
Error validCredentials(const Credentials& value) {
    if (!value.values.isEmpty()) {
        // Named values of a custom HTTP uploader: no key pair beside them.
        qsizetype total = 0;
        for (auto it = value.values.cbegin(); it != value.values.cend(); ++it) {
            if (it.key().isEmpty() || it.key().size() > 300 || it.value().size() > 65536) return {"credential_invalid", "A stored value is too long", {}};
            total += it.key().size() + it.value().size();
        }
        if (!value.accessKey.isEmpty() || !value.secretKey.isEmpty() || !value.sessionToken.isEmpty()) return {"credential_invalid", "Named values are stored without an access key", {}};
        if (value.values.size() > maximumNamedValues || total > maximumNamedValueChars)
            return {"credential_invalid", "This destination's secret values are too large to store together: at most 128 KB, names included", {}};
        return {};
    }
    if (value.accessKey.isEmpty() || value.secretKey.isEmpty() || value.accessKey.size() > 4096 ||
        value.secretKey.size() > 4096 || value.sessionToken.size() > 16384)
        return {"credential_invalid", "Provide an access key and secret key", {}};
    return {};
}
QByteArray encode(const Credentials& value) {
    if (value.values.isEmpty())
        return QJsonDocument(QJsonObject{{"accessKey", value.accessKey}, {"secretKey", value.secretKey}, {"sessionToken", value.sessionToken}}).toJson(QJsonDocument::Compact);
    QJsonObject values;
    for (auto it = value.values.cbegin(); it != value.values.cend(); ++it) values[it.key()] = it.value();
    return QJsonDocument(QJsonObject{{"values", values}}).toJson(QJsonDocument::Compact);
}
Credentials decode(const QByteArray& bytes) {
    const auto object = QJsonDocument::fromJson(bytes).object();
    Credentials value{object["accessKey"].toString(), object["secretKey"].toString(), object["sessionToken"].toString()};
    const auto values = object["values"].toObject();
    for (auto it = values.begin(); it != values.end(); ++it) if (it.value().isString()) value.values.insert(it.key(), it.value().toString());
    return value;
}
}
CredentialStore::CredentialStore(QObject* parent) : QObject(parent) {}
QString CredentialStore::cacheKey(const QString& profile, const QJsonObject& reference) {
    const auto mode = reference["mode"].toString();
    const auto name = reference["reference"].toString();
    if (mode == "keychain" && !name.isEmpty() && name.size() <= 256)
        return "keychain:" + name;
    if (mode == "session" && reference["reference"].isNull())
        return "session:" + profile;
    return {};
}
void CredentialStore::remember(const QString& key, Credentials value) {
    QMutexLocker lock(&mutex_);
    value.generation = ++generation_;
    cache_.insert(key, std::move(value));
}
void CredentialStore::setSession(const QString& profile, Credentials value) {
    if (!validCredentials(value)) remember("session:" + profile, std::move(value));
}
void CredentialStore::forget(const QString& profile, const QJsonObject& reference) {
    QMutexLocker lock(&mutex_);
    cache_.remove(cacheKey(profile, reference));
}
std::optional<Credentials> CredentialStore::cached(const QString& profile, const QJsonObject& reference) const {
    QMutexLocker lock(&mutex_);
    const auto key = cacheKey(profile, reference);
    if (key.isEmpty()) return std::nullopt;
    const auto it = cache_.constFind(key);
    if (it == cache_.cend()) return std::nullopt;
    return *it;
}
void CredentialStore::unlock(const QString& profile, const QJsonObject& reference, std::function<void(Error)> reply) {
    if (reference["mode"] != "keychain" || cacheKey(profile, reference).isEmpty()) {
        reply({"credential_setup_required", "Select and provide credentials explicitly", {}});
        return;
    }
    auto* job = new QKeychain::ReadPasswordJob("putgd", this);
    job->setInsecureFallback(false);
    job->setKey(reference["reference"].toString());
    connect(job, &QKeychain::Job::finished, this, [this, profile, reference, reply](QKeychain::Job* base) {
        auto* read = static_cast<QKeychain::ReadPasswordJob*>(base);
        if (auto error = storeError(read)) {
            reply(error);
            return;
        }
        auto value = decode(read->binaryData());
        if (auto error = validCredentials(value)) {
            reply(error);
            return;
        }
        remember(cacheKey(profile, reference), std::move(value));
        reply({});
    });
    job->start();
}
void CredentialStore::save(const QString& profile, const QJsonObject& reference, Credentials value, std::function<void(Error)> reply) {
    if (auto error = validCredentials(value)) {
        reply(error);
        return;
    }
    if (reference["mode"] == "session" && !cacheKey(profile, reference).isEmpty()) {
        setSession(profile, std::move(value));
        reply({});
        return;
    }
    if (reference["mode"] != "keychain" || cacheKey(profile, reference).isEmpty()) {
        reply({"credential_setup_required", "Select a credential reference", {}});
        return;
    }
    auto* job = new QKeychain::WritePasswordJob("putgd", this);
    job->setInsecureFallback(false);
    job->setKey(reference["reference"].toString());
    job->setBinaryData(encode(value));
    connect(job, &QKeychain::Job::finished, this, [this, profile, reference, value, reply](QKeychain::Job* base) {
        if (auto error = storeError(base)) {
            reply(error);
            return;
        }
        remember(cacheKey(profile, reference), value);
        reply({});
    });
    job->start();
}
}
