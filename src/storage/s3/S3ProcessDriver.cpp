#include "S3ProcessDriver.h"
#include "S3WorkerProtocol.h"
#include "S3Multipart.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
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
   // AWS SDK warnings and errors arrive here; the resident log redacts them.
   const auto bytes=process_->readAllStandardError();stderrBytes_+=bytes.size();output_.append(bytes);if(stderrBytes_>1024*1024)failed_=true;
  });
  QObject::connect(control_.get(),&UiControl::frame,process_.get(),[this](const QString& type,const QString& sequence,const QJsonObject& value){receive(type,sequence,value);});
  auto environment=QProcessEnvironment::systemEnvironment();
  environment.insert("PUTGD_AWS_LOG_LEVEL",logging::debug()?"info":"warn");environment.insert("QT_FORCE_STDERR_LOGGING","1");
  process_->setProcessEnvironment(environment);
  process_->start(helperPath(),{});
  if(!process_->waitForStarted(10000)){output_.failedToStart(process_->errorString());failed_=true;resetProcess();return false;}
  output_.started(process_->processId(),{{"generation",qint64(generation_)}});
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
  // A multipart journal is acknowledged by the caller's durable store. Callers
  // without one (synthetic tests) still get abort on cancel and final failure.
  DurableCheckpoint journal=request.checkpoint?DurableCheckpoint(request.checkpoint):DurableCheckpoint([](const QJsonObject&,std::function<void(Error)> ack){ack({});});
  lanes_=request.extraLanes;trace_=request.trace;
  QJsonObject result;auto error=run("upload",s3wire::encode(request),request.destination,request.cancellation,std::move(journal),result,request.progress);
  lanes_={};trace_={};
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
 void abandon(const Destination& destination,const QJsonObject& journal,std::function<void(Error)> callback)override{
  QJsonObject result;auto error=run("abandon",{{"destination",destination.toJson()},{"journal",journal}},destination,{},{},result);
  Error value;if(!error&&(result.size()!=1||!result["error"].isObject()||!s3wire::decode(result["error"].toObject(),value)))error=uncertain();callback(error?error:value);
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
 void receive(const QString& type,const QString& sequence,const QJsonObject& object){
  if(failed_||!validEnvelope(object)){failed_=true;return;}
  if(type=="ready"&&!ready_&&!activated_&&object.size()==6&&object["build"].toObject()==build()&&object["contract"]==QString::fromUtf8(PUTGD_S3_BUILD_ID)&&object["safeDelete"]==bool(PUTGD_SDK_CURL_TRANSPORT)){ready_=true;return;}
  if(type=="observed"&&activated_&&!terminal_&&object.size()==5&&object["data"].isObject()){
   if(object["kind"]=="activated"&&!activationAcknowledged_&&object["data"].toObject()==QJsonObject{{"method",activeMethod_}}){activationAcknowledged_=true;activity_.restart();return;}
   if(object["kind"]=="alive"&&activationAcknowledged_&&object["data"].toObject().isEmpty()){activity_.restart();return;}
   if(object["kind"]=="uploadProgress"&&activationAcknowledged_&&activeMethod_=="upload"){
    const auto progress=UploadProgress::fromJson(object["data"].toObject());
    if(!progress||!progress->bytesTransferred||progress->bytesTotal!=uploadBytes_){failed_=true;return;}
    activity_.restart();
    if(progress_)try{progress_(*progress);}catch(...){}
    answer("uploadProgress",sequence,{});activity_.restart();return;
   }
   // Per-request diagnostics (#148), batched by the worker; informational only.
   if(object["kind"]=="uploadTrace"&&activationAcknowledged_&&activeMethod_=="upload"){
    const auto data=object["data"].toObject();
    if(data.size()!=2||!data["records"].isArray()||data["records"].toArray().size()>64||!data["dropped"].isDouble()||data["dropped"].toInteger()<0){failed_=true;return;}
    for(const auto& value:data["records"].toArray())if(!value.isObject()||!s3wire::decodeUploadTrace(value.toObject())){failed_=true;return;}
    activity_.restart();
    if(trace_)for(const auto& value:data["records"].toArray())try{trace_(value.toObject());}catch(...){}
    if(const auto dropped=data["dropped"].toInteger())logging::debug(logS3(),"Upload diagnostics dropped",{{"records",dropped}});
    return;
   }
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
 Error run(const QString& method,const QJsonObject& request,const Destination& destination,const std::shared_ptr<std::atomic_bool>& cancellation,DurableCheckpoint checkpoint,QJsonObject& result,std::function<void(const UploadProgress&)> progress={}){
  if(!s3wire::validateRequest(method,request))return invalidRequest(method,request);
  destination_=destination;checkpoint_=std::move(checkpoint);rpc_=0;activeMethod_=method;
  progress_=std::move(progress);uploadBytes_=method=="upload"?request["artifact"].toObject()["bytes"].toInteger():0;
  if(!prepare()){progress_={};return unavailable();}
  auto activate=envelope();activate["method"]=method;activate["request"]=request;
  // SDK CURL bounds each HTTP exchange; allow the complete driver's bounded
  // call chain, slow local artifact hashing and durable persistence separately.
  // A multipart upload makes up to three attempts per part plus its create,
  // listing, completion and abort calls; each part is bounded like one PUT.
  const auto resume=request["resume"].toObject();const qint64 parts=method=="upload"?qBound<qint64>(0LL,s3PartCount(request["artifact"].toObject()["bytes"].toInteger(),qBound<qint64>(0LL,resume["partBytes"].toInteger(),s3MaximumPartBytes)),s3MaximumParts):0;
  const qint64 calls=method=="testDestination"?20:method=="remove"?8:method=="abandon"?3:method=="upload"?6+parts*3+(parts?16:0):1;
  const auto settings=destination.settings;
  const auto timeout=[&](const char* key){return qBound<qint64>(1LL,settings[key].toInteger(),2147483647LL);};
  const qint64 perCall=qMax<qint64>(timeout("requestTimeoutMs"),timeout("connectTimeoutMs")+timeout("lowSpeedTimeSeconds")*1000)+1000;
  // Clamped protocol domains keep all budget arithmetic below 2^63 even for
  // malformed direct driver calls, before child request validation rejects them.
  const qint64 bytes=method=="upload"?qBound<qint64>(0LL,request["artifact"].toObject()["bytes"].toInteger(),1099511627776LL):0;
  const qint64 hashBudget=qMax<qint64>(60000,((bytes+16383)/16384)*4000);
  const qint64 checkpoints=method=="testDestination"?12:method=="remove"?8:method=="upload"&&parts?4:0;
  const qint64 cancellationGrace=limits_.cancellationGraceMs>0?limits_.cancellationGraceMs:qBound<qint64>(30000LL,perCall+5000,120000LL);
  QElapsedTimer cancelDeadline;
  const qint64 totalBudget=calls*perCall+hashBudget+checkpoints*300000+60000;
  QElapsedTimer workDeadline;workDeadline.start();activity_.start();
  // A multipart upload learns how many idle upload slots it may borrow (#174).
  QElapsedTimer lanesPoll;lanesPoll.start();int lentLanes=0;const bool borrows=lanes_&&parts>1;
  activated_=true;if(control_->send("activate",activate).isEmpty())failed_=true;
  while(process_->state()!=QProcess::NotRunning&&!failed_){
   flushAck();
   if(borrows&&activationAcknowledged_&&!terminal_&&lanesPoll.elapsed()>=100){
    lanesPoll.restart();int lent=0;try{lent=qBound(0,lanes_(s3MaximumPartLanes-1),s3MaximumPartLanes-1);}catch(...){}
    if(lent!=lentLanes){auto value=envelope();value["lanes"]=lent;if(control_->send("lanes",value).isEmpty())failed_=true;lentLanes=lent;}
   }
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
  resetProcess();checkpoint_={};progress_={};destination_={};return success?Error{}:uncertain();
 }
 void closePrepared(){
  if(process_&&ready_&&!activated_&&!failed_){auto value=envelope();value["cancel"]=true;control_->send("shutdown",value);process_->waitForFinished(10000);}
  resetProcess();
 }
 void resetProcess(){
  if(pending_){std::lock_guard lock(pending_->mutex);pending_->closed=true;}pending_.reset();
  if(control_)control_->stop();
  bool requested=false;
  if(process_&&process_->state()!=QProcess::NotRunning){requested=true;process_->closeWriteChannel();process_->terminate();if(!process_->waitForFinished(1000)){process_->kill();process_->waitForFinished(2000);}}
  if(process_&&output_.pid()>0) {
   output_.append(process_->readAllStandardError());
   output_.finished(process_->exitCode(),process_->exitStatus(),requested);
  }
  control_.reset();process_.reset();ready_=false;stderrBytes_=0;
 }
 QString path_,nonce_,pendingRpc_,activeMethod_;QElapsedTimer activity_,checkpointDeadline_,exitDeadline_;quint64 generation_=0,rpc_=0;CredentialLookup credentials_;ReadinessCheck readiness_;
 std::unique_ptr<QProcess> process_;std::unique_ptr<UiControl> control_;std::shared_ptr<Ack> pending_;
 S3ProcessLimits limits_;Destination destination_;DurableCheckpoint checkpoint_;QJsonObject reply_;
 std::function<void(const UploadProgress&)> progress_;qint64 uploadBytes_=0;
 std::function<int(int)> lanes_;std::function<void(const QJsonObject&)> trace_;
 bool ready_=false,activated_=false,activationAcknowledged_=false,terminal_=false,failed_=false,cancelSent_=false,checkpointException_=false;qint64 stderrBytes_=0;
 HelperOutput output_{"putgd-s3-worker",logS3(),true};
};
}
S3Driver* createS3ProcessDriver(const QString& path,CredentialLookup credentials,ReadinessCheck readiness,Error& error,S3ProcessLimits limits){
 auto driver=std::make_unique<ProcessDriver>(path,std::move(credentials),std::move(readiness),limits);
 if(!driver->prepare()){error=unavailable();return nullptr;}return driver.release();
}
}
