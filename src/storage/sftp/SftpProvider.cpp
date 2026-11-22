#include "SftpProvider.h"
#include "storage/worker/HelperProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <future>
#include <thread>
namespace putgd {
namespace {
// A worker that answered within this long still has its minute left; its own idle exit is 60 s.
constexpr qint64 reuseWithinMs = 50000;
// The worker reports at least every 250 ms while a file moves and has a 120 s SFTP deadline.
constexpr qint64 silenceLimitMs = 200000;
// How long a cancelled request may take to finish before the helper is stopped: the worker's
// 20 s OPEN wait and its 8 s cleanup budget fit inside it.
constexpr qint64 cancelGraceMs = 30000;
Error parseError(const QJsonObject& value) { return {value["code"].toString("sftp_failed"), value["message"].toString("The SFTP request failed"), value["details"].toObject()}; }
// Only these methods sign in, so only they get the password and passphrase.
bool signsIn(const QString& method) {
    static const QStringList methods{"connect", "checkName", "upload", "stat", "reconcile", "remove", "createFolder", "testDestination"};
    return methods.contains(method);
}
// What a recorded upload needs for the identity check before a deletion (#1871).
bool recordable(const QJsonObject& object) {
    // The worker's own rule: the recorded known_hosts lines are the identity; a display fingerprint
    // may be empty (a server known only through a certificate authority).
    for (const auto* key : {"path", "host", "user", "sha256"}) if (object[key].toString().isEmpty()) return false;
    for (const auto* key : {"port", "size", "mtime", "uid"}) if (!object[key].isDouble()) return false;
    return !object["knownHosts"].toArray().isEmpty();
}
}
struct SftpProvider::Session {
    HelperProcess process;
    QElapsedTimer idle;
};
SftpProvider::SftpProvider(CredentialStore& credentials, QString knownHostsFile) : credentials_(credentials), knownHosts_(std::move(knownHostsFile)) {
    // Idle workers end when their time is up, whether or not their destination is asked again.
    reaper_ = std::thread([this] {
        std::unique_lock lock(mutex_);
        while (!stopping_.load()) {
            wake_.wait_for(lock, std::chrono::seconds(10));
            for (auto it = idle_.begin(); it != idle_.end();) {
                if (stopping_.load() || it->second->idle.elapsed() >= reuseWithinMs || !it->second->process.running()) {
                    it->second->process.closeInput();
                    it = idle_.erase(it);
                } else ++it;
            }
        }
    });
}
SftpProvider::~SftpProvider() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        wake_.notify_all();
    }
    // Every thread this provider started ends before its state goes: anticipations see
    // stopping_ and stop their worker at once.
    if (reaper_.joinable()) reaper_.join();
    std::vector<Anticipation> threads;
    {
        std::lock_guard lock(mutex_);
        threads = std::move(anticipations_);
    }
    for (auto& entry : threads) if (entry.thread.joinable()) entry.thread.join();
    std::lock_guard lock(mutex_);
    for (auto& [key, session] : idle_) session->process.closeInput();
    idle_.clear();
}
QString SftpProvider::workerPath() {
    const auto app = QDir::cleanPath(QCoreApplication::applicationDirPath());
#ifdef PUTGD_SFTP_BUILD_APP_DIR
    if (app == QString::fromUtf8(PUTGD_SFTP_BUILD_APP_DIR)) return QString::fromUtf8(PUTGD_SFTP_WORKER_PATH);
#endif
    return QDir(app).absoluteFilePath(sftp::workerName);
}
QJsonObject SftpProvider::diagnostics() const {
    std::lock_guard lock(mutex_);
    return {{"activeRequests", active_.load()}, {"idleSessions", int(idle_.size())}, {"protocolVersion", 1}};
}
ProviderCapabilities SftpProvider::receiptCapabilities(const StorageReceipt& receipt) const {
    auto caps = capabilities(receipt.destination);
    // A record without the server's identity and the file's size, time, owner and hash can't be
    // checked before a deletion, so it can't be deleted.
    if (!recordable(receipt.objectReference)) {
        caps.remove = false;
        caps.removeUnavailable = QStringLiteral("This upload's record lacks the server identity and file details put.gd checks before deleting it.");
    }
    return caps;
}
ProviderCapabilities SftpProvider::capabilities(const Destination& destination) const {
    ProviderCapabilities caps;
    caps.conditionalCreate = true;  // the final name opens with CREAT|EXCL
    caps.conditionalDelete = true;  // after the identity check below; see deletionEffect
    caps.stat = true;
    caps.remove = true;
    caps.urls = destination.settings["publicUrlBase"].isString();
    caps.maximumNameBytes = 255;
    caps.deletionEffect = QStringLiteral(
        "Removes the file after checking that it is still the one put.gd uploaded: a regular file of the same size, time and owner, and up to 32 MB the "
        "same bytes. SFTP has no conditional delete, so a file replaced between that check and the removal is removed too; web caches may keep serving it.");
    return caps;
}
QJsonObject SftpProvider::diagnosticSettings(const QJsonObject& settings) const {
    // The server, user, folder and link name private machines and people: a package says only whether each is set.
    QJsonObject out;
    for (const auto& key : {"server", "user", "folder"}) out[key] = settings[key].toString().isEmpty() ? QString() : QStringLiteral("(set)");
    out["publicUrlBase"] = settings["publicUrlBase"].isString() ? QJsonValue(QStringLiteral("(set)")) : QJsonValue(QJsonValue::Null);
    out["hostKeyFingerprint"] = settings["hostKeyFingerprint"];
    return out;
}
QString SftpProvider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    const auto& s = destination.settings;
    // The server ssh resolved and the folder's canonical path, as the name check and the receipt
    // record them: the same file has the same key however the destination names it.
    if (object["path"].isString() && object["host"].isString())
        return object["host"].toString() + "|" + QString::number(object["port"].toInt()) + "|" + object["user"].toString() + "|" + object["path"].toString();
    if (object["path"].isString()) return s["server"].toString() + "|" + s["user"].toString() + "|" + object["path"].toString();
    return s["server"].toString() + "|" + s["user"].toString() + "|" + sftp::remoteFolder(s["folder"].toString()) + "/" + candidate;
}
Error SftpProvider::validateSettings(const QJsonObject& settings) const { return sftp::validate(settings, false); }
QString SftpProvider::sessionKey(const Destination& destination) const {
    const auto credential = credentials_.cached(destination.profileId, destination.credentialReference);
    const auto material = QJsonDocument(QJsonObject{{"profile", destination.profileId}, {"settings", destination.settings}, {"credentials", destination.credentialReference},
                                                    {"generation", credential ? double(credential->generation) : -1.0}}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}
std::unique_ptr<SftpProvider::Session> SftpProvider::take(const QString& key) {
    std::unique_ptr<Session> found;
    {
        std::lock_guard lock(mutex_);
        const auto it = idle_.find(key.toStdString());
        if (it == idle_.end()) return {};
        found = std::move(it->second);
        idle_.erase(it);
    }
    if (found->idle.elapsed() < reuseWithinMs && found->process.running()) return found;
    retire(std::move(found));
    return {};
}
void SftpProvider::park(const QString& key, std::unique_ptr<Session> session) {
    session->idle.restart();
    std::unique_ptr<Session> replaced;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_.load()) {
            auto& slot = idle_[key.toStdString()];
            replaced = std::move(slot);
            slot = std::move(session);
        } else replaced = std::move(session);
    }
    retire(std::move(replaced));
}
void SftpProvider::retire(std::unique_ptr<Session> session) {
    // Its stdin closes; the worker ends its ssh connection and exits on its own.
    if (session) session->process.closeInput();
}
std::shared_ptr<void> SftpProvider::anticipate(const Destination& destination) {
    if (stopping_.load() || sftp::validate(destination.settings, true)) return {};
    std::vector<Anticipation> finished;
    {
        std::lock_guard lock(mutex_);
        if (stopping_.load() || idle_.count(sessionKey(destination).toStdString())) return {};
        // Threads that are done are joined here, so none is ever left behind.
        for (auto it = anticipations_.begin(); it != anticipations_.end();)
            if (it->done->load()) { finished.push_back(std::move(*it)); it = anticipations_.erase(it); } else ++it;
        if (anticipations_.size() >= 8) return {};
        auto done = std::make_shared<std::atomic_bool>(false);
        // Connects ahead of the requests an action is about to make; the connected worker waits
        // in the pool for them, and ends itself after a minute if none comes.
        anticipations_.push_back({std::thread([this, destination, done] {
                                      Error ignored;
                                      call("connect", destination, {}, ignored);
                                      *done = true;
                                  }),
                                  done});
    }
    for (auto& entry : finished) entry.thread.join();
    return {};
}
QJsonObject SftpProvider::call(const QString& method, const Destination& destination, QJsonObject params, Error& error, bool* uncertain,
                               std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress, DurableCheckpoint checkpoint,
                               std::shared_ptr<std::atomic_bool> kill, std::function<void(const QString&)> notice) {
    if (uncertain) *uncertain = false;
    if (stopping_.load()) { error = {"operation_cancelled", "The application is stopping", {}}; return {}; }
    if (auto e = sftp::validate(destination.settings, false)) { error = e; return {}; }
    // A keychain destination whose secrets aren't loaded still signs in with the agent and key files.
    const auto credential = credentials_.cached(destination.profileId, destination.credentialReference);
    const auto id = ++nextId_;
    params["schemaVersion"] = 1;
    params["id"] = double(id);
    params["method"] = method;
    params["destination"] = destination.toJson();
    params["knownHostsFile"] = knownHosts_;
    // Secrets go only with a request that signs in; Settings' address check learns only whether a password is set.
    if (signsIn(method))
        params["credentials"] = QJsonObject{{sftp::passwordKey(), credential ? credential->values.value(sftp::passwordKey()) : QString()},
                                            {sftp::passphraseKey(), credential ? credential->values.value(sftp::passphraseKey()) : QString()}};
    else if (method == QLatin1String("inspect")) params["passwordSet"] = credential && !credential->values.value(sftp::passwordKey()).isEmpty();
    ++active_;
    struct Release { std::atomic_int& count; ~Release() { --count; } } release{active_};
    const auto key = sessionKey(destination);
    auto session = take(key);
    if (!session) {
        session = std::make_unique<Session>();
        if (!session->process.start(workerPath(), {"--session"}, QProcessEnvironment::systemEnvironment())) {
            error = {"sftp_worker_start", "put.gd couldn't start its SFTP helper. Try again; if it keeps failing, reinstall put.gd.", {}};
            return {};
        }
    }
    auto bytes = QJsonDocument(params).toJson(QJsonDocument::Compact);
    params = {};
    bytes.append('\n');
    const bool mutation = method == QLatin1String("upload") || method == QLatin1String("remove") || method == QLatin1String("testDestination") ||
                          method == QLatin1String("createFolder");
    const auto written = session->process.write(bytes, 10000);
    bytes.fill('\0');
    if (written != bytes.size()) {
        error = {"sftp_worker_start", "The SFTP helper did not take its request. Try again.", {}};
        return {};
    }
    QByteArray pending;
    QJsonObject final;
    bool got = false, cancelSent = false, broken = false;
    QElapsedTimer silence, cancelClock;
    silence.start();
    while (!got) {
        // The provider is going away: the helper is stopped at once, not asked.
        if (stopping_.load() && method == QLatin1String("connect")) {
            session->process.kill();
            error = {"operation_cancelled", "The application is stopping", {}};
            return {};
        }
        if (kill && kill->load()) {
            session->process.kill();
            error = {"sftp_transport_unknown", "put.gd quit while the SFTP helper was working; check the server before retrying", {{"reason", "quit"}}};
            if (uncertain) *uncertain = mutation;
            return {};
        }
        if (!cancelSent && (stopping_.load() || (cancellation && cancellation->load()))) {
            session->process.write("{\"cancel\":" + QByteArray::number(id) + "}\n", 2000);
            cancelSent = true;
            cancelClock.start();
        }
        if ((cancelSent && cancelClock.elapsed() > cancelGraceMs) || silence.elapsed() > silenceLimitMs) {
            session->process.kill();
            error = {"sftp_transport_unknown", "The SFTP helper stopped responding; check the server before retrying", {{"reason", cancelSent ? "cancelled" : "deadline"}}};
            if (uncertain) *uncertain = mutation;
            return {};
        }
        if (!session->process.waitForReadable(100) && !session->process.outputOpen()) { broken = true; break; }
        const auto chunk = session->process.read(1024 * 1024);
        if (!chunk.isEmpty()) silence.restart();
        else if (!session->process.outputOpen()) { broken = true; break; }
        pending += chunk;
        if (pending.size() > 8 * 1024 * 1024) { broken = true; break; }
        while (pending.contains('\n') && !got) {
            const auto line = pending.left(pending.indexOf('\n'));
            pending.remove(0, line.size() + 1);
            const auto value = QJsonDocument::fromJson(line).object();
            if (qint64(value["id"].toDouble()) != id) continue;
            if (value["event"] == "progress") {
                auto fields = value;
                fields.remove("event");
                fields.remove("id");
                if (const auto report = UploadProgress::fromJson(fields); report && progress) progress(*report);
                continue;
            }
            if (value["event"] == "notice") {
                if (notice) notice(value["message"].toString().left(200));
                continue;
            }
            if (value["event"] == "checkpoint") {
                Error saved;
                if (checkpoint) {
                    auto promise = std::make_shared<std::promise<Error>>();
                    auto future = promise->get_future();
                    checkpoint(value["journal"].toObject(), [promise](Error e) { promise->set_value(e); });
                    saved = future.get();
                } else saved = {"checkpoint_failed", "This request keeps no journal", {}};
                QJsonObject ack{{"ack", !saved}, {"id", double(id)}};
                if (saved) ack["error"] = QJsonObject{{"code", saved.code}, {"message", saved.message}};
                session->process.write(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n', 5000);
                continue;
            }
            if (!value["ok"].isBool()) { broken = true; break; }
            final = value;
            got = true;
        }
        if (broken) break;
    }
    if (!got) {
        error = {"sftp_transport_unknown", "The SFTP helper ended without a confirmed result; check the server before retrying", {}};
        if (uncertain) *uncertain = mutation;
        return {};
    }
    // The worker keeps its connection for the next request to this destination.
    if (session->process.running() && !cancelSent) park(key, std::move(session));
    else retire(std::move(session));
    if (!final["ok"].toBool()) {
        error = parseError(final["error"].toObject());
        // The worker knows whether its failure came after it created the file (a refused write
        // whose partial file it couldn't remove): its word stands, whatever the code.
        if (uncertain) *uncertain = final["uncertain"].toBool();
    }
    return final["result"].toObject();
}
void SftpProvider::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation) {
    if (auto e = sftp::validate(destination.settings, true)) { reply({"inconclusive", {}, {}, e}); return; }
    Error e;
    const auto r = call("checkName", destination, {{"candidate", candidate}}, e, nullptr, std::move(cancellation));
    reply({e ? "inconclusive" : r["status"].toString("inconclusive"), r["objectReference"].toObject(), {}, e});
}
void SftpProvider::upload(const UploadRequest& request, std::function<void(UploadResult)> reply) {
    if (request.condition != "absent") {
        reply({"failed", {}, {"unsupported_capability", "SFTP uploads never replace a file on the server; choose rename or skip for name collisions", {}}});
        return;
    }
    if (auto e = sftp::validate(request.destination.settings, true)) { reply({"failed", {}, e}); return; }
    Error e;
    bool uncertain = false;
    const auto r = call("upload", request.destination,
                        {{"artifact", request.artifact.toJson()}, {"candidate", request.candidate}, {"publicationId", request.publicationId},
                         {"condition", request.condition}, {"objectReference", request.objectReference}},
                        e, &uncertain, request.cancellation, request.progress, request.checkpoint);
    const auto status = r["status"].toString();
    if (status == "uploaded" && !e) { reply({"uploaded", StorageReceipt::fromJson(r["receipt"].toObject()), {}}); return; }
    if (status == "collision") { reply({"collision", {}, {"sftp_name_taken", "A file with this name is already on the server", {}}}); return; }
    reply({uncertain ? "unknown" : "failed", {}, e ? e : Error{"sftp_failed", "The SFTP upload failed", {}}});
}
void SftpProvider::stat(const StorageReceipt& receipt, std::function<void(StatResult)> reply) { statForRemoval(receipt, {}, std::move(reply)); }
void SftpProvider::statForRemoval(const StorageReceipt& receipt, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)> reply) {
    Error e;
    const auto r = call("stat", receipt.destination, {{"receipt", receipt.toJson()}}, e, nullptr, {}, {}, {}, std::move(kill));
    reply({e ? "unknown" : r["status"].toString("unknown"), r.contains("receipt") ? StorageReceipt::fromJson(r["receipt"].toObject()) : receipt, e});
}
void SftpProvider::reconcile(const UploadRequest& request, std::function<void(StatResult)> reply) {
    Error e;
    const auto r = call("reconcile", request.destination,
                        {{"artifact", request.artifact.toJson()}, {"candidate", request.candidate}, {"publicationId", request.publicationId},
                         {"objectReference", request.objectReference}, {"resume", request.resume}}, e, nullptr, request.cancellation);
    reply({e ? "unknown" : r["status"].toString("unknown"), StorageReceipt::fromJson(r["receipt"].toObject()), e});
}
void SftpProvider::remove(const DeleteRequest& request, DurableCheckpoint checkpoint, std::function<void(DeleteResult)> reply) {
    Error e;
    bool uncertain = false;
    // The worker saves its intent through `checkpoint` and waits for it before REMOVE.
    QJsonObject saved = request.progress;
    DurableCheckpoint keep = [&saved, checkpoint](const QJsonObject& journal, std::function<void(Error)> ack) {
        if (!checkpoint) { ack({"checkpoint_failed", "This deletion keeps no journal", {}}); return; }
        checkpoint(journal, [&saved, journal, ack](Error error) { if (!error) saved = journal; ack(error); });
    };
    const auto r = call("remove", request.receipt.destination, {{"receipt", request.receipt.toJson()}}, e, &uncertain, request.cancellation, {}, keep, request.kill);
    auto status = r["status"].toString();
    if (status.isEmpty() || (e && status != "unknown")) status = uncertain ? "unknown" : "failed";
    // An unclear outcome keeps the saved intent; a settled one has nothing left to resume.
    reply({status, status == "unknown" ? saved : QJsonObject{}, e});
}
void SftpProvider::testDestination(const Destination& destination, DurableCheckpoint checkpoint, std::function<void(QJsonObject, Error)> reply,
                                   std::function<void(const QString&)> notice) {
    if (auto e = sftp::validate(destination.settings, true)) { reply({}, e); return; }
    Error e;
    const auto r = call("testDestination", destination, {}, e, nullptr, {}, {}, std::move(checkpoint), {}, std::move(notice));
    reply(r, e);
}
QJsonObject SftpProvider::setup(const QString& kind, const Destination& destination, const QJsonObject& input, Error& error) {
    if (kind == QLatin1String("inspectAccount")) return call("inspect", destination, {}, error);
    if (kind == QLatin1String("trustHostKey")) return call("trust", destination, {{"fingerprint", input["fingerprint"]}}, error);
    if (kind == QLatin1String("createFolder")) {
        if (auto e = sftp::validate(destination.settings, true)) { error = e; return {}; }
        // Only the folder the destination names: the request repeats it, so a stale form can't create another.
        if (input["name"].toString() != destination.settings["folder"].toString()) {
            error = {"sftp_settings_invalid", "Create folder makes the destination's own Folder on server; save that folder first", {{"field", "folder"}}};
            return {};
        }
        // The path the test reported missing: the worker makes it only if the folder resolves there still.
        return call("createFolder", destination, {{"path", input["path"]}}, error);
    }
    error = {"setup_unsupported", "This SFTP setup step doesn't exist", {}};
    return {};
}
}
