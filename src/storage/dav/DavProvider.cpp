#include "DavProvider.h"
#include "storage/worker/ProviderExchange.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QPointer>
#include <future>
namespace putgd {
namespace {
Error parseError(const QJsonObject& value) {
    return {value["code"].toString("webdav_failed"), value["message"].toString("The WebDAV request failed"), value["details"].toObject()};
}
// Errors the worker reports before any request that could change the server.
bool definite(const QString& code) {
    static const QStringList codes{"webdav_settings_invalid", "webdav_protocol", "webdav_auth_failed", "webdav_folder_missing", "webdav_plain_http_refused",
                                   "webdav_tls_failed", "webdav_name_invalid", "webdav_quota", "webdav_rate_limited", "webdav_too_large", "webdav_forbidden",
                                   "nextcloud_sharing_disabled", "nextcloud_share_password_required", "nextcloud_not_found", "unsupported_capability",
                                   "credential_setup_required", "artifact_invalid", "artifact_changed"};
    return codes.contains(code);
}
QString label(const QString& id) { return id == dav::nextcloudId ? QStringLiteral("Nextcloud") : QStringLiteral("WebDAV"); }
}
DavProvider::DavProvider(CredentialStore& credentials, QString providerId, qint64 chunkThresholdBytes, qint64 chunkBytes)
    : credentials_(credentials), id_(std::move(providerId)), chunkThreshold_(chunkThresholdBytes), chunkBytes_(chunkBytes) {}
DavProvider::~DavProvider() { stopping_ = true; }
QString DavProvider::workerPath() {
    const auto app = QDir::cleanPath(QCoreApplication::applicationDirPath());
#ifdef PUTGD_DAV_BUILD_APP_DIR
    if (app == QString::fromUtf8(PUTGD_DAV_BUILD_APP_DIR)) return QString::fromUtf8(PUTGD_DAV_WORKER_PATH);
#endif
    return QDir(app).absoluteFilePath("putgd-webdav-worker");
}
QJsonObject DavProvider::diagnostics() const { return {{"activeHelpers", active_.load()}, {"protocolVersion", 1}}; }
ProviderCapabilities DavProvider::capabilities(const Destination&) const {
    ProviderCapabilities caps;
    caps.conditionalCreate = true;   // PUT with If-None-Match: *
    caps.conditionalDelete = true;   // DELETE with If-Match: <recorded ETag>
    caps.stat = true;
    caps.remove = true;
    caps.urls = true;
    caps.sharing = id_ == dav::nextcloudId;
    caps.maximumNameBytes = dav::maximumNameBytes;
    caps.deletionEffect = id_ == dav::nextcloudId
        ? QStringLiteral("Deletes the uploaded file only while it is unchanged since the upload; its share link stops working. Nextcloud keeps it in the trash bin until the trash is emptied.")
        : QStringLiteral("Deletes the uploaded file only while the server reports it unchanged since the upload (If-Match on its ETag). Links to it stop working.");
    return caps;
}
ProviderCapabilities DavProvider::receiptCapabilities(const StorageReceipt& receipt) const {
    auto caps = capabilities(receipt.destination);
    if (receipt.concurrencyToken.isEmpty()) {
        caps.remove = false;
        caps.removeUnavailable = QStringLiteral("The server reported no ETag for this upload, so put.gd can't delete it without risking another file.");
    }
    return caps;
}
QJsonObject DavProvider::diagnosticSettings(const QJsonObject& settings) const {
    auto out = settings;
    // The account's display name is a person's name; the package says only whether one is set.
    if (out.contains("accountName")) out["accountName"] = out["accountName"].toString().isEmpty() ? QString() : QStringLiteral("<set>");
    return out;
}
QString DavProvider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    const auto name = object["name"].isString() ? object["name"].toString() : candidate;
    const auto& s = destination.settings;
    return id_ == dav::nextcloudId ? s["server"].toString() + "/" + s["folder"].toString() + "/" + name : s["address"].toString() + name;
}
Error DavProvider::validateSettings(const QJsonObject& settings) const { return dav::validateSettings(id_, settings); }

QJsonObject DavProvider::call(const QString& method, const Destination& destination, QJsonObject params, Error& error, bool* uncertain,
                              std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress,
                              DurableCheckpoint checkpoint, std::shared_ptr<std::atomic_bool> kill, std::function<void(const QJsonObject&)> notice, bool needsCredentials) {
    if (uncertain) *uncertain = false;
    if (stopping_.load()) { error = {"operation_cancelled", "The application is stopping", {}}; return {}; }
    if (destination.providerId != id_) { error = {"webdav_protocol", "The destination is not a " + label(id_) + " destination", {}}; return {}; }
    if (method != "signIn")
        if (auto e = dav::validateSettings(id_, destination.settings)) { error = e; return {}; }
    const auto credential = credentials_.cached(destination.profileId, destination.credentialReference);
    if (needsCredentials && (!credential || credential->accessKey.isEmpty() || credential->secretKey.isEmpty())) {
        error = {"credential_setup_required", id_ == dav::nextcloudId
            ? QStringLiteral("put.gd isn't signed in to this Nextcloud destination. Sign in, or unlock saved credentials.")
            : QStringLiteral("Credentials are not loaded for this destination. Unlock saved credentials, or enter the WebDAV user and password and choose Save to keychain or Use for this session."), {}};
        return {};
    }
    params["schemaVersion"] = 1;
    params["method"] = method;
    params["destination"] = destination.toJson();
    if (credential && needsCredentials) params["credentials"] = QJsonObject{{"user", credential->accessKey}, {"password", credential->secretKey}};
    else if (credential && method == "signOut") params["credentials"] = QJsonObject{{"user", credential->accessKey}, {"password", credential->secretKey}};
    if (method == "upload") { params["chunkThresholdBytes"] = double(chunkThreshold_); params["chunkBytes"] = double(chunkBytes_); }
    ++active_;
    struct Release { std::atomic_int& count; ~Release() { --count; } } release{active_};
    const worker::ExchangePolicy policy{"webdav", label(id_), label(id_), "webdav_transport_unknown",
        "The " + label(id_) + " helper could not start; reinstall put.gd",
        "The " + label(id_) + " helper stopped responding; reconcile before retrying",
        "The " + label(id_) + " helper ended without a confirmed result; reconcile before retrying",
        {"upload", "remove", "share", "testDestination"},
        !QStringList{"checkName", "stat", "reconcile", "inspectAccount", "signIn"}.contains(method), true};
    // Sign-in waits for the browser (up to 20 minutes) without output; assembling a large
    // chunked upload can keep the server quiet for minutes.
    const qint64 perRequest = method == "signIn" ? 7 * 60 * 1000 : 5 * 60 * 1000;
    const auto final = worker::exchange(workerPath(), method, std::move(params), perRequest, policy, stopping_, error, uncertain, cancellation, progress,
                                        checkpoint, kill, {}, std::move(notice));
    if (error) return {};
    if (!final["ok"].toBool()) {
        error = parseError(final["error"].toObject());
        if (uncertain) *uncertain = final["uncertain"].toBool() && !definite(error.code);
        return final["result"].toObject();
    }
    if (uncertain) *uncertain = false;
    return final["result"].toObject();
}
void DavProvider::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation) {
    Error e;
    const auto r = call("checkName", destination, {{"candidate", candidate}}, e, nullptr, cancellation);
    reply({e ? "inconclusive" : r["status"].toString("inconclusive"), r["objectReference"].toObject(), r["concurrencyToken"].toString(), e});
}
void DavProvider::upload(const UploadRequest& request, std::function<void(UploadResult)> reply) {
    if (request.condition != "absent") {
        reply({"failed", {}, {"unsupported_capability", label(id_) + " uploads never replace an existing file; choose rename or skip for name collisions", {}}});
        return;
    }
    Error e;
    bool uncertain = false;
    const auto r = call("upload", request.destination,
                        {{"artifact", request.artifact.toJson()}, {"candidate", request.candidate}, {"publicationId", request.publicationId},
                         {"condition", request.condition}, {"objectReference", request.objectReference}, {"resume", request.resume}},
                        e, &uncertain, request.cancellation, request.progress, request.checkpoint);
    const auto status = r["status"].toString();
    if (status == "uploaded" && !e) { reply({"uploaded", StorageReceipt::fromJson(r["receipt"].toObject()), {}}); return; }
    if (status == "collision") { reply({"collision", {}, e}); return; }
    reply({uncertain || status == "unknown" ? "unknown" : "failed", {}, e ? e : Error{"webdav_failed", "The " + label(id_) + " upload failed", {}}});
}
void DavProvider::stat(const StorageReceipt& receipt, std::function<void(StatResult)> reply) {
    Error e;
    const auto r = call("stat", receipt.destination, {{"receipt", receipt.toJson()}}, e);
    reply({e ? "unknown" : r["status"].toString("unknown"), StorageReceipt::fromJson(r["receipt"].toObject()), e});
}
void DavProvider::statForRemoval(const StorageReceipt& receipt, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)> reply) {
    Error e;
    const auto r = call("stat", receipt.destination, {{"receipt", receipt.toJson()}}, e, nullptr, {}, {}, {}, kill);
    reply({e ? "unknown" : r["status"].toString("unknown"), StorageReceipt::fromJson(r["receipt"].toObject()), e});
}
void DavProvider::reconcile(const UploadRequest& request, std::function<void(StatResult)> reply) {
    Error e;
    const auto r = call("reconcile", request.destination,
                        {{"artifact", request.artifact.toJson()}, {"candidate", request.candidate}, {"publicationId", request.publicationId},
                         {"objectReference", request.objectReference}}, e, nullptr, request.cancellation);
    reply({e ? "unknown" : r["status"].toString("unknown"), StorageReceipt::fromJson(r["receipt"].toObject()), e});
}
void DavProvider::remove(const DeleteRequest& request, DurableCheckpoint checkpoint, std::function<void(DeleteResult)> reply) {
    Error e;
    bool uncertain = false;
    const auto r = call("remove", request.receipt.destination, {{"receipt", request.receipt.toJson()}, {"progress", request.progress}}, e, &uncertain,
                        request.cancellation, {}, checkpoint, request.kill);
    auto status = r["status"].toString();
    if (status.isEmpty()) status = uncertain ? "unknown" : "failed";
    reply({status, r["progress"].toObject(), e});
}
void DavProvider::share(const ShareRequest& request, DurableCheckpoint checkpoint, std::function<void(ShareResult)> reply) {
    if (id_ != dav::nextcloudId) { StorageProvider::share(request, checkpoint, std::move(reply)); return; }
    auto options = request.options;
    options["action"] = request.action;
    Error e;
    bool uncertain = false;
    const auto r = call("share", request.receipt.destination, {{"receipt", request.receipt.toJson()}, {"options", options}}, e, &uncertain,
                        request.cancellation, {}, checkpoint);
    auto receipt = r.contains("receipt") ? StorageReceipt::fromJson(r["receipt"].toObject()) : request.receipt;
    const auto status = r["status"].toString(e ? (uncertain ? "unknown" : "failed") : "unknown");
    if (status != "shared") receipt.directUrl.clear();
    ShareResult result{status, receipt, r["progress"].toObject(), e};
    const auto expires = QDateTime::fromString(r.value("expiresAt").toString(), Qt::ISODateWithMs);
    if (status == "shared" && expires.isValid()) result.expiresAt = expires.toUTC().toString(Qt::ISODateWithMs);
    reply(result);
}
void DavProvider::abandonUpload(const Destination& destination, const QJsonObject& journal, std::function<void(Error)> reply) {
    if (!journal["uploadId"].isString()) { reply({}); return; }
    Error e;
    call("abandon", destination, {{"journal", journal}}, e);
    reply(e);
}
void DavProvider::testDestination(const Destination& destination, DurableCheckpoint checkpoint, std::function<void(QJsonObject, Error)> reply) {
    Error e;
    const auto r = call("testDestination", destination, {}, e, nullptr, {}, {}, checkpoint);
    reply(r, e);
}
void DavProvider::setup(const QString& kind, const Destination& destination, const QJsonObject& input, const Background& background,
                        std::function<void(Error, QJsonObject)> reply, std::function<void(QJsonObject)> event, std::shared_ptr<std::atomic_bool> cancelled) {
    const bool nextcloud = id_ == dav::nextcloudId;
    if (kind == "connectAccount") {
        if (!nextcloud) { reply({"setup_unsupported", "A WebDAV destination has no browser sign-in; store its user and password instead", {}}, {}); return; }
        auto store = &credentials_;
        background([this, destination, event, cancelled](Error& error) {
            return call("signIn", destination, {}, error, nullptr, cancelled, {}, {}, {}, [event](const QJsonObject& value) {
                const QUrl url(value["url"].toString());
                if (event && value.size() == 2 && QStringList{"http", "https"}.contains(url.scheme())) event({{"event", "signIn"}, {"url", url.toString(QUrl::FullyEncoded)}});
            }, false);
        }, [store, destination, reply](Error error, QJsonObject result) {
            auto password = result.take("appPassword").toString();
            const auto login = result["loginName"].toString();
            if (error) { password.fill(QChar('\0')); reply(error, {}); return; }
            // Only the store keeps the app password; the job's result names the account.
            store->save(destination.profileId, destination.credentialReference, Credentials{login, password, {}}, [reply, result](Error e) { reply(e, e ? QJsonObject{} : result); });
            password.fill(QChar('\0'));
        });
        return;
    }
    if (kind == "signOut") {
        auto store = &credentials_;
        const bool signedIn = credentials_.cached(destination.profileId, destination.credentialReference).has_value();
        background([this, destination, nextcloud, signedIn](Error& error) -> QJsonObject {
            if (!nextcloud || !signedIn) return {{"revoked", false}};
            Error revoke;
            auto out = call("signOut", destination, {}, revoke, nullptr, {}, {}, {}, {}, {}, false);
            // A server that can't be reached keeps the app password; the user is told to revoke it there.
            if (revoke) out = {{"revoked", false}, {"revokeError", revoke.toJson()}};
            error = {};
            return out;
        }, [store, destination, reply](Error, QJsonObject result) {
            store->erase(destination.profileId, destination.credentialReference, [reply, result](Error e) { reply(e, e ? QJsonObject{} : result); });
        });
        return;
    }
    if (kind == "inspectAccount" || kind == "createFolder") {
        const auto method = kind;
        background([this, destination, method](Error& error) { return call(method, destination, {}, error); }, std::move(reply));
        return;
    }
    Q_UNUSED(input);
    reply({"setup_unsupported", "This destination has no such setup step", {}}, {});
}
}
