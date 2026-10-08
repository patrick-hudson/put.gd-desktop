#pragma once
#include "app/HistoryService.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <thread>
#include <chrono>
namespace putgd::test {
class Source final:public SourceAdapter {
public:
 quint64 revision=0;ClipboardPayload payload;int reads=0,copies=0;CopyPayload copied;bool defer=false;QList<std::function<void(PayloadResult)>> acquisitions;
 quint64 clipboardRevision() const override {return revision;}
 QJsonObject capabilities() const override {return {{"region",true},{"clipboardImport",QJsonArray{"files","image","text"}},{"clipboardResult",QJsonArray{"files","image","text"}}};}
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> cb) override {++reads;if(defer)acquisitions.append(cb);else cb({payload,{}});}
 void cancelAcquisition() override {if(!acquisitions.isEmpty()) {auto cb=acquisitions.takeFirst();cb({{}, {"source_cancelled","Selection cancelled",{}}});}}
 void copy(const CopyPayload& p,std::function<void(Error)> cb) override {++copies;copied=p;++revision;cb({});}
};
class OpaqueProvider final:public StorageProvider {
public:
 mutable QMutex mutex;QHash<QString,StorageReceipt> objects;std::atomic_int uploads{0},deletes{0},checks{0};bool loseReply=false,failUpload=false,allowDelete=true,slowDelete=false,wrongStatDestination=false;QObject* dispatcher=nullptr;
 bool deferUploads=false;QList<std::function<void()>> completions;QList<UploadRequest> requests;
 QHash<QString,QString> deletionResults;std::function<void(const DeleteRequest&)> checkpointObserver;
 static QString fileId(const QString& candidate) {return "opaque:α/%?#:"+QString::fromLatin1(QCryptographicHash::hash(candidate.toUtf8(),QCryptographicHash::Sha256).toHex());}
 ProviderCapabilities capabilities(const Destination&) const override {return {true,true,true,true,true,allowDelete,true,255,"Remove only the recorded opaque file revision"};}
 QString serializationKey(const Destination& d,const QJsonObject& ref,const QString& name) const override {return d.settings["workspace"].toString()+":"+(ref.isEmpty()?fileId(name):ref["fileId"].toString());}
 Error validateSettings(const QJsonObject& s) const override {if(s.size()!=1||!s["workspace"].isString())return {"config_invalid","A workspace is required",{}};return {};}
 void checkName(const Destination&,const QString& name,std::function<void(NameCheck)> cb) override {++checks;QMutexLocker lock(&mutex);auto id=fileId(name);auto found=objects.value(id);NameCheck r{objects.contains(id)?"present":"absent",{{"format",1},{"fileId",id}},found.concurrencyToken,{}};lock.unlock();cb(r);}
 void upload(const UploadRequest& request,std::function<void(UploadResult)> cb) override {
  ++uploads;
  auto complete=[this,request,cb] {
   if(failUpload) {cb({"failed",{}, {"fixture_failed","Synthetic failure",{}}});return;}
   QMutexLocker lock(&mutex);const auto id=request.objectReference["fileId"].toString();const auto existing=objects.value(id);
   if((request.condition=="absent"&&objects.contains(id))||(request.condition=="match"&&existing.concurrencyToken!=request.concurrencyToken)) {lock.unlock();cb({"collision",{}, {"precondition_changed","Synthetic collision",{}}});return;}
   StorageReceipt r;r.destination=request.destination;r.publicationId=request.publicationId;r.objectReference={{"format",1},{"fileId",id},{"revisionId","opaque-version!"+request.publicationId}};r.concurrencyToken="opaque-token!"+request.publicationId;r.directUrl="https://example.invalid/direct?file="+request.publicationId+"#unrelated";objects.insert(id,r);lock.unlock();cb({loseReply?"unknown":"uploaded",r,{}});
  };
  if(deferUploads) {QMetaObject::invokeMethod(dispatcher,[this,complete,request] {requests.append(request);completions.append(complete);},Qt::QueuedConnection);}else complete();
 }
 void stat(const StorageReceipt& receipt,std::function<void(StatResult)> cb) override {QMutexLocker lock(&mutex);auto id=receipt.objectReference["fileId"].toString();auto r=objects.value(id);if(wrongStatDestination)r.destination.settings["workspace"]="different-namespace";QString status=!objects.contains(id)?"missing":r.publicationId==receipt.publicationId?"own":"foreign";lock.unlock();cb({status,r,{}});}
 void reconcile(const UploadRequest& request,std::function<void(StatResult)> cb) override {QMutexLocker lock(&mutex);auto id=request.objectReference["fileId"].toString();auto r=objects.value(id);QString status=!objects.contains(id)?"missing":r.publicationId==request.publicationId?"own":"foreign";lock.unlock();cb({status,r,{}});}
 void remove(const DeleteRequest& request,DurableCheckpoint checkpoint,std::function<void(DeleteResult)> cb) override {
  const QJsonObject progress{{"schemaVersion",1},{"phase","recorded-intent"},{"opaqueMutationId",request.receipt.publicationId}};
  checkpoint(progress,[this,request,progress,cb](Error e) {if(e) {cb({"failed",{},e});return;}if(checkpointObserver)checkpointObserver(request);++deletes;QMutexLocker lock(&mutex);auto id=request.receipt.objectReference["fileId"].toString();auto status=deletionResults.value(id,"removed");if(status=="removed")objects.remove(id);lock.unlock();if(slowDelete)std::thread([cb,status,progress] {std::this_thread::sleep_for(std::chrono::milliseconds(250));cb({status,progress,{}});}).detach();else cb({status,progress,{}});});
 }
};
inline QJsonObject configuration(const QString& root,bool local=false) {
 auto doc=Settings::defaults();auto actions=doc["actions"].toObject();for(const auto& action:Settings::actionNames()) {auto a=actions[action].toObject();a["destination"]=local?"local":"upload";a["profileId"]=local?QJsonValue(QJsonValue::Null):QJsonValue("test");actions[action]=a;}doc["actions"]=actions;
 if(!local)doc["profiles"]=QJsonArray{QJsonObject{{"id","test"},{"label","Synthetic opaque provider"},{"provider","opaque"},{"settings",QJsonObject{{"workspace","fixture-workspace"}}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}}};
 auto archive=doc["archive"].toObject();archive["root"]=root+"/archive";doc["archive"]=archive;return doc;
}
inline ClipboardPayload textPayload() {ClipboardPayload p;p.kind="text";p.bytes="synthetic source content";return p;}
inline QString fixture(const QString& root,const QString& name,const QByteArray& bytes) {QFile f(root+"/"+name);if(!f.open(QIODevice::WriteOnly))return {};f.write(bytes);return f.fileName();}
}
