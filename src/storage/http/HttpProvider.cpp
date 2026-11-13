#include "HttpProvider.h"
#include <functional>
#include "HttpTemplate.h"
#include "storage/worker/ProviderExchange.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
namespace putgd {
namespace {
Error parseError(const QJsonObject& value) { return {value["code"].toString("http_failed"), value["message"].toString("The custom uploader request failed"), value["details"].toObject()}; }
}
HttpUploaderProvider::HttpUploaderProvider(CredentialStore& credentials) : credentials_(credentials) {}
HttpUploaderProvider::~HttpUploaderProvider() { stopping_ = true; }
QString HttpUploaderProvider::workerPath() {
    const auto app = QDir::cleanPath(QCoreApplication::applicationDirPath());
#ifdef PUTGD_HTTP_BUILD_APP_DIR
    if (app == QString::fromUtf8(PUTGD_HTTP_BUILD_APP_DIR)) return QString::fromUtf8(PUTGD_HTTP_WORKER_PATH);
#endif
    return QDir(app).absoluteFilePath("putgd-http-worker");
}
QJsonObject HttpUploaderProvider::diagnostics() const { return {{"activeHelpers", active_.load()}, {"protocolVersion", 1}}; }
ProviderCapabilities HttpUploaderProvider::capabilities(const Destination& destination) const {
    ProviderCapabilities caps;
    // The server chooses the name and address of what it stores, so nothing here can
    // create conditionally, look an upload up afterwards or address a version.
    caps.serverNamesObjects = true;
    caps.referenceDelete = true;
    caps.remove = !destination.settings["deletionUrl"].toString().isEmpty();
    caps.urls = true;
    caps.maximumNameBytes = 255;
    caps.deletionEffect = "Asks the server to delete this upload through the deletion URL it returned with the link. "
                          "What the server removes, and how soon the link stops working, is up to the server.";
    return caps;
}
ProviderCapabilities HttpUploaderProvider::receiptCapabilities(const StorageReceipt& receipt) const {
    auto caps = capabilities(receipt.destination);
    // Deletion depends on what this upload's reply carried, not on today's settings.
    caps.remove = !receipt.objectReference["deletionUrl"].toString().isEmpty();
    if (!caps.remove) {
        // One came back and was not kept: say that, and why, not that none came back.
        const auto leftOut = receipt.objectReference["deletionUrlLeftOut"].toString();
        caps.removeUnavailable = leftOut == "other_host"
            ? "The server returned a deletion URL on another host than this destination's request URL. It would take a stored value of this destination there, so put.gd did not keep it and cannot delete this upload from the server. Delete it on the server itself."
            : leftOut == "stored_value"
            ? "The server returned a deletion URL that repeats a credential of this destination where put.gd cannot keep it by name, so it was not kept and put.gd cannot delete this upload from the server. Delete it on the server itself."
            : leftOut == "unusable"
            ? "The server returned a deletion URL that this destination does not use: plain HTTP it does not allow, an address on this computer, or one with a user name in it. put.gd cannot delete this upload from the server. Delete it on the server itself."
            : "This upload came back without a deletion URL, so put.gd cannot delete it from the server. Delete it on the server itself.";
    }
    return caps;
}
// A diagnostics package shows which fields a destination sets, never a value: a row
// marked non-secret can still hold something its owner would not publish.
QJsonObject HttpUploaderProvider::diagnosticSettings(const QJsonObject& settings) const {
    auto out = settings;
    for (const auto& list : http::rowLists()) {
        QJsonArray rows;
        for (const auto& value : settings[list].toArray()) rows.append(QJsonObject{{"name", value.toObject()["name"]}, {"secret", value.toObject()["secret"]}});
        out[list] = rows;
    }
    if (!settings["data"].toString().isEmpty()) out["data"] = "<set>";
    // An address or a reply template can hold a key someone typed into it by hand. The
    // package keeps where the request goes and which placeholders each value uses.
    const auto shape = [](const QString& text) {
        QStringList names;
        const std::function<void(const http::Template&)> collect = [&](const http::Template& nodes) {
            for (const auto& node : nodes) {
                if (!node.call) continue;
                // Only the placeholder vocabulary: anything else is someone's own text.
                static const QStringList vocabulary{"json", "xml", "regex", "header", "response", "responseurl", "filename", "input", "random", "base64", "secret"};
                const auto name = vocabulary.contains(node.text) ? node.text : QStringLiteral("other");
                if (!names.contains(name)) names.append(name);
                for (const auto& parameter : node.parameters) collect(parameter);
            }
        };
        collect(http::parseTemplate(text));
        return names.isEmpty() ? QStringLiteral("<set>") : "<set: " + names.join(", ") + ">";
    };
    if (const auto address = settings["requestUrl"].toString(); !address.isEmpty()) {
        const auto authority = address.indexOf("://");
        const auto path = authority < 0 ? -1 : address.indexOf('/', authority + 3);
        const QUrl url(path < 0 ? address : address.left(path), QUrl::StrictMode);
        out["requestUrl"] = url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() ? url.scheme() + "://" + url.authority() + (path < 0 || path + 1 >= address.size() ? QString() : "/" + shape(address.mid(path)))
                                                                                                : shape(address);
    }
    for (const auto* key : {"responseUrl", "thumbnailUrl", "deletionUrl", "errorMessage"})
        if (const auto text = settings[key].toString(); !text.isEmpty()) out[key] = shape(text);
    return out;
}
QString HttpUploaderProvider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    // Each upload is its own object: the name check hands out a nonce the receipt keeps.
    return destination.settings["requestUrl"].toString() + "#" + (object["nonce"].isString() ? object["nonce"].toString() : candidate);
}
Error HttpUploaderProvider::validateSettings(const QJsonObject& settings) const { return http::validateSettings(settings); }
Error HttpUploaderProvider::readiness(const Destination& destination) const {
    if (auto e = http::validateSettings(destination.settings)) return e;
    if (destination.settings["requestUrl"].toString().isEmpty())
        return {"http_setup_required", "This destination has no request URL yet. Enter it in Settings, or import a .sxcu file.", {}};
    const auto keys = http::secretKeys(destination.settings);
    if (keys.isEmpty()) return {};
    const auto cached = credentials_.cached(destination.profileId, destination.credentialReference);
    if (!cached)
        return {"credential_setup_required", "Credentials are not loaded for this destination. Unlock saved credentials, or enter its secret values in Settings and store them.", {}};
    QStringList missing;
    for (const auto& key : keys) if (!cached->values.contains(key)) missing.append(http::secretLabel(key));
    if (!missing.isEmpty())
        return {"credential_setup_required", "This destination has no stored value for: " + missing.join(", ") + ". Enter it in Settings and store the credentials.", {}};
    return {};
}

QJsonObject HttpUploaderProvider::call(const QString& method, const Destination& destination, QJsonObject params, Error& error, bool* uncertain,
                                       std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress,
                                       DurableCheckpoint checkpoint) {
    if (uncertain) *uncertain = false;
    if (stopping_.load()) { error = {"operation_cancelled", "The application is stopping", {}}; return {}; }
    if (auto e = http::validateSettings(destination.settings)) { error = e; return {}; }
    QJsonObject secrets;
    if (method == "remove") {
        // Deletion sends nothing but the deletion URL the upload returned. Where that
        // URL names a stored value, such as a token, only that value is handed over.
        const auto keys = http::secretReferences(params["receipt"].toObject()["objectReference"].toObject()["deletionUrl"].toString());
        if (!keys.isEmpty()) {
            const auto cached = credentials_.cached(destination.profileId, destination.credentialReference);
            for (const auto& key : keys) {
                if (!cached || !cached->values.contains(key)) {
                    error = {"credential_setup_required", "Deleting this upload needs its destination's stored credentials. Unlock them, or enter and store them in Settings, then try again.", {}};
                    return {};
                }
                secrets[key] = cached->values.value(key);
            }
        }
    } else {
        if (auto e = readiness(destination)) { error = e; return {}; }
        const auto keys = http::secretKeys(destination.settings);
        if (!keys.isEmpty()) {
            const auto cached = credentials_.cached(destination.profileId, destination.credentialReference);
            for (const auto& key : keys) secrets[key] = cached ? cached->values.value(key) : QString();
        }
    }
    params["schemaVersion"] = 1;
    params["method"] = method;
    params["destination"] = destination.toJson();
    params["secrets"] = secrets;
    secrets = {};
    ++active_;
    struct Release { std::atomic_int& count; ~Release() { --count; } } release{active_};
    const worker::ExchangePolicy policy{"http", "custom uploader", "Custom uploader", "http_outcome_unknown",
        "The custom uploader helper could not start; reinstall put.gd",
        "The custom uploader helper stopped responding; the upload may or may not be stored on the server",
        "The custom uploader helper ended without a result; the upload may or may not be stored on the server",
        {"remove", "testDestination"}};
    const auto final = worker::exchange(workerPath(), method, std::move(params), destination.settings["requestTimeoutMs"].toInt(600000),
                                        policy, stopping_, error, uncertain, cancellation, progress, checkpoint);
    if (error) return {};
    if (uncertain) *uncertain = final["uncertain"].toBool();
    if (!final["ok"].toBool()) error = parseError(final["error"].toObject());
    return final["result"].toObject();
}
void HttpUploaderProvider::checkName(const Destination& destination, const QString&, std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool>) {
    // No request: the server picks the name, so a proposed name is always free. The
    // nonce makes every upload its own object, two files of one name included.
    if (auto e = readiness(destination)) { reply({"inconclusive", {}, {}, e}); return; }
    reply({"absent", {{"schemaVersion", 1}, {"nonce", QUuid::createUuid().toString(QUuid::WithoutBraces)}}, {}, {}});
}
void HttpUploaderProvider::upload(const UploadRequest& request, std::function<void(UploadResult)> reply) {
    Error e;
    bool uncertain = false;
    const auto r = call("upload", request.destination,
                        {{"artifact", request.artifact.toJson()}, {"candidate", request.candidate}, {"publicationId", request.publicationId},
                         {"objectReference", request.objectReference}},
                        e, &uncertain, request.cancellation, request.progress);
    const auto status = r["status"].toString();
    if (status == "uploaded" && !e) { reply({"uploaded", StorageReceipt::fromJson(r["receipt"].toObject()), {}}); return; }
    reply({uncertain || status == "unknown" ? "unknown" : "failed", {}, e ? e : Error{"http_failed", "The custom uploader request failed", {}}});
}
void HttpUploaderProvider::stat(const StorageReceipt&, std::function<void(StatResult)> reply) {
    reply({"unknown", {}, {"unsupported_capability", "A custom uploader cannot look up a stored upload", {}}});
}
void HttpUploaderProvider::reconcile(const UploadRequest&, std::function<void(StatResult)> reply) {
    reply({"unknown", {}, {"unsupported_capability", "A custom uploader cannot check whether an upload was stored", {}}});
}
void HttpUploaderProvider::remove(const DeleteRequest& request, DurableCheckpoint checkpoint, std::function<void(DeleteResult)> reply) {
    Error e;
    bool uncertain = false;
    const auto r = call("remove", request.receipt.destination, {{"receipt", request.receipt.toJson()}, {"progress", request.progress}}, e, &uncertain,
                        request.cancellation, {}, checkpoint);
    auto status = r["status"].toString();
    if (status.isEmpty()) status = uncertain ? "unknown" : "failed";
    reply({status, r["progress"].toObject(), e});
}
void HttpUploaderProvider::testDestination(const Destination& destination, DurableCheckpoint checkpoint, std::function<void(QJsonObject, Error)> reply) {
    Error e;
    const auto r = call("testDestination", destination, {}, e, nullptr, {}, {}, checkpoint);
    reply(r, e);
}
}
