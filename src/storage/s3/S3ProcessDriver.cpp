#include "S3ProcessDriver.h"
#include "S3WorkerProtocol.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <mutex>
namespace putgd {
namespace {
QJsonObject build(){return QJsonDocument::fromJson(buildManifestJson()).object();}
Error unavailable(){return {"s3_module_unavailable","S3 support is missing or incompatible; restart put.gd after an update. If it remains unavailable, reinstall this build of put.gd",{}};}
bool noEffects(const Error& error){return error && error.code != "s3_transport_unknown";}
Error invalidRequest(const QString& method,const QJsonObject& request){
 if(method=="stat"||method=="remove")return {"receipt_invalid","Recorded storage receipt is invalid",{}};
 if(method=="checkName"){
  Destination destination;
  if(request["destination"].isObject()&&s3wire::decode(request["destination"].toObject(),destination)){
   auto prefix=destination.settings["objectPrefix"].toString();
   if(!prefix.isEmpty()&&!prefix.endsWith('/'))prefix+='/';
   const auto key=prefix+request["candidate"].toString();
   if(key.toUtf8().size()>1024||key.contains(QChar(0)))return {"s3_key_invalid","S3 key exceeds its byte limit",{}};
  }
 }
 if(method=="upload"){
  Artifact artifact;
  if(!request["artifact"].isObject()||!s3wire::decode(request["artifact"].toObject(),artifact))return {"artifact_invalid","Retained artifact metadata is invalid",{}};
  const auto candidate=request["candidate"].toString();
  if(candidate.toUtf8().size()>1024||candidate.contains(QChar(0)))return {"s3_key_invalid","S3 key exceeds its byte limit",{}};
  return {"s3_precondition_invalid","Persist an exact object key and write precondition before upload",{}};
 }
 return {"s3_request_invalid","Storage request does not match its supported schema",{}};
}
Error uncertain(){return {"s3_transport_unknown","S3 work ended unexpectedly; reconcile its recorded target before retrying",{}};}
QString helperPath(){
 if(QDir::cleanPath(QCoreApplication::applicationDirPath())==QString::fromUtf8(PUTGD_S3_BUILD_APP_DIR))return QString::fromUtf8(PUTGD_S3_WORKER_PATH);
 return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("putgd-s3-worker");
}
struct Ack {std::mutex mutex;bool complete=false,closed=false;Error error;};
class ProcessDriver final:public S3Driver {
public:
 ProcessDriver(QString path,CredentialLookup credentials,ReadinessCheck readiness,S3ProcessLimits limits):path_(std::move(path)),credentials_(std::move(credentials)),readiness_(std::move(readiness)),limits_(limits){}
 ~ProcessDriver()override{closePrepared();}
 bool prepare(){
  if(process_)return ready_&&!failed_;
  ready_=activated_=activationAcknowledged_=terminal_=failed_=cancelSent_=checkpointException_=false;reply_={};pending_.reset();
  nonce_=QUuid::createUuid().toString(QUuid::WithoutBraces);++generation_;
  process_=std::make_unique<QProcess>();process_->setProcessChannelMode(QProcess::SeparateChannels);
  process_->setReadChannel(QProcess::StandardOutput);
  control_=std::make_unique<UiControl>(*process_);
  QObject::connect(control_.get(),&UiControl::failed,process_.get(),[this](Error){failed_=true;});
  QObject::connect(process_.get(),&QProcess::readyReadStandardError,process_.get(),[this]{
   while(process_->bytesAvailable()>0&&process_->readChannel()==QProcess::StandardError)process_->read(16384);
   auto discarded=process_->readAllStandardError();stderrBytes_+=discarded.size();if(stderrBytes_>1024*1024)failed_=true;
  });
  QObject::connect(control_.get(),&UiControl::frame,process_.get(),[this](const QString& type,const QString&,const QJsonObject& value){receive(type,value);});
  process_->start(helperPath(),{});
  if(!process_->waitForStarted(10000)){failed_=true;resetProcess();return false;}
  auto boot=envelope();boot["build"]=build();boot["contract"]=QString::fromUtf8(PUTGD_S3_BUILD_ID);boot["safeDelete"]=bool(PUTGD_SDK_CURL_TRANSPORT);boot["modulePath"]=path_;
  if(control_->send("bootstrap",boot).isEmpty())failed_=true;
  QElapsedTimer clock;clock.start();
  while(!ready_&&!failed_&&process_->state()!=QProcess::NotRunning&&clock.elapsed()<10000)process_->waitForReadyRead(20);
  if(!ready_||failed_){failed_=true;resetProcess();return false;}
  return true;
 }
 void checkName(const Destination& destination,const QString& candidate,std::function<void(NameCheck)> callback)override{
  QJsonObject result;auto error=run("checkName",{{"destination",destination.toJson()},{"candidate",candidate}},destination,{}, {},result);
  NameCheck value;if(error||!s3wire::decode(result,value))value={"inconclusive",{}, {},error?error:uncertain()};callback(std::move(value));
 }
 void upload(const UploadRequest& request,std::function<void(UploadResult)> callback)override{
  QJsonObject result;auto error=run("upload",s3wire::encode(request),request.destination,request.cancellation,{},result);
  UploadResult value;if(error||!s3wire::decode(result,value))value={noEffects(error)?"failed":"unknown",{},error?error:uncertain()};callback(std::move(value));
 }
 void stat(const StorageReceipt& receipt,std::function<void(StatResult)> callback)override{
  QJsonObject result;auto error=run("stat",receipt.toJson(),receipt.destination,{}, {},result);
  StatResult value;if(error||!s3wire::decode(result,value))value={"unknown",{},error?error:uncertain()};callback(std::move(value));
 }
 void reconcile(const UploadRequest& request,std::function<void(StatResult)> callback)override{
  QJsonObject result;auto error=run("reconcile",s3wire::encode(request),request.destination,request.cancellation,{},result);
  StatResult value;if(error||!s3wire::decode(result,value))value={"unknown",{},error?error:uncertain()};callback(std::move(value));
 }
 void remove(const DeleteRequest& request,DurableCheckpoint checkpoint,std::function<void(DeleteResult)> callback)override{
  QJsonObject result;auto error=run("remove",s3wire::encode(request),request.receipt.destination,request.cancellation,std::move(checkpoint),result);
  DeleteResult value;if(error||!s3wire::decode(result,value))value={noEffects(error)?"failed":"unknown",noEffects(error)?request.progress:QJsonObject{},error?error:uncertain()};callback(std::move(value));
 }
 void testDestination(const Destination& destination,DurableCheckpoint checkpoint,std::function<void(QJsonObject,Error)> callback)override{
  QJsonObject result;auto error=run("testDestination",{{"destination",destination.toJson()}},destination,{},std::move(checkpoint),result);
  Error replyError;
  if(error||result.size()!=2||!result["result"].isObject()||!result["error"].isObject()||!s3wire::decode(result["error"].toObject(),replyError)||!s3wire::decodeTestDestinationResult(result["result"].toObject()))callback({},error?error:uncertain());
  else callback(result["result"].toObject(),replyError);
 }
private:
 qint64 checkpointLimit()const{return limits_.checkpointDeadlineMs>0?limits_.checkpointDeadlineMs:300000;}
 QJsonObject envelope()const{return {{"role","s3-worker"},{"nonce",nonce_},{"generation",QString::number(generation_)}};}
 bool validEnvelope(const QJsonObject& object)const{return object["role"]=="s3-worker"&&object["nonce"]==nonce_&&object["generation"]==QString::number(generation_);}
 void answer(const QString& kind,const QString& rpc,const QJsonObject& data){auto value=envelope();value["kind"]=kind;value["replyTo"]=rpc;value["data"]=data;if(control_->send("checkpointAccepted",value).isEmpty())failed_=true;}
 void receive(const QString& type,const QJsonObject& object){
  if(failed_||!validEnvelope(object)){failed_=true;return;}
  if(type=="ready"&&!ready_&&!activated_&&object.size()==6&&object["build"].toObject()==build()&&object["contract"]==QString::fromUtf8(PUTGD_S3_BUILD_ID)&&object["safeDelete"]==bool(PUTGD_SDK_CURL_TRANSPORT)){ready_=true;return;}
  if(type=="observed"&&activated_&&!terminal_&&object.size()==5&&object["data"].isObject()){
   if(object["kind"]=="activated"&&!activationAcknowledged_&&object["data"].toObject()==QJsonObject{{"method",activeMethod_}}){activationAcknowledged_=true;activity_.restart();return;}
   if(object["kind"]=="alive"&&activationAcknowledged_&&object["data"].toObject().isEmpty()){activity_.restart();return;}
   failed_=true;return;
  }
  if(type=="checkpoint"&&activated_&&activationAcknowledged_&&!terminal_&&object.size()==5&&object["kind"]=="result"&&object["data"].isObject()&&!pending_){terminal_=true;reply_=object["data"].toObject();exitDeadline_.start();answer("result","terminal",{});return;}
  if(type!="observed"||!activated_||!activationAcknowledged_||terminal_||object.size()!=6||!object["kind"].isString()||!object["rpc"].isString()||!object["data"].isObject()||pending_){failed_=true;return;}
  activity_.restart();
  const auto kind=object["kind"].toString(),rpc=object["rpc"].toString();bool ok=false;const auto number=rpc.toULongLong(&ok);
  if(!ok||number!=rpc_+1||QString::number(number)!=rpc){failed_=true;return;}rpc_=number;
  const auto data=object["data"].toObject();
  if(kind=="credentials"){
   if(data.size()!=2||data["profileId"]!=destination_.profileId||data["reference"].toObject()!=destination_.credentialReference||!data["reference"].isObject()){failed_=true;return;}
   const auto credentials=credentials_(destination_.profileId,destination_.credentialReference);
   answer(kind,rpc,{{"credentials",credentials?QJsonValue(s3wire::encode(*credentials)):QJsonValue(QJsonValue::Null)}});activity_.restart();return;
  }
  if(kind=="readiness"){
   if(data.size()!=1||data["destination"].toObject()!=destination_.toJson()||!data["destination"].isObject()){failed_=true;return;}
   answer(kind,rpc,{{"error",s3wire::encode(readiness_(destination_))}});activity_.restart();return;
  }
  if(kind=="checkpoint"&&checkpoint_){
   if(data.size()!=1||!data["progress"].isObject()){failed_=true;return;}
   pending_=std::make_shared<Ack>();pendingRpc_=rpc;checkpointDeadline_.start();
   const auto ack=pending_;
   try{checkpoint_(data["progress"].toObject(),[ack](Error error){std::lock_guard lock(ack->mutex);if(!ack->closed&&!ack->complete){ack->error=std::move(error);ack->complete=true;}});}
   catch(...){checkpointException_=true;std::lock_guard lock(ack->mutex);ack->error=uncertain();ack->complete=true;}
   // The host journal/cache callback owns this thread while it runs. Its time
   // remains charged to the total/checkpoint bounds, never to child silence.
   activity_.restart();
   return;
  }
  failed_=true;
 }
 void flushAck(){
  if(!pending_)return;
  if(checkpointDeadline_.elapsed()>checkpointLimit()){failed_=true;return;}
  Error error;
  {std::lock_guard lock(pending_->mutex);if(!pending_->complete)return;error=pending_->error;pending_->closed=true;}
  answer("checkpoint",pendingRpc_,{{"error",s3wire::encode(error)}});pending_.reset();pendingRpc_.clear();activity_.restart();
 }
 Error run(const QString& method,const QJsonObject& request,const Destination& destination,const std::shared_ptr<std::atomic_bool>& cancellation,DurableCheckpoint checkpoint,QJsonObject& result){
  if(!s3wire::validateRequest(method,request))return invalidRequest(method,request);
  destination_=destination;checkpoint_=std::move(checkpoint);rpc_=0;activeMethod_=method;
  if(!prepare())return unavailable();
  auto activate=envelope();activate["method"]=method;activate["request"]=request;
  // SDK CURL bounds each HTTP exchange; allow the complete driver's bounded
  // call chain, slow local artifact hashing and durable persistence separately.
  const qint64 calls=method=="testDestination"?16:method=="remove"?8:method=="upload"?6:1;
  const auto settings=destination.settings;
  const auto timeout=[&](const char* key){return qBound<qint64>(1LL,settings[key].toInteger(),2147483647LL);};
  const qint64 perCall=qMax<qint64>(timeout("requestTimeoutMs"),timeout("connectTimeoutMs")+timeout("lowSpeedTimeSeconds")*1000)+1000;
  // Clamped protocol domains keep all budget arithmetic below 2^63 even for
  // malformed direct driver calls, before child request validation rejects them.
  const qint64 bytes=method=="upload"?qBound<qint64>(0LL,request["artifact"].toObject()["bytes"].toInteger(),1099511627776LL):0;
  const qint64 hashBudget=qMax<qint64>(60000,((bytes+16383)/16384)*4000);
  const qint64 checkpoints=method=="testDestination"?10:method=="remove"?8:0;
  const qint64 cancellationGrace=limits_.cancellationGraceMs>0?limits_.cancellationGraceMs:qBound<qint64>(30000LL,perCall+5000,120000LL);
  QElapsedTimer cancelDeadline;
  const qint64 totalBudget=calls*perCall+hashBudget+checkpoints*300000+60000;
  QElapsedTimer workDeadline;workDeadline.start();activity_.start();
  activated_=true;if(control_->send("activate",activate).isEmpty())failed_=true;
  while(process_->state()!=QProcess::NotRunning&&!failed_){
   flushAck();
   if(cancellation&&cancellation->load()&&!cancelSent_){auto cancel=envelope();cancel["cancel"]=true;if(control_->send("shutdown",cancel).isEmpty())failed_=true;cancelSent_=true;cancelDeadline.start();}
   process_->waitForReadyRead(20);
   if((!activationAcknowledged_&&workDeadline.elapsed()>10000)||
      (activationAcknowledged_&&!pending_&&activity_.elapsed()>(limits_.childSilenceMs>0?limits_.childSilenceMs:15000))||workDeadline.elapsed()>totalBudget||
      (pending_&&checkpointDeadline_.elapsed()>checkpointLimit())||(terminal_&&exitDeadline_.elapsed()>5000)||(cancelSent_&&cancelDeadline.elapsed()>cancellationGrace))failed_=true;
  }
  // Process EOF may leave a final complete frame buffered.
  if(!failed_&&process_->bytesAvailable())process_->waitForReadyRead(1);
  const bool success=!failed_&&!checkpointException_&&terminal_&&process_->state()==QProcess::NotRunning&&process_->exitStatus()==QProcess::NormalExit&&process_->exitCode()==0;
  if(success)result=reply_;
  resetProcess();checkpoint_={};destination_={};return success?Error{}:uncertain();
 }
 void closePrepared(){
  if(process_&&ready_&&!activated_&&!failed_){auto value=envelope();value["cancel"]=true;control_->send("shutdown",value);process_->waitForFinished(10000);}
  resetProcess();
 }
 void resetProcess(){
  if(pending_){std::lock_guard lock(pending_->mutex);pending_->closed=true;}pending_.reset();
  if(control_)control_->stop();
  if(process_&&process_->state()!=QProcess::NotRunning){process_->closeWriteChannel();process_->terminate();if(!process_->waitForFinished(1000)){process_->kill();process_->waitForFinished(2000);}}
  control_.reset();process_.reset();ready_=false;stderrBytes_=0;
 }
 QString path_,nonce_,pendingRpc_,activeMethod_;QElapsedTimer activity_,checkpointDeadline_,exitDeadline_;quint64 generation_=0,rpc_=0;CredentialLookup credentials_;ReadinessCheck readiness_;
 std::unique_ptr<QProcess> process_;std::unique_ptr<UiControl> control_;std::shared_ptr<Ack> pending_;
 S3ProcessLimits limits_;Destination destination_;DurableCheckpoint checkpoint_;QJsonObject reply_;
 bool ready_=false,activated_=false,activationAcknowledged_=false,terminal_=false,failed_=false,cancelSent_=false,checkpointException_=false;qint64 stderrBytes_=0;
};
}
S3Driver* createS3ProcessDriver(const QString& path,CredentialLookup credentials,ReadinessCheck readiness,Error& error,S3ProcessLimits limits){
 auto driver=std::make_unique<ProcessDriver>(path,std::move(credentials),std::move(readiness),limits);
 if(!driver->prepare()){error=unavailable();return nullptr;}return driver.release();
}
}
