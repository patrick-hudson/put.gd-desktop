#include "R2Provider.h"
#include "R2Settings.h"
namespace putgd {
namespace {
// A result about `original` names the Cloudflare R2 destination it was asked about.
std::function<void(StatResult)> asR2(const StorageReceipt& original, std::function<void(StatResult)> reply) {
    return [destination = original.destination, reply = std::move(reply)](StatResult result) {
        result.receipt.destination = destination;
        reply(std::move(result));
    };
}
// The R2 settings as saved or recorded, checked before any request is built from them: a malformed
// account, jurisdiction or bucket refuses, and never turns into another host or bucket (#1699).
Error refused(const Destination& destination) { return r2::validate(destination.settings); }
UploadRequest s3Request(const UploadRequest& request) {
    auto converted = request;
    converted.destination = R2Provider::s3Destination(request.destination);
    return converted;
}
}
R2Provider::R2Provider(std::shared_ptr<S3Provider> s3) : s3_(std::move(s3)) {}
Destination R2Provider::s3Destination(const Destination& destination) {
    return {QStringLiteral("s3"), destination.profileId, destination.profileRevision, r2::s3Settings(destination.settings), destination.credentialReference};
}
StorageReceipt R2Provider::s3Receipt(const StorageReceipt& receipt) {
    auto converted = receipt;
    converted.destination = s3Destination(receipt.destination);
    return converted;
}
int R2Provider::maximumConcurrentRequests(const Destination& destination) const { return s3_->maximumConcurrentRequests(s3Destination(destination)); }
ProviderCapabilities R2Provider::capabilities(const Destination& destination) const { return s3_->capabilities(s3Destination(destination)); }
ProviderCapabilities R2Provider::receiptCapabilities(const StorageReceipt& receipt) const { return s3_->receiptCapabilities(s3Receipt(receipt)); }
QString R2Provider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    return s3_->serializationKey(s3Destination(destination), object, candidate);
}
Error R2Provider::validateSettings(const QJsonObject& settings) const {
    if (auto error = r2::validate(settings)) return error;
    return s3_->validateSettings(r2::s3Settings(settings));
}
Error R2Provider::validateCredentials(const QJsonObject& reference) const { return s3_->validateCredentials(reference); }
void R2Provider::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation) {
    if (auto error = refused(destination)) { reply({"inconclusive", {}, {}, error}); return; }
    s3_->checkName(s3Destination(destination), candidate, std::move(reply), std::move(cancellation));
}
void R2Provider::upload(const UploadRequest& request, std::function<void(UploadResult)> reply) {
    if (auto error = refused(request.destination)) { reply({"failed", {}, error}); return; }
    s3_->upload(s3Request(request), [destination = request.destination, reply = std::move(reply)](UploadResult result) {
        result.receipt.destination = destination;
        reply(std::move(result));
    });
}
void R2Provider::unlockLinkCredentials(const StorageReceipt& receipt, std::function<void(Error)> reply) {
    if (auto error = refused(receipt.destination)) { reply(error); return; }
    s3_->unlockLinkCredentials(s3Receipt(receipt), std::move(reply)); }
void R2Provider::refreshLink(const StorageReceipt& receipt, std::function<void(StatResult)> reply) {
    if (auto error = refused(receipt.destination)) { reply({"unknown", receipt, error}); return; }
    s3_->refreshLink(s3Receipt(receipt), asR2(receipt, std::move(reply))); }
void R2Provider::refreshLinkForCopy(const StorageReceipt& receipt, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)> reply) {
    if (auto error = refused(receipt.destination)) { reply({"unknown", receipt, error}); return; }
    s3_->refreshLinkForCopy(s3Receipt(receipt), std::move(kill), asR2(receipt, std::move(reply)));
}
void R2Provider::stat(const StorageReceipt& receipt, std::function<void(StatResult)> reply) {
    if (auto error = refused(receipt.destination)) { reply({"unknown", receipt, error}); return; }
    s3_->stat(s3Receipt(receipt), asR2(receipt, std::move(reply))); }
void R2Provider::statForRemoval(const StorageReceipt& receipt, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)> reply) {
    if (auto error = refused(receipt.destination)) { reply({"unknown", receipt, error}); return; }
    s3_->statForRemoval(s3Receipt(receipt), std::move(kill), asR2(receipt, std::move(reply)));
}
void R2Provider::reconcile(const UploadRequest& request, std::function<void(StatResult)> reply) {
    if (auto error = refused(request.destination)) { reply({"unknown", {}, error}); return; }
    s3_->reconcile(s3Request(request), [destination = request.destination, reply = std::move(reply)](StatResult result) {
        result.receipt.destination = destination;
        reply(std::move(result));
    });
}
void R2Provider::remove(const DeleteRequest& request, DurableCheckpoint save, std::function<void(DeleteResult)> reply) {
    if (auto error = refused(request.receipt.destination)) { reply({"failed", request.progress, error}); return; }
    auto converted = request;
    converted.receipt = s3Receipt(request.receipt);
    s3_->remove(converted, std::move(save), std::move(reply));
}
void R2Provider::abandonUpload(const Destination& destination, const QJsonObject& journal, std::function<void(Error)> reply) {
    if (auto error = refused(destination)) { reply(error); return; }
    s3_->abandonUpload(s3Destination(destination), journal, std::move(reply));
}
void R2Provider::testDestination(const Destination& destination, DurableCheckpoint save, std::function<void(QJsonObject, Error)> reply) {
    if (auto error = refused(destination)) { reply({}, error); return; }
    if (auto error = validateSettings(destination.settings)) { reply({}, error); return; }
    s3_->testDestination(s3Destination(destination), std::move(save), std::move(reply));
}
}
