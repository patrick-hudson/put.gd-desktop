#include "core/ByteSize.h"
#include "SettingsService.h"
#include "Log.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QPointer>

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
    if (file.size() > maximumConfigBytes) return {"config_too_large", ByteSize::labels("Configuration exceeds the 1 MiB limit."), {}};
    bytes = file.read(maximumConfigBytes + 1);
    if (file.error() != QFile::NoError) return {"config_unreadable", "Cannot read the configuration file.", {}};
    if (bytes.size() > maximumConfigBytes) return {"config_too_large", ByteSize::labels("Configuration exceeds the 1 MiB limit."), {}};
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
struct LoadedSettings { QJsonObject candidate; QString revision; Error error; };
LoadedSettings loadValidated(const QString& path,const ProfileValidator& validator,const CredentialValidator& credentialValidator) {
    LoadedSettings loaded;
    QByteArray bytes;
    bool exists = false;
    auto error = readConfig(path, bytes, exists);
    loaded.revision = exists && !error ? revisionOf(bytes) : QString{};
    if (!error && !exists) error = {"config_missing", "Choose or restore a configuration in setup.", {}};
    QJsonObject candidate;
    if (!error) {
        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject())
            error = {"config_invalid", "Configuration must be a valid JSON object.", {{"offset", parseError.offset}}};
        else {
            candidate = json.object();
            error = Settings::validate(candidate, validator, credentialValidator);
        }
    }
    loaded.candidate=candidate;loaded.error=error;return loaded;
}
// A destination keeps the rules it was saved under while the file holds it with the same id,
// provider and settings, so fixing one destination or any other setting never needs every
// other one fixed first. Any other destination, a copy under a new id included, meets
// today's rules. `current` is the file as the revision check read it.
Error currentRules(const QJsonObject& proposed,const QByteArray& current,const ProfileValidator& validator) {
    QHash<QString,QJsonObject> saved;
    for(const auto& entry:Settings::normalized(QJsonDocument::fromJson(current).object())["profiles"].toArray())
        saved.insert(entry.toObject()["id"].toString(),entry.toObject());
    for(const auto& entry:Settings::normalized(proposed)["profiles"].toArray()) {
        const auto profile=entry.toObject();const auto kept=saved.value(profile["id"].toString());
        if(!kept.isEmpty()&&kept["provider"]==profile["provider"]&&kept["settings"]==profile["settings"])continue;
        if(auto error=validator(profile["provider"].toString(),profile["settings"].toObject()))return error;
    }
    return {};
}
Error writeCandidate(const QString& path,const ProfileValidator& validator,const ProfileValidator& stored,const CredentialValidator& credentialValidator,const QJsonObject& proposed,const QString& expectedRevision) {
    // What was sent is checked as sent, so 201 repeats of one word are refused like 201 words; only then
    // are Quick redact's saved lists written trimmed and without repeats, whoever sent them (#1618).
    // The rules a saved destination met first; today's rules once the file is locked and read.
    if (auto error = Settings::validate(proposed, stored ? stored : validator, credentialValidator)) return error;
    const auto candidate = Settings::withCleanRedactLists(proposed);
    const auto bytes = QJsonDocument(candidate).toJson(QJsonDocument::Indented);
    if (bytes.size() > maximumConfigBytes) return {"config_too_large", ByteSize::labels("Configuration exceeds the 1 MiB limit."), {}};
    const auto parent = QFileInfo(path).absolutePath();
    if (!QDir(parent).exists()) {
        if (!QDir().mkpath(parent) || !QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
            return {"config_unwritable", "Cannot create a private configuration directory.", {}};
    }
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(0)) {
        if (lock.error() == QLockFile::LockFailedError)
            return {"config_busy", "Another cooperative writer holds the configuration lock.", {}};
        return {"config_unwritable", "Cannot create the configuration lock file.", {}};
    }
    QByteArray current;
    bool exists = false;
    if (auto error = readConfig(path, current, exists)) return error;
    const auto actualRevision = exists ? revisionOf(current) : QString{};
    if (actualRevision != expectedRevision)
        return {"config_conflict", "Configuration changed; reload or rebase before saving.", {{"observedRevision", actualRevision}}};
    if (validator)
        if (auto error = currentRules(proposed, current, validator)) return error;
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        output.write(bytes) != bytes.size() || !output.commit())
        return {"config_unwritable", "Atomic configuration save failed; prior bytes were preserved.", {}};
    return {};
}

}

SettingsService::SettingsService(QString path, ProfileValidator validator, QObject* parent, CredentialValidator credentialValidator, ProfileValidator storedValidator)
    : QObject(parent), path_(QFileInfo(path).absoluteFilePath()), validator_(std::move(validator)), storedValidator_(storedValidator?std::move(storedValidator):validator_), credentialValidator_(std::move(credentialValidator)),
      settings_(Settings::fromValidated(Settings::defaults())) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(75);
    fallback_.setInterval(2000);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &SettingsService::scheduleReload);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &SettingsService::scheduleReload);
    connect(&debounce_, &QTimer::timeout, this, [this] { reloadAsync(); });
    connect(&fallback_, &QTimer::timeout, this, [this] { if (!watching_) reloadAsync(); });
    worker_=new QObject;worker_->moveToThread(&workerThread_);
    connect(&workerThread_,&QThread::finished,worker_,&QObject::deleteLater);workerThread_.start();
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

// The JSON pointer and message only: a rejected value never reaches the log.
static void logRejected(const Error& error,const char* stage) {
    // A stale expected revision or a busy lock is an ordinary retry, not a rejection.
    if(error.code=="config_conflict"||error.code=="config_busy")return;
    logging::warning(logSettings(),"Configuration rejected",{{"stage",stage},{"code",error.code},{"pointer",error.details["path"]},{"message",error.message.left(512)}});
}
Result SettingsService::acceptLoaded(const QJsonObject& candidate,const QString& revision,const Error& error,quint64 sequence) {
    if(sequence<appliedSequence_)return {error,false};
    if(error&&(error.code!=lastError_.code||error.details!=lastError_.details||revision!=observedRevision_))logRejected(error,"load");
    const auto before=metadata();
    appliedSequence_=sequence;observedRevision_=revision;
    bool changed=false;
    if(!error&&revision!=acceptedRevision_) {settings_=Settings::fromValidated(candidate);acceptedRevision_=revision;changed=true;}
    lastError_=error;armWatcher();if(changed)emit accepted(acceptedRevision_);
    // An unchanged rescan (the fallback timer, or a watcher event for a
    // neighbouring file) is not news: each emission refreshes every open view (#234).
    if(changed||metadata()!=before)emit statusChanged();
    return {error,changed};
}
Result SettingsService::reload() {
    std::lock_guard guard(ioMutex_);auto loaded=loadValidated(path_,storedValidator_,credentialValidator_);
    return acceptLoaded(loaded.candidate,loaded.revision,loaded.error,++sequence_);
}
Result SettingsService::save(const QJsonObject& candidate,const QString& expectedRevision) {
    std::lock_guard guard(ioMutex_);if(auto error=writeCandidate(path_,validator_,storedValidator_,credentialValidator_,candidate,expectedRevision)){logRejected(error,"save");return {error};}
    auto loaded=loadValidated(path_,storedValidator_,credentialValidator_);auto result=acceptLoaded(loaded.candidate,loaded.revision,loaded.error,++sequence_);result.written=true;return result;
}
Result SettingsService::patch(const QJsonObject& patchObject, const QString& expectedRevision) {
    return save(overlay(document(), patchObject), expectedRevision);
}
SettingsService::~SettingsService() {workerThread_.quit();workerThread_.wait();}
void SettingsService::asyncLoad(const QJsonObject& candidate,const QString& expected,bool write,std::function<void(Result)> callback) {
    QPointer<SettingsService> guarded(this);const auto path=path_;const auto validator=validator_;const auto stored=storedValidator_;const auto credentialValidator=credentialValidator_;
    QMetaObject::invokeMethod(worker_,[guarded,path,validator,stored,credentialValidator,candidate,expected,write,callback] {
        if(!guarded)return;
        std::lock_guard guard(guarded->ioMutex_);
        Error error;if(write)error=writeCandidate(path,validator,stored,credentialValidator,candidate,expected);
        LoadedSettings loaded;if(!error)loaded=loadValidated(path,stored,credentialValidator);
        const auto sequence=++guarded->sequence_;
        QMetaObject::invokeMethod(guarded,[guarded,loaded,error,sequence,callback,write] {
            if(!guarded)return;
            if(error)logRejected(error,"save");
            auto result=error?Result{error}:guarded->acceptLoaded(loaded.candidate,loaded.revision,loaded.error,sequence);
            result.written=write&&!error;
            if(callback)callback(result);
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
void SettingsService::reloadAsync(std::function<void(Result)> callback) {asyncLoad({}, {},false,std::move(callback));}
void SettingsService::saveAsync(const QJsonObject& candidate,const QString& expected,std::function<void(Result)> callback) {asyncLoad(candidate,expected,true,std::move(callback));}
void SettingsService::patchAsync(const QJsonObject& patch,const QString& expected,std::function<void(Result)> callback) {saveAsync(overlay(document(),patch),expected,std::move(callback));}

}
