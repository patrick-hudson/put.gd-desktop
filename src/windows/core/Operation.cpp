#include "core/Operation.h"
#include <algorithm>
#include <cmath>

namespace putgd {
using json::Object; using json::Value; using json::Array;
namespace {
constexpr double maxProgressBytes = 9007199254740991.0;
bool in(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }
std::optional<long long> progressNumber(const Value& v) {
    const auto d = v.toDouble(-1);
    if (!v.isNumber() || !std::isfinite(d) || d < 0 || d > maxProgressBytes || std::floor(d) != d) return std::nullopt;
    return static_cast<long long>(d);
}
}
Object Artifact::toJson() const {
    return {{"id", id}, {"path", path}, {"name", name}, {"mime", mime}, {"category", category}, {"bytes", double(bytes)}, {"sha256", sha256},
        {"originalFile", originalFile}, {"previewPath", previewPath}, {"namingPrepared", namingPrepared}};
}
Artifact Artifact::fromJson(const Object& o) {
    return {o.value("id").toString(), o.value("path").toString(), o.value("name").toString(), o.value("mime").toString(), o.value("category").toString(),
        o.value("sha256").toString(), static_cast<long long>(o.value("bytes").toDouble()), o.value("originalFile").toBool(), o.value("previewPath").toString(), o.value("namingPrepared").toBool()};
}
Object LocalCopy::toJson() const { return {{"status", status}, {"path", path}, {"error", error.toJson()}}; }
LocalCopy LocalCopy::fromJson(const Object& o) { return {o.value("status").toString("disabled"), o.value("path").toString(), Error::fromJson(o.value("error").toObject())}; }
Object Destination::toJson() const { return {{"providerId", providerId}, {"profileId", profileId}, {"profileRevision", profileRevision}, {"settings", settings}, {"credentials", credentialReference}}; }
Destination Destination::fromJson(const Object& o) { return {o.value("providerId").toString(), o.value("profileId").toString(), o.value("profileRevision").toString(), o.value("settings").toObject(), o.value("credentials").toObject()}; }
Object StorageReceipt::toJson() const { return {{"schemaVersion", schemaVersion}, {"destination", destination.toJson()}, {"objectReference", objectReference}, {"concurrencyToken", concurrencyToken}, {"publicationId", publicationId}, {"directUrl", directUrl}}; }
StorageReceipt StorageReceipt::fromJson(const Object& o) { return {o.value("schemaVersion").toInt(), Destination::fromJson(o.value("destination").toObject()), o.value("objectReference").toObject(), o.value("concurrencyToken").toString(), o.value("publicationId").toString(), o.value("directUrl").toString()}; }
Error StorageReceipt::validate() const {
    if (schemaVersion != 1 || destination.providerId.empty() || destination.profileId.empty() || destination.profileRevision.empty() || objectReference.empty() || publicationId.empty())
        return {"receipt_invalid", "Provider returned an incomplete receipt", {}};
    return {};
}
bool UploadProgress::valid() const {
    return in({"preparing", "uploading", "finalizing"}, phase) && (!bytesTransferred || (*bytesTransferred >= 0 && double(*bytesTransferred) <= maxProgressBytes)) &&
        (!bytesTotal || (bytesTransferred && *bytesTotal >= 0 && double(*bytesTotal) <= maxProgressBytes && *bytesTransferred <= *bytesTotal));
}
Object UploadProgress::toJson() const {
    Object o{{"phase", phase}};
    if (bytesTransferred) o["bytesTransferred"] = double(*bytesTransferred);
    if (bytesTotal) o["bytesTotal"] = double(*bytesTotal);
    return o;
}
std::optional<UploadProgress> UploadProgress::fromJson(const Object& o) {
    if (o.empty()) return std::nullopt;
    UploadProgress p; p.phase = o.value("phase").toString();
    if (o.contains("bytesTransferred")) { p.bytesTransferred = progressNumber(o.value("bytesTransferred")); if (!p.bytesTransferred) return std::nullopt; }
    if (o.contains("bytesTotal")) { p.bytesTotal = progressNumber(o.value("bytesTotal")); if (!p.bytesTotal) return std::nullopt; }
    if (!p.valid()) return std::nullopt;
    return p;
}
bool OperationItem::linkAvailable() const {
    return uploadStatus == "uploaded" && !receipt.directUrl.empty() && !in({"pending", "unknown", "revoked", "private", "failed", "changed"}, shareStatus) &&
        !in({"removed", "already_missing", "changed", "hidden_version_retained"}, remoteStatus);
}
Object OperationItem::toJson() const {
    return {{"artifact", artifact.toJson()}, {"uploadStatus", uploadStatus}, {"candidate", candidate}, {"condition", condition}, {"concurrencyToken", concurrencyToken},
        {"publicationId", publicationId}, {"objectReference", objectReference}, {"receipt", receipt.toJson()}, {"local", local.toJson()}, {"remoteStatus", remoteStatus},
        {"shareStatus", shareStatus}, {"shareIntent", shareIntent}, {"shareIntentOptions", shareIntentOptions}, {"error", error.toJson()}, {"phase", phase},
        {"uploadProgress", uploadProgress ? Value(uploadProgress->toJson()) : Value()}};
}
OperationItem OperationItem::fromJson(const Object& o) {
    OperationItem i;
    i.artifact = Artifact::fromJson(o.value("artifact").toObject()); i.uploadStatus = o.value("uploadStatus").toString(); i.candidate = o.value("candidate").toString();
    i.condition = o.value("condition").toString(); i.concurrencyToken = o.value("concurrencyToken").toString(); i.publicationId = o.value("publicationId").toString();
    i.objectReference = o.value("objectReference").toObject(); i.receipt = StorageReceipt::fromJson(o.value("receipt").toObject()); i.local = LocalCopy::fromJson(o.value("local").toObject());
    i.remoteStatus = o.value("remoteStatus").toString(); i.shareStatus = o.value("shareStatus").toString("not_requested"); i.shareIntent = o.value("shareIntent").toString();
    i.shareIntentOptions = o.value("shareIntentOptions").toObject(); i.error = Error::fromJson(o.value("error").toObject()); i.phase = o.value("phase").toString(i.uploadStatus);
    i.uploadProgress = UploadProgress::fromJson(o.value("uploadProgress").toObject());
    return i;
}
bool Operation::activeState(const std::string& s) { return in({"accepted", "acquiring", "editing", "recording", "stored", "running"}, s); }
bool Operation::terminalUpload(const std::string& s) { return in({"uploaded", "skipped", "conflict", "local_only", "cancelled"}, s); }
Object Operation::toJson() const {
    Array array; for (const auto& i : items) array.push_back(i.toJson());
    return {{"id", id}, {"action", action}, {"state", state}, {"phase", phase}, {"settingsRevision", settingsRevision}, {"settings", settingsDocument},
        {"destination", destination.toJson()}, {"requestTimeoutMs", requestTimeoutMs ? Value(*requestTimeoutMs) : Value()}, {"created", created}, {"items", array},
        {"invocation", std::to_string(invocation)}, {"clipboardRevision", std::to_string(clipboardRevision)}, {"copyStatus", copyStatus}, {"cancelled", cancelled}, {"error", error.toJson()}};
}
Object Operation::publicJson() const {
    auto result = toJson();
    result.remove("settings");
    auto array = result.value("items").toArray();
    for (auto& value : array) { auto item = value.toObject(); auto artifact = item.value("artifact").toObject(); artifact.remove("path"); item["artifact"] = artifact; value = item; }
    result["items"] = array;
    return result;
}
Operation Operation::fromJson(const Object& o) {
    Operation op;
    op.id = o.value("id").toString(); op.action = o.value("action").toString(); op.state = o.value("state").toString(); op.phase = o.value("phase").toString(op.state);
    op.settingsRevision = o.value("settingsRevision").toString(); op.settingsDocument = o.value("settings").toObject(); op.destination = Destination::fromJson(o.value("destination").toObject());
    const auto timeout = o.value("requestTimeoutMs").toInt(); if (timeout >= 1000) op.requestTimeoutMs = timeout;
    op.created = o.value("created").toString();
    for (const auto& i : o.value("items").toArray()) op.items.push_back(OperationItem::fromJson(i.toObject()));
    op.invocation = std::strtoull(o.value("invocation").toString("0").c_str(), nullptr, 10); op.clipboardRevision = std::strtoull(o.value("clipboardRevision").toString("0").c_str(), nullptr, 10);
    op.copyStatus = o.value("copyStatus").toString("pending"); op.cancelled = o.value("cancelled").toBool(); op.error = Error::fromJson(o.value("error").toObject());
    return op;
}
}
