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
    // Nextcloud's share page serves an uploaded .html file as a download, never as a page.
    caps.encryptUnavailable = id_ == dav::nextcloudId ? QStringLiteral("Nextcloud share links download HTML files instead of opening them, so an encrypted page can't open there.") : QString();
    caps.encryptsplit = id_ != dav::nextcloudId;
    caps.maximumNameBytes = dav::maximumNameBytes;
    caps.deletionEffect = id_ == dav::nextcloudId
        ? QStringLiteral("Deletes the uploaded file only if it is unchanged since upload. Its share link stops working, and Nextcloud moves the file to the trash bin until emptied.")
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
    // Addresses keep only their scheme and host, as the custom uploader's request URL does.
    for (const auto* key : {"server", "address", "linkBase"})
        if (out[key].isString()) out[key] = dav::originOnly(out[key].toString());
    // The account's display name is a person's name; the package says only whether one is set.
    if (out.contains("accountName")) out["accountName"] = out["accountName"].toString().isEmpty() ? QString() : QStringLiteral("<set>");
    return out;
}
// The canonical DAV address of the file: scheme, host, port and path, with the bound account's
// ID for Nextcloud, so two spellings of one server serialize together.
QString DavProvider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    if (object["target"].isString() && !object["target"].toString().isEmpty()) return object["target"].toString();
    const auto name = object["name"].isString() ? object["name"].toString() : candidate;
    QString userId;
    if (const auto credential = credentials_.cached(destination.profileId, destination.credentialReference))
        userId = dav::parseBinding(credential->sessionToken)["userId"].toString();
    return dav::canonicalUrl(dav::fileUrl(destination.settings, id_, userId, name));
}
void DavProvider::unlockLinkCredentials(const StorageReceipt& receipt, std::function<void(Error)> reply) {
    credentials_.unlock(receipt.destination.profileId, receipt.destination.credentialReference, std::move(reply));
}
Error DavProvider::validateSettings(const QJsonObject& settings) const { return dav::validateSettings(id_, settings); }

QJsonObject DavProvider::call(const QString& method, const Destination& destination, QJsonObject params, Error& error, bool* uncertain,
                              std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress,
                              DurableCheckpoint checkpoint, std::shared_ptr<std::atomic_bool> kill, std::function<void(const QJsonObject&)> notice, bool needsCredentials,
                              const Credentials* given) {
    if (uncertain) *uncertain = false;
    if (stopping_.load()) { error = {"operation_cancelled", "put.gd is closing, so the request didn't start.", {}}; return {}; }
    if (destination.providerId != id_) { error = {"webdav_protocol", "The destination is not a " + label(id_) + " destination", {}}; return {}; }
    if (method != "signIn")
        if (auto e = dav::validateSettings(id_, destination.settings)) { error = e; return {}; }
    const auto credential = given ? std::optional<Credentials>(*given) : credentials_.cached(destination.profileId, destination.credentialReference);
    if (needsCredentials && (!credential || credential->accessKey.isEmpty() || credential->secretKey.isEmpty())) {
        error = {"credential_setup_required", id_ == dav::nextcloudId
            ? QStringLiteral("Not signed in to this Nextcloud destination. Unlock saved credentials, or select Sign in… in Destinations.")
            : QStringLiteral("Credentials for this destination are locked or missing. Unlock saved credentials, or enter the WebDAV user and password and select Save to keychain or Use for this session."), {}};
        return {};
    }
    // Credentials go only to the place they were given for (#1872): the binding stored beside them
    // must name this destination's server or folder; otherwise nothing is sent.
    const bool sendsCredentials = credential && (needsCredentials || method == "signOut");
    if (sendsCredentials) {
        const auto binding = dav::parseBinding(credential->sessionToken);
        if (binding.isEmpty() || binding["base"].toString() != dav::bindingBase(destination.settings, id_)) {
            error = {"credential_setup_required", id_ == dav::nextcloudId
                ? QStringLiteral("The saved app password belongs to a different Nextcloud address. Select Sign in again… to reconnect.")
                : QStringLiteral("These credentials were saved for a different WebDAV address. Enter them again and select Save to keychain or Use for this session."), {{"reason", "binding"}}};
            return {};
        }
        // A Nextcloud app password stored by hand is checked (and bound to its account) before
        // any other operation uses it.
        if (id_ == dav::nextcloudId && binding["userId"].toString().isEmpty() && !QStringList{"inspectAccount", "signOut"}.contains(method)) {
            error = {"credential_setup_required", QStringLiteral("put.gd hasn't verified which Nextcloud account this app password belongs to yet. Select Test upload and link to check it, or select Sign in… to replace it."), {{"reason", "unbound"}}};
            return {};
        }
        params["binding"] = binding;
    }
    params["schemaVersion"] = 1;
    params["method"] = method;
    params["destination"] = destination.toJson();
    if (sendsCredentials) params["credentials"] = QJsonObject{{"user", credential->accessKey}, {"password", credential->secretKey}};
    if (method == "upload") { params["chunkThresholdBytes"] = double(chunkThreshold_); params["chunkBytes"] = double(chunkBytes_); }
    ++active_;
    struct Release { std::atomic_int& count; ~Release() { --count; } } release{active_};
    const bool mutation = !QStringList{"checkName", "stat", "reconcile", "inspectAccount", "signIn"}.contains(method);
    const auto ending = worker::unconfirmedEnding(method, mutation, "the server", ", so the file may already be on the server. Select Retry upload to check before sending again.");
    const worker::ExchangePolicy policy{"webdav", label(id_), label(id_), "webdav_transport_unknown",
        "Couldn't start the " + label(id_) + " helper. Reinstall put.gd.",
        "The " + label(id_) + " helper stopped responding" + ending,
        "The " + label(id_) + " helper exited without confirming the result" + ending,
        {"upload", "remove", "share", "testDestination"}, mutation, true};
    // Sign-in waits for the browser (up to 20 minutes) without output; assembling a large chunked
    // upload sends a heartbeat while the server works.
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
        reply({"failed", {}, {"unsupported_capability", label(id_) + " uploads can't replace an existing file. Change When a name exists in Settings → Capture to Add a unique suffix or Skip duplicate names.", {}}});
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
    // The last journal the step saved is its progress: the share ID it recorded stays on record
    // whatever the outcome, so a later inspection or revoke finds that share.
    auto last = std::make_shared<QJsonObject>(request.progress);
    const DurableCheckpoint kept = [checkpoint, last](const QJsonObject& journal, std::function<void(Error)> ack) {
        checkpoint(journal, [last, journal, ack](Error e) { if (!e) *last = journal; ack(e); });
    };
    const auto r = call("share", request.receipt.destination, {{"receipt", request.receipt.toJson()}, {"options", options}, {"progress", request.progress}}, e, &uncertain,
                        request.cancellation, {}, kept);
    auto receipt = r.contains("receipt") ? StorageReceipt::fromJson(r["receipt"].toObject()) : request.receipt;
    const auto status = r["status"].toString(e ? (uncertain ? "unknown" : "failed") : "unknown");
    if (status != "shared") receipt.directUrl.clear();
    auto progress = r["progress"].toObject();
    if (progress.isEmpty()) progress = *last;
    // A recorded share ID outlives the journal of a step that didn't record one itself, unless this
    // step confirmed that share gone.
    // Only the old ID the lookup found gone is dropped; one this step checkpointed (a replacement) stays.
    const auto gone = r["forgetShareId"].toString();
    if (!gone.isEmpty() && progress["shareId"].toString() == gone) progress.remove("shareId");
    else if (gone.isEmpty() && !progress.contains("shareId") && request.progress.contains("shareId")) progress["shareId"] = request.progress["shareId"];
    progress.remove("tokenDigest");
    ShareResult result{status, receipt, progress, e};
    const auto expires = QDateTime::fromString(r.value("expiresAt").toString(), Qt::ISODateWithMs);
    if (status == "shared" && expires.isValid()) result.expiresAt = expires.toUTC().toString(Qt::ISODateWithMs);
    reply(result);
}
void DavProvider::abandonUpload(const Destination& destination, const QJsonObject& journal, std::function<void(Error)> reply) {
    if (!journal["uploadId"].isString() && !journal["earlier"].isArray()) { reply({}); return; }
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
                        std::function<void(Error, QJsonObject)> reply, std::function<void(QJsonObject)> event, std::shared_ptr<std::atomic_bool> cancelled,
                        std::function<bool(const Destination&)> current) {
    Q_UNUSED(input);
    const bool nextcloud = id_ == dav::nextcloudId;
    auto* store = &credentials_;
    // Revokes an app password put.gd won't keep, with those credentials and nothing else, in a
    // helper of its own (never the sign-in's, which may be stopping). `done` hears whether the
    // server confirmed it.
    const auto revoke = [this, background, destination](Credentials grant, std::function<void(bool)> done) {
        background([this, destination, grant](Error& error) {
            Credentials given = grant;
            return call("signOut", destination, {}, error, nullptr, {}, {}, {}, {}, {}, false, &given);
        }, [done](Error error, QJsonObject result) { done(!error && result["revoked"].toBool()); });
    };
    const QString removeThere = QStringLiteral(" Remove it in Nextcloud under Personal settings → Security → Devices & sessions.");
    // A server that enforces link passwords: signed in, but no link can be made.
    const auto passwordEnforced = [](const QJsonObject& account, bool saved) {
        Error e{"nextcloud_share_password_required",
                QString("Signed in as %1, but this Nextcloud server requires a password on every public link. put.gd doesn't set link passwords, so uploads won't get a link. Ask the administrator to turn off mandatory link passwords.")
                    .arg(account["accountName"].toString(account["userId"].toString())),
                {{"credentialSaved", saved}, {"accountName", account["accountName"]}, {"userId", account["userId"]}}};
        if (account.contains("revokeWarning")) { e.details["revokeWarning"] = account["revokeWarning"]; e.message += " " + account["revokeWarning"].toString(); }
        return e;
    };
    if (kind == "connectAccount") {
        if (!nextcloud) { reply({"setup_unsupported", "A WebDAV destination has no browser sign-in; store its user and password instead", {}}, {}); return; }
        // Signed in before, with the app password still locked in the keychain: it is read first,
        // so it can be revoked once the new one is saved.
        auto unlockFailed = std::make_shared<bool>(false);
        const auto start = [this, store, destination, background, reply, event, cancelled, current, revoke, passwordEnforced, removeThere, unlockFailed]() {
            background([this, destination, event, cancelled](Error& error) {
                return call("signIn", destination, {}, error, nullptr, cancelled, {}, {}, {}, [event](const QJsonObject& value) {
                    const QUrl url(value["url"].toString());
                    if (event && value.size() == 2 && QStringList{"http", "https"}.contains(url.scheme())) event({{"event", "signIn"}, {"url", url.toString(QUrl::FullyEncoded)}});
                }, false);
            }, [store, destination, reply, cancelled, current, revoke, passwordEnforced, removeThere, unlockFailed](Error error, QJsonObject result) {
                auto password = result.take("appPassword").toString();
                const Credentials grant{result.take("loginName").toString(), password, result.take("binding").toString()};
                password.fill(QChar('\0'));
                // A grant that isn't kept is revoked; whether the server confirmed it is said.
                const auto refuse = [reply, revoke, grant, removeThere](Error e) {
                    revoke(grant, [reply, e, removeThere](bool revoked) mutable {
                        e.details.remove("grantReturned");
                        e.details["grantRevoked"] = revoked;
                        if (!revoked) e.message += " put.gd couldn't revoke the app password Nextcloud had just given it." + removeThere;
                        reply(e, {});
                    });
                };
                if (error) {
                    if (error.details.value("grantReturned").toBool() && !grant.secretKey.isEmpty()) { refuse(error); return; }
                    // The helper stopped before its last word (killed, stalled, or its answer lost): a grant
                    // it may already have had can't be revoked from here, and the user is told so.
                    if (error.code == "webdav_transport_unknown" || error.code == "webdav_protocol" || error.code == "webdav_worker_start") {
                        error.details["grantRevocation"] = "unknown";
                        error.message += " If you had already allowed put.gd in the browser, remove its app password in Nextcloud under Personal settings → Security → Devices & sessions.";
                    }
                    reply(error, {});
                    return;
                }
                // Cancelled meanwhile, or the destination is gone or names another server.
                if ((cancelled && cancelled->load()) || (current && !current(destination))) {
                    refuse(cancelled && cancelled->load() ? Error{"nextcloud_signin_cancelled", "Sign-in cancelled.", {}}
                                                          : Error{"config_conflict", "The destination changed while you were signing in. Select Sign in… to try again.", {}});
                    return;
                }
                const auto previous = store->cached(destination.profileId, destination.credentialReference);
                store->save(destination.profileId, destination.credentialReference, grant,
                            [destination, reply, result, previous, grant, revoke, refuse, passwordEnforced, removeThere, unlockFailed](Error saved) mutable {
                    // Not kept here, so not kept on the server either.
                    if (saved) { refuse(saved); return; }
                    const auto finish = [reply, passwordEnforced](QJsonObject account) {
                        if (account["sharePasswordEnforced"].toBool()) reply(passwordEnforced(account, true), {});
                        else reply({}, account);
                    };
                    // The earlier grant goes only now that the new one is saved, and only when it was
                    // given for this same place.
                    if (previous && previous->secretKey != grant.secretKey &&
                        dav::parseBinding(previous->sessionToken)["base"] == dav::parseBinding(grant.sessionToken)["base"]) {
                        revoke(*previous, [result, finish, removeThere](bool revoked) mutable {
                            result["previousRevoked"] = revoked;
                            if (!revoked) result["revokeWarning"] = "put.gd couldn't revoke the app password it had before." + removeThere;
                            finish(result);
                        });
                        return;
                    }
                    if (*unlockFailed) result["revokeWarning"] = "put.gd couldn't read its earlier app password from the keychain, so it couldn't revoke it." + removeThere;
                    finish(result);
                });
            });
        };
        if (!store->cached(destination.profileId, destination.credentialReference) && destination.credentialReference["mode"] == "keychain") {
            // An entry that isn't there is no earlier grant; one that can't be read is said.
            store->unlock(destination.profileId, destination.credentialReference, [start, unlockFailed](Error e) { *unlockFailed = e && !e.details.value("missing").toBool(); start(); });
            return;
        }
        start();
        return;
    }
    if (kind == "signOut") {
        // A keychain entry not read yet is read first, so its app password can be revoked.
        const auto proceed = [this, store, destination, nextcloud, background, reply]() {
            const auto loaded = store->cached(destination.profileId, destination.credentialReference);
            background([this, destination, nextcloud, loaded](Error& error) -> QJsonObject {
                if (!nextcloud || !loaded) return {{"revoked", false}};
                Error revokeError;
                auto out = call("signOut", destination, {}, revokeError, nullptr, {}, {}, {}, {}, {}, false);
                // A server that can't be reached keeps the app password; the user is told to revoke it there.
                if (revokeError) out = {{"revoked", false}, {"revokeError", revokeError.toJson()}};
                error = {};
                return out;
            }, [store, destination, reply](Error, QJsonObject result) {
                store->erase(destination.profileId, destination.credentialReference, [reply, result](Error e) { reply(e, e ? QJsonObject{} : result); });
            });
        };
        if (nextcloud && !store->cached(destination.profileId, destination.credentialReference) && destination.credentialReference["mode"] == "keychain") {
            store->unlock(destination.profileId, destination.credentialReference, [proceed](Error) { proceed(); });
            return;
        }
        proceed();
        return;
    }
    if (kind == "inspectAccount") {
        // The inspection uses one credential, and only that one is bound to the account it found:
        // a replacement stored meanwhile is never bound to the old origin or account.
        const auto used = store->cached(destination.profileId, destination.credentialReference);
        if (!used) { reply({"credential_setup_required", nextcloud ? "Not signed in to this Nextcloud destination. Unlock saved credentials, or select Sign in… in Destinations." : "Credentials for this destination are locked or missing. Unlock saved credentials, or enter them and select Save to keychain or Use for this session.", {}}, {}); return; }
        background([this, destination, used](Error& error) {
            Credentials given = *used;
            return call("inspectAccount", destination, {}, error, nullptr, {}, {}, {}, {}, {}, true, &given);
        }, [store, destination, nextcloud, reply, passwordEnforced, used, current](Error error, QJsonObject result) {
            if (error || !nextcloud) { reply(error, result); return; }
            const auto finish = [reply, result, passwordEnforced] { if (result["sharePasswordEnforced"].toBool()) reply(passwordEnforced(result, true), {}); else reply({}, result); };
            if (!dav::parseBinding(used->sessionToken)["userId"].toString().isEmpty()) { finish(); return; }
            const auto now = store->cached(destination.profileId, destination.credentialReference);
            if (!now || now->generation != used->generation || (current && !current(destination))) {
                reply({"credential_changed", "Credentials or destination settings changed during the check. Try again.", {}}, {});
                return;
            }
            auto bound = *used;
            bound.sessionToken = dav::binding(destination.settings, dav::nextcloudId, result["userId"].toString());
            store->save(destination.profileId, destination.credentialReference, bound, [reply, finish](Error saved) { if (saved) reply(saved, {}); else finish(); });
        });
        return;
    }
    if (kind == "createFolder") {
        background([this, destination](Error& error) { return call("createFolder", destination, {}, error); }, std::move(reply));
        return;
    }
    reply({"setup_unsupported", "This destination has no such setup step", {}}, {});
}
void DavProvider::bindThen(const Destination& destination, const Background& background, std::function<bool(const Destination&)> current, std::function<void(Error)> then) {
    // A Nextcloud app password stored by hand is bound to its account before anything else uses it.
    const auto credential = credentials_.cached(destination.profileId, destination.credentialReference);
    if (id_ != dav::nextcloudId || !credential || !dav::parseBinding(credential->sessionToken)["userId"].toString().isEmpty()) { then({}); return; }
    setup("inspectAccount", destination, {}, background, [then](Error e, QJsonObject) {
        // The share-password policy is the Test's own finding; binding still happened.
        then(e.code == "nextcloud_share_password_required" ? Error{} : e);
    }, {}, {}, std::move(current));
}
}
