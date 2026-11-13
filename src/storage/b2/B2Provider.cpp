#include "B2Provider.h"
#include "storage/worker/ProviderExchange.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
namespace putgd {
namespace {
Error accessError() { return {"credential_setup_required", "Credentials are not loaded for this destination. Unlock saved credentials, or enter and Store the B2 application key for this session.", {}}; }
Error parseError(const QJsonObject& value) { return {value["code"].toString("b2_failed"), value["message"].toString("Backblaze B2 request failed"), value["details"].toObject()}; }
// Errors the worker reports before any request that could change B2.
bool definite(const QString& code) {
    static const QStringList codes{"b2_settings_invalid", "b2_protocol", "b2_authentication_failed", "b2_key_restricted", "b2_bucket_not_found",
                                   "b2_name_check_required", "b2_name_invalid", "unsupported_capability", "artifact_invalid", "artifact_changed",
                                   "b2_plan_invalid", "credential_setup_required", "b2_bucket_private"};
    return codes.contains(code);
}
}
B2Provider::B2Provider(CredentialStore& credentials, b2::UploadPolicy policy) : credentials_(credentials), policy_(policy) {}
B2Provider::~B2Provider() { stopping_ = true; }
QString B2Provider::workerPath() {
    const auto app = QDir::cleanPath(QCoreApplication::applicationDirPath());
#ifdef PUTGD_B2_BUILD_APP_DIR
    if (app == QString::fromUtf8(PUTGD_B2_BUILD_APP_DIR)) return QString::fromUtf8(PUTGD_B2_WORKER_PATH);
#endif
    return QDir(app).absoluteFilePath("putgd-b2-worker");
}
QJsonObject B2Provider::diagnostics() const { return {{"activeHelpers", active_.load()}, {"protocolVersion", 1}}; }
ProviderCapabilities B2Provider::capabilities(const Destination& destination) const {
    ProviderCapabilities caps;
    // No conditional create in B2: the worker checks the name's versions before the
    // upload and removes its own version when another appeared meanwhile (#282).
    caps.conditionalCreate = true;
    caps.versionAddressing = true;
    caps.stat = true;
    caps.remove = true;
    caps.urls = true;
    caps.maximumNameBytes = b2::maximumNameBytes - 1 - int(destination.settings["objectPrefix"].toString().toUtf8().size());
    caps.maximumObjectBytes = b2::maximumFileBytes;
    caps.deletionEffect = "Deletes exactly the uploaded B2 file version by its ID. put.gd never uploads over a visible file, so the name then shows "
                          "nothing, or an older version that was hidden before the upload. Authorized links stop working when they expire.";
    return caps;
}
QString B2Provider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    return destination.settings["apiUrl"].toString() + "/" + destination.settings["bucket"].toString() + "/" +
           (object["key"].isString() ? object["key"].toString() : b2::fileName(destination.settings, candidate));
}
Error B2Provider::validateSettings(const QJsonObject& settings) const { return b2::validateSettings(settings); }

QJsonObject B2Provider::call(const QString& method, const Destination& destination, QJsonObject params, Error& error, bool* uncertain,
                             std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress,
                             DurableCheckpoint checkpoint) {
    if (uncertain) *uncertain = false;
    if (stopping_.load()) { error = {"operation_cancelled", "The application is stopping", {}}; return {}; }
    if (auto e = b2::validateSettings(destination.settings)) { error = e; return {}; }
    const auto credential = credentials_.cached(destination.profileId, destination.credentialReference);
    if (!credential || credential->accessKey.isEmpty() || credential->secretKey.isEmpty()) { error = accessError(); return {}; }
    params["schemaVersion"] = 1;
    params["method"] = method;
    params["destination"] = destination.toJson();
    params["credentials"] = QJsonObject{{"keyId", credential->accessKey}, {"applicationKey", credential->secretKey}};
    ++active_;
    struct Release { std::atomic_int& count; ~Release() { --count; } } release{active_};
    const worker::ExchangePolicy policy{"b2", "B2", "B2", "b2_transport_unknown",
        "The Backblaze B2 helper could not start; reinstall put.gd",
        "The B2 helper stopped responding; reconcile its saved identity before retrying",
        "The B2 helper ended without a confirmed result; reconcile before retrying",
        {"upload", "remove", "testDestination"}, method != "checkName" && method != "stat", true};
    const auto final = worker::exchange(workerPath(), method, std::move(params), destination.settings["requestTimeoutMs"].toInt(600000),
                                        policy, stopping_, error, uncertain, cancellation, progress, checkpoint);
    if (error) return {};
    if (!final["ok"].toBool()) {
        error = parseError(final["error"].toObject());
        if (uncertain) *uncertain = final["uncertain"].toBool() && !definite(error.code);
        return final["result"].toObject();
    }
    if (uncertain) *uncertain = false;
    return final["result"].toObject();
}
void B2Provider::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation) {
    Error e;
    const auto r = call("checkName", destination, {{"candidate", candidate}}, e, nullptr, cancellation);
    reply({e ? "inconclusive" : r["status"].toString("inconclusive"), r["objectReference"].toObject(), r["concurrencyToken"].toString(), e});
}
void B2Provider::upload(const UploadRequest& request, std::function<void(UploadResult)> reply) {
    if (request.condition != "absent") {
        reply({"failed", {}, {"unsupported_capability", "Backblaze B2 cannot replace a recorded file version safely; choose rename or skip for name collisions", {}}});
        return;
    }
    // A new large upload starts with a part plan; an existing journal keeps its own.
    auto resume = request.resume;
    if (resume.isEmpty())
        if (const auto part = b2::partBytes(request.artifact.bytes, policy_)) resume = {{"schemaVersion", 1}, {"partBytes", double(part)}};
    Error e;
    bool uncertain = false;
    const auto r = call("upload", request.destination,
                        {{"artifact", request.artifact.toJson()}, {"candidate", request.candidate}, {"publicationId", request.publicationId},
                         {"condition", request.condition}, {"objectReference", request.objectReference}, {"resume", resume}},
                        e, &uncertain, request.cancellation, request.progress, request.checkpoint);
    const auto status = r["status"].toString();
    if (status == "uploaded" && !e) { reply({"uploaded", StorageReceipt::fromJson(r["receipt"].toObject()), {}}); return; }
    if (status == "collision") { reply({"collision", {}, e}); return; }
    reply({uncertain || status == "unknown" ? "unknown" : "failed", {}, e ? e : Error{"b2_failed", "Backblaze B2 upload failed", {}}});
}
void B2Provider::stat(const StorageReceipt& receipt, std::function<void(StatResult)> reply) {
    Error e;
    const auto r = call("stat", receipt.destination, {{"receipt", receipt.toJson()}}, e);
    reply({e ? "unknown" : r["status"].toString("unknown"), StorageReceipt::fromJson(r["receipt"].toObject()), e});
}
void B2Provider::reconcile(const UploadRequest& request, std::function<void(StatResult)> reply) {
    Error e;
    const auto r = call("reconcile", request.destination,
                        {{"artifact", request.artifact.toJson()}, {"candidate", request.candidate}, {"publicationId", request.publicationId},
                         {"objectReference", request.objectReference}}, e);
    reply({e ? "unknown" : r["status"].toString("unknown"), StorageReceipt::fromJson(r["receipt"].toObject()), e});
}
void B2Provider::remove(const DeleteRequest& request, DurableCheckpoint checkpoint, std::function<void(DeleteResult)> reply) {
    Error e;
    bool uncertain = false;
    const auto r = call("remove", request.receipt.destination, {{"receipt", request.receipt.toJson()}, {"progress", request.progress}}, e, &uncertain,
                        request.cancellation, {}, checkpoint);
    auto status = r["status"].toString();
    if (status.isEmpty()) status = uncertain ? "unknown" : "failed";
    reply({status, r["progress"].toObject(), e});
}
void B2Provider::abandonUpload(const Destination& destination, const QJsonObject& journal, std::function<void(Error)> reply) {
    if (!journal["uploadId"].isString()) { reply({}); return; }
    Error e;
    call("abandon", destination, {{"journal", journal}}, e);
    reply(e);
}
void B2Provider::testDestination(const Destination& destination, DurableCheckpoint checkpoint, std::function<void(QJsonObject, Error)> reply) {
    Error e;
    const auto r = call("testDestination", destination, {}, e, nullptr, {}, {}, checkpoint);
    reply(r, e);
}
}

namespace putgd {
void B2Provider::refreshLink(const StorageReceipt& receipt, std::function<void(StatResult)> reply) {
    Error e;const auto result=call("refreshLink",receipt.destination,{{"receipt",receipt.toJson()}},e);
    reply({e?"unknown":result["status"].toString("unknown"),StorageReceipt::fromJson(result["receipt"].toObject()),e});
}
}

namespace putgd {
void B2Provider::unlockLinkCredentials(const StorageReceipt& receipt,std::function<void(Error)> reply) {
    credentials_.unlock(receipt.destination.profileId,receipt.destination.credentialReference,std::move(reply));
}
}
