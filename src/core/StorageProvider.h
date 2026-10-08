#pragma once
#include "StorageReceipt.h"
#include "ArtifactStore.h"
#include <functional>
#include <memory>
#include <atomic>
#include <QHash>
namespace putgd {
struct ProviderCapabilities {
 bool conditionalCreate=false,conditionalReplace=false,conditionalDelete=false,versionAddressing=false,stat=false,remove=false;
 bool urls=false;
 int maximumNameBytes=255;
 QString deletionEffect;
 QJsonObject toJson() const;
};
struct NameCheck {
 QString status; // absent, present, inconclusive
 QJsonObject objectReference;
 QString concurrencyToken;
 Error error;
};
struct UploadRequest {
 Destination destination; Artifact artifact;
 QString candidate,publicationId,condition; // absent or match
 QJsonObject objectReference;
 QString concurrencyToken;
 std::shared_ptr<std::atomic_bool> cancellation;
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
class StorageProvider {
public:
 virtual ~StorageProvider()=default;
 virtual bool reconcilesDeleteProgress() const { return false; }
 virtual ProviderCapabilities capabilities(const Destination&) const=0;
 virtual QString serializationKey(const Destination&,const QJsonObject& objectReference,const QString& candidate) const=0;
 virtual Error validateSettings(const QJsonObject&) const=0;
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
 QJsonObject capabilities() const;
private: QHash<QString,std::shared_ptr<StorageProvider>> providers_;
};
}
