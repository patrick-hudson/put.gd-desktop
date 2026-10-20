#pragma once
#include "StorageReceipt.h"
#include "ArtifactStore.h"
#include <functional>
#include <memory>
#include <atomic>
#include <QHash>
#include <optional>
namespace putgd {
struct ProviderCapabilities {
 bool conditionalCreate=false,conditionalReplace=false,conditionalDelete=false,versionAddressing=false,stat=false,remove=false;
 bool urls=false;
 int maximumNameBytes=255;
 QString deletionEffect;
 bool sharing=false,privateUploads=false;
 // Largest object the provider accepts in one upload; zero when it reports no
 // fixed limit (a server may still refuse a file for quota or plan reasons).
 qint64 maximumObjectBytes=0;
 QJsonObject toJson() const;
};
struct NameCheck {
 QString status; // absent, present, inconclusive
 QJsonObject objectReference;
 QString concurrencyToken;
 Error error;
};
// Informational provider byte counters, never publication/ownership evidence.
// Total must use the same accounting as transferred bytes; unknown totals stay
// absent. Retries may reduce the current counter. Reports stop before completion.
struct UploadProgress {
 QString phase="uploading"; // preparing, uploading, finalizing
 std::optional<qint64> bytesTransferred,bytesTotal;
 bool valid() const;
 QJsonObject toJson() const;
 static std::optional<UploadProgress> fromJson(const QJsonObject&);
};
struct UploadRequest {
 Destination destination; Artifact artifact;
 QString candidate,publicationId,condition; // absent or match
 QJsonObject objectReference;
 QString concurrencyToken;
 std::shared_ptr<std::atomic_bool> cancellation;
 std::function<void(const UploadProgress&)> progress={};
 std::optional<int> requestTimeoutMs={}; // Attempt budget; accepted destination identity stays unchanged.
 // Opaque adapter-owned journal of a resumable upload (for example an S3
 // multipart upload ID), last acknowledged through `checkpoint`. Empty starts fresh.
 QJsonObject resume={};
 // A resumable upload awaits this durable acknowledgement before it relies on
 // remote state it created; an empty journal means none remains to resume or abort.
 std::function<void(const QJsonObject&,std::function<void(Error)>)> checkpoint={};
};
struct UploadResult { QString status; StorageReceipt receipt; Error error; }; // uploaded, collision, failed, unknown
struct StatResult { QString status; StorageReceipt receipt; Error error; }; // own, foreign, missing, unknown
struct DeleteRequest {
 QString id; StorageReceipt receipt;
 QJsonObject progress;
 std::shared_ptr<std::atomic_bool> cancellation;
};
struct DeleteResult { QString status; QJsonObject progress; Error error; }; // removed, missing, changed, failed, unknown, hidden_version_retained
// A driver must await a successful durable checkpoint callback BEFORE the associated
// destructive network step. Progress is versioned opaque adapter-owned data.
using DurableCheckpoint=std::function<void(const QJsonObject&,std::function<void(Error)>)>;
struct ShareRequest {
 QString id,action; // create, revoke, inspect
 StorageReceipt receipt;
 QJsonObject options,progress; // non-secret options and opaque driver progress
 std::shared_ptr<std::atomic_bool> cancellation;
};
struct ShareResult { QString status; StorageReceipt receipt; QJsonObject progress; Error error; };
bool shareResultMatchesReceipt(const ShareResult&,const StorageReceipt&);
class StorageProvider {
public:
 virtual ~StorageProvider()=default;
 // True when uploads reliably report preparing/uploading/finalizing phases.
 // Such uploads become active only upon uploading/finalizing; telemetry remains optional.
 virtual bool reportsUploadProgress() const { return false; }
 virtual bool reconcilesDeleteProgress() const { return false; }
 // Providers may adopt longer execution budgets on explicit retry, without changing destination identity.
 virtual std::optional<int> retryRequestTimeout(const Destination&,const QJsonObject&) const { return {}; }
 virtual QString defaultPublicationMode(const Destination&) const { return "share"; }
 virtual void share(const ShareRequest& request,DurableCheckpoint,std::function<void(ShareResult)> reply) {
  reply({"unsupported",request.receipt,{}, {"sharing_unsupported","This provider cannot create or revoke share links",{}}});
 }
 // Bounded non-secret resource counters only; no settings, payloads or credentials.
 virtual QJsonObject diagnostics() const { return {}; }
 virtual ProviderCapabilities capabilities(const Destination&) const=0;
 virtual QString serializationKey(const Destination&,const QJsonObject& objectReference,const QString& candidate) const=0;
 virtual Error validateSettings(const QJsonObject&) const=0;
 virtual Error validateCredentials(const QJsonObject&) const { return {}; }
 virtual void checkName(const Destination&,const QString&,std::function<void(NameCheck)>)=0;
 virtual void upload(const UploadRequest&,std::function<void(UploadResult)>)=0;
 virtual void stat(const StorageReceipt&,std::function<void(StatResult)>)=0;
 virtual void reconcile(const UploadRequest&,std::function<void(StatResult)>)=0;
 virtual void remove(const DeleteRequest&,DurableCheckpoint,std::function<void(DeleteResult)>)=0;
};
class ProviderRegistry {
public:
 void add(const QString&,std::shared_ptr<StorageProvider>);
 std::shared_ptr<StorageProvider> find(const QString&) const;
 Error validate(const QString&,const QJsonObject&) const;
 Error validateCredentials(const QString&,const QJsonObject&) const;
 QJsonObject capabilities() const;
 QJsonObject diagnostics() const;
private: QHash<QString,std::shared_ptr<StorageProvider>> providers_;
};
}
