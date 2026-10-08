#include "SettingsService.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>

namespace putgd {
namespace {
constexpr qint64 maximumConfigBytes = 1024 * 1024;
QString revisionOf(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
Error readConfig(const QString& path, QByteArray& bytes, bool& exists) {
    exists = QFile::exists(path);
    if (!exists) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {"config_unreadable", "Cannot read the configuration file.", {}};
    if (file.size() > maximumConfigBytes) return {"config_too_large", "Configuration exceeds the 1 MiB limit.", {}};
    bytes = file.read(maximumConfigBytes + 1);
    if (file.error() != QFile::NoError) return {"config_unreadable", "Cannot read the configuration file.", {}};
    if (bytes.size() > maximumConfigBytes) return {"config_too_large", "Configuration exceeds the 1 MiB limit.", {}};
    return {};
}
QJsonObject overlay(const QJsonObject& original, const QJsonObject& patch) {
    auto result = original;
    for (auto i = patch.begin(); i != patch.end(); ++i) {
        if (i.value().isObject()) result[i.key()] = overlay(result[i.key()].toObject(), i.value().toObject());
        else result[i.key()] = i.value();
    }
    return result;
}
}

SettingsService::SettingsService(QString path, ProfileValidator validator, QObject* parent)
    : QObject(parent), path_(QFileInfo(path).absoluteFilePath()), validator_(std::move(validator)),
      settings_(Settings::fromValidated(Settings::defaults())) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(75);
    fallback_.setInterval(2000);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &SettingsService::scheduleReload);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &SettingsService::scheduleReload);
    connect(&debounce_, &QTimer::timeout, this, [this] { reload(); });
    connect(&fallback_, &QTimer::timeout, this, [this] { if (!watching_) reload(); });
    reload();
    fallback_.start();
}

void SettingsService::scheduleReload() { debounce_.start(); }

void SettingsService::armWatcher() {
    const auto parent = QFileInfo(path_).absolutePath();
    if (QDir(parent).exists() && !watcher_.directories().contains(parent)) watcher_.addPath(parent);
    if (QFile::exists(path_) && !watcher_.files().contains(path_)) watcher_.addPath(path_);
    watching_ = watcher_.directories().contains(parent) && (!QFile::exists(path_) || watcher_.files().contains(path_));
}

QJsonObject SettingsService::metadata() const {
    return {{"path", path_}, {"schemaVersion", 1}, {"acceptedRevision", acceptedRevision_},
        {"observedRevision", observedRevision_}, {"watching", watching_}, {"fallbackRescanMs", 2000},
        {"validation", QJsonObject{{"ok", !lastError_}, {"error", lastError_.toJson()}}}};
}

Result SettingsService::reload() {
    QByteArray bytes;
    bool exists = false;
    auto error = readConfig(path_, bytes, exists);
    observedRevision_ = exists && !error ? revisionOf(bytes) : QString{};
    if (!error && !exists) error = {"config_missing", "Choose or restore a configuration in setup.", {}};
    QJsonObject candidate;
    if (!error) {
        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject())
            error = {"config_invalid", "Configuration must be a valid JSON object.", {{"offset", parseError.offset}}};
        else {
            candidate = json.object();
            error = Settings::validate(candidate, validator_);
        }
    }
    bool changed = false;
    if (!error && observedRevision_ != acceptedRevision_) {
        settings_ = Settings::fromValidated(candidate);
        acceptedRevision_ = observedRevision_;
        changed = true;
    }
    lastError_ = error;
    armWatcher();
    if (changed) emit accepted(acceptedRevision_);
    emit statusChanged();
    return {error, changed};
}

Result SettingsService::save(const QJsonObject& candidate, const QString& expectedRevision) {
    if (auto error = Settings::validate(candidate, validator_)) return {error};
    const auto bytes = QJsonDocument(candidate).toJson(QJsonDocument::Indented);
    if (bytes.size() > maximumConfigBytes) return {{"config_too_large", "Configuration exceeds the 1 MiB limit.", {}}};
    const auto parent = QFileInfo(path_).absolutePath();
    if (!QDir(parent).exists()) {
        if (!QDir().mkpath(parent) || !QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
            return {{"config_unwritable", "Cannot create a private configuration directory.", {}}};
    }
    QLockFile lock(path_ + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(0)) {
        if (lock.error() == QLockFile::LockFailedError)
            return {{"config_busy", "Another cooperative writer holds the configuration lock.", {}}};
        return {{"config_unwritable", "Cannot create the configuration lock file.", {}}};
    }
    QByteArray current;
    bool exists = false;
    if (auto error = readConfig(path_, current, exists)) return {error};
    const auto actualRevision = exists ? revisionOf(current) : QString{};
    if (actualRevision != expectedRevision)
        return {{"config_conflict", "Configuration changed; reload or rebase before saving.", {{"observedRevision", actualRevision}}}};
    QSaveFile output(path_);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        output.write(bytes) != bytes.size() || !output.commit())
        return {{"config_unwritable", "Atomic configuration save failed; prior bytes were preserved.", {}}};
    return reload();
}

Result SettingsService::patch(const QJsonObject& patchObject, const QString& expectedRevision) {
    return save(overlay(document(), patchObject), expectedRevision);
}
}
