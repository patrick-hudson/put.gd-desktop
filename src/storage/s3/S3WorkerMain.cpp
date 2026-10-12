#include "S3Module.h"
#include "S3WorkerProtocol.h"
#include "app/UiControl.h"
#include "ui/StdioDevice.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QMetaObject>
#include <QTimer>
#include <QUuid>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
namespace putgd {
namespace {
Error uncertain(){return {"s3_transport_unknown","S3 work ended unexpectedly; reconcile its recorded target before retrying",{}};}
struct Exchange {std::mutex mutex;std::condition_variable changed;bool complete=false;QJsonObject reply;};
class Worker final {
public:
 Worker(QCoreApplication& app,QIODevice& pipes):app_(app),control_(pipes){
  build_=QJsonDocument::fromJson(buildManifestJson()).object();
  QObject::connect(&pipes,&QIODevice::readChannelFinished,&app_,[this]{fail();});
  QObject::connect(&control_,&UiControl::failed,&app_,[this](Error){fail();});
  QObject::connect(&control_,&UiControl::frame,&app_,[this](const QString& type,const QString&,const QJsonObject& object){receive(type,object);});
  heartbeat_.setInterval(1000);QObject::connect(&heartbeat_,&QTimer::timeout,&app_,[this]{
   if(!active_||terminal_||failed_||pendingKind_=="checkpoint")return;
   auto object=envelope();object["kind"]="alive";object["data"]=QJsonObject{};if(control_.send("observed",object).isEmpty())fail();
  });
  deadline_.setSingleShot(true);deadline_.start(10000);QObject::connect(&deadline_,&QTimer::timeout,&app_,[this]{fail();});
 }
 ~Worker(){abortExchange();module_.reset();}
private:
 QJsonObject envelope()const{return {{"role","s3-worker"},{"nonce",nonce_},{"generation",generation_}};}
 bool valid(const QJsonObject& object)const{return object["role"]=="s3-worker"&&object["nonce"]==nonce_&&object["generation"]==generation_;}
 QJsonObject exchange(const QString& kind,const QJsonObject& data){
  auto pending=std::make_shared<Exchange>();quint64 rpc=0;
  {std::lock_guard lock(exchangeMutex_);if(aborted_)return {};pending_=pending;rpc=++rpc_;}
  QMetaObject::invokeMethod(&app_,[this,kind,data,rpc]{
   if(failed_)return;
   pendingKind_=kind;pendingRpc_=QString::number(rpc);
   auto object=envelope();object["kind"]=kind;object["rpc"]=pendingRpc_;object["data"]=data;
   if(control_.send("observed",object).isEmpty())fail();
  },Qt::QueuedConnection);
  std::unique_lock lock(pending->mutex);
  const auto timeout=kind=="checkpoint"?std::chrono::minutes(5):std::chrono::minutes(1);
  if(!pending->changed.wait_for(lock,timeout,[&]{return pending->complete;})){
   lock.unlock();std::lock_guard exchangeLock(exchangeMutex_);if(pending_==pending)pending_.reset();return {};
  }
  return pending->reply;
 }
 void abortExchange(){
  std::shared_ptr<Exchange> pending;
  {std::lock_guard lock(exchangeMutex_);aborted_=true;pending=pending_;pending_.reset();}
  if(pending){std::lock_guard lock(pending->mutex);pending->complete=true;pending->reply={};pending->changed.notify_all();}
 }
 void finish(QJsonObject result,bool terminal){
  // Only the child event-loop owner joins the module's original SDK worker.
  module_.reset();deadline_.stop();heartbeat_.stop();
  if(failed_){control_.stop();app_.exit(3);return;}
  if(terminal){terminal_=true;auto object=envelope();object["kind"]="result";object["data"]=result;if(control_.send("checkpoint",object).isEmpty()){fail();return;}deadline_.start(10000);return;}
  QTimer::singleShot(0,&app_,[this]{app_.exit(0);});
 }
 void stopPrepared(){
  if(cleaning_)return;
  cleaning_=true;
  if(!module_){app_.exit(failed_?3:0);return;}
  auto* module=module_.get();
  module->post([this,module]{if(driver_){module->destroyDriver(driver_);driver_=nullptr;}QMetaObject::invokeMethod(&app_,[this]{finish({},false);},Qt::QueuedConnection);});
 }
 void fail(){
  if(failed_)return;
  failed_=true;cancellation_->store(true);deadline_.stop();abortExchange();
  if(!module_)app_.exit(3);else if(!active_)stopPrepared();
 }
 void receive(const QString& type,const QJsonObject& object){
  if(failed_)return;
  if(type=="bootstrap"&&!bootstrapped_){
   nonce_=object["nonce"].toString();generation_=object["generation"].toString();bool ok=false;auto generation=generation_.toULongLong(&ok);
   const QUuid id(nonce_);
   if(object.size()!=7||!valid(object)||id.isNull()||id.toString(QUuid::WithoutBraces)!=nonce_||!ok||generation==0||QString::number(generation)!=generation_||!object["build"].isObject()||object["build"].toObject()!=build_||object["contract"]!=QString::fromUtf8(PUTGD_S3_BUILD_ID)||object["safeDelete"]!=bool(PUTGD_SDK_CURL_TRANSPORT)||!object["modulePath"].isString()){fail();return;}
   bootstrapped_=true;module_=std::make_shared<S3Module>(object["modulePath"].toString());auto* module=module_.get();
   module->post([this,module]{
    Error error;
    driver_=module->createDriver([this](const QString& profile,const QJsonObject& reference)->std::optional<Credentials>{
     const auto data=exchange("credentials",{{"profileId",profile},{"reference",reference}});
     if(data.size()!=1||!data.contains("credentials"))return {};
     if(data["credentials"].isNull())return {};
     Credentials value;if(!data["credentials"].isObject()||!s3wire::decode(data["credentials"].toObject(),value))return {};return value;
    },[this](const Destination& destination){
     const auto data=exchange("readiness",{{"destination",destination.toJson()}});Error error;
     if(data.size()!=1||!data["error"].isObject()||!s3wire::decode(data["error"].toObject(),error))return uncertain();
     return error;
    },error);
    QMetaObject::invokeMethod(&app_,[this,error]{
     if(failed_)return;
     if(error){fail();return;}
     ready_=true;deadline_.stop();auto object=envelope();object["build"]=build_;object["contract"]=QString::fromUtf8(PUTGD_S3_BUILD_ID);object["safeDelete"]=bool(PUTGD_SDK_CURL_TRANSPORT);
     if(control_.send("ready",object).isEmpty())fail();
    },Qt::QueuedConnection);
   });return;
  }
  if(!bootstrapped_||!valid(object)){fail();return;}
  if(type=="shutdown"&&object.size()==4&&object["cancel"]==true){cancellation_->store(true);if(!active_)stopPrepared();return;}
  if(type=="checkpointAccepted"&&terminal_&&object.size()==6&&object["kind"]=="result"&&object["replyTo"]=="terminal"&&object["data"].isObject()&&object["data"].toObject().isEmpty()){deadline_.stop();QTimer::singleShot(0,&app_,[this]{app_.exit(0);});return;}
  if(type=="checkpointAccepted"&&active_&&!terminal_&&object.size()==6&&object["kind"]==pendingKind_&&object["replyTo"]==pendingRpc_&&object["data"].isObject()){
   std::shared_ptr<Exchange> pending;{std::lock_guard lock(exchangeMutex_);pending=pending_;pending_.reset();}
   if(!pending){fail();return;}
   pendingKind_.clear();pendingRpc_.clear();{std::lock_guard lock(pending->mutex);if(pending->complete){fail();return;}pending->reply=object["data"].toObject();pending->complete=true;pending->changed.notify_all();}return;
  }
  if(type!="activate"||!ready_||active_||object.size()!=5||!object["method"].isString()||!object["request"].isObject()||!s3wire::validateRequest(object["method"].toString(),object["request"].toObject())){fail();return;}
  active_=true;heartbeat_.start();
  auto acknowledged=envelope();acknowledged["kind"]="activated";acknowledged["data"]=QJsonObject{{"method",object["method"].toString()}};
  if(control_.send("observed",acknowledged).isEmpty()){fail();return;}
  const auto method=object["method"].toString();const auto request=object["request"].toObject();auto* module=module_.get();
  module->post([this,module,method,request]{
   QJsonObject result;bool delivered=false;
   auto checkpoint=[this](const QJsonObject& progress,std::function<void(Error)> ack){auto data=exchange("checkpoint",{{"progress",progress}});Error error;if(data.size()!=1||!data["error"].isObject()||!s3wire::decode(data["error"].toObject(),error))error=uncertain();ack(error);};
   try{
    if(method=="checkName"){Destination destination;s3wire::decode(request["destination"].toObject(),destination);driver_->checkName(destination,request["candidate"].toString(),[&](NameCheck value){if(!delivered){result=s3wire::encode(value);delivered=true;}});}
    else if(method=="upload"||method=="reconcile"){UploadRequest value;if(method=="reconcile")s3wire::decodeReconcileRequest(request,value);else s3wire::decode(request,value);value.cancellation=cancellation_;if(method=="upload")driver_->upload(value,[&](UploadResult reply){if(!delivered){result=s3wire::encode(reply);delivered=true;}});else driver_->reconcile(value,[&](StatResult reply){if(!delivered){result=s3wire::encode(reply);delivered=true;}});}
    else if(method=="stat"){StorageReceipt value;s3wire::decodeReceiptRequest(request,value);driver_->stat(value,[&](StatResult reply){if(!delivered){result=s3wire::encode(reply);delivered=true;}});}
    else if(method=="remove"){DeleteRequest value;s3wire::decode(request,value);value.cancellation=cancellation_;driver_->remove(value,checkpoint,[&](DeleteResult reply){if(!delivered){result=s3wire::encode(reply);delivered=true;}});}
    else if(method=="testDestination"){Destination destination;s3wire::decode(request["destination"].toObject(),destination);driver_->testDestination(destination,checkpoint,[&](QJsonObject value,Error error){if(!delivered){result={{"result",value},{"error",s3wire::encode(error)}};delivered=true;}});}
   }catch(...){delivered=false;}
   module->destroyDriver(driver_);driver_=nullptr;
   QMetaObject::invokeMethod(&app_,[this,result,delivered]{if(!delivered){failed_=true;abortExchange();}finish(result,delivered);},Qt::QueuedConnection);
  });
 }
 QCoreApplication& app_;UiControl control_;QTimer deadline_,heartbeat_;QJsonObject build_;QString nonce_,generation_,pendingKind_,pendingRpc_;
 std::shared_ptr<S3Module> module_;S3Driver* driver_=nullptr;std::shared_ptr<std::atomic_bool> cancellation_=std::make_shared<std::atomic_bool>(false);
 std::mutex exchangeMutex_;std::shared_ptr<Exchange> pending_;quint64 rpc_=0;bool aborted_=false;
 bool bootstrapped_=false,ready_=false,active_=false,failed_=false,cleaning_=false,terminal_=false;
};
}
}
int main(int argc,char** argv){
 if(argc==2&&std::strcmp(argv[1],"--version-json")==0){std::printf("%s\n",putgd::buildManifestJson());return 0;}
 if(argc!=1)return 2;
 putgd::UiControlPipes handles;if(!handles.valid())return 2;
 QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("putgd-s3-worker");
 putgd::StdioDevice pipes(handles.input(),handles.output());if(!pipes.isOpen())return 2;
 putgd::Worker worker(app,pipes);return app.exec();
}
