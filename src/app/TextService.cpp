#include "TextService.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace putgd {
namespace {
Error notFound(){return {"operation_not_found","Operation not found",{}};}
QJsonArray strings(const QStringList& values){QJsonArray array;for(const auto& value:values)array.append(value);return array;}
// "German" for deu; the identifier itself when Qt does not know the code.
QString languageName(const QString& identifier){
 const auto language=QLocale::codeToLanguage(identifier.section('_',0,0));
 return language==QLocale::AnyLanguage||language==QLocale::C?identifier:QLocale::languageToString(language);
}
}
HelperTextBackend::HelperTextBackend(QString helperPath,int timeoutMs,QStringList dataDirectories):job_(std::move(helperPath),timeoutMs),dataDirectories_(std::move(dataDirectories)){
 if(!dataDirectories_.isEmpty())job_.setDataDirectories(dataDirectories_);
}
QList<textjob::Language> HelperTextBackend::languages() const {
 if(dataDirectories_.isEmpty())return textjob::installedLanguages(textjob::dataDirectories(QDir::homePath(),QCoreApplication::applicationDirPath()));
 QList<textjob::DataDirectory> directories;for(const auto& path:dataDirectories_)directories.append({path,"user"});
 return textjob::installedLanguages(directories);
}
int HelperTextBackend::landlockAbi(){
#if defined(Q_OS_LINUX)&&defined(SYS_landlock_create_ruleset)
 // LANDLOCK_CREATE_RULESET_VERSION: asks for the ABI and creates nothing.
 const long abi=::syscall(SYS_landlock_create_ruleset,nullptr,0,1U);
 return abi>0?int(abi):0;
#else
 return 0;
#endif
}
QJsonObject HelperTextBackend::capabilities() const {
 QJsonArray languages;bool english=false;
 for(const auto& language:this->languages()){languages.append(QJsonObject{{"id",language.id},{"bytes",double(language.bytes)},{"kind",language.kind}});english=english||language.id=="eng";}
 const int abi=landlockAbi();const QFileInfo helper(job_.helper());Error error;
 if(!helper.isFile()||!helper.isExecutable())error=textjob::failure("text_helper_missing");
 else if(abi<3){error=textjob::failure("helper_sandbox_unavailable");error.details["landlockAbi"]=abi;}
 else if(!english)error=textjob::failure("engine_unavailable");
 QJsonObject report{{"available",!error},{"engines",QJsonDocument::fromJson(buildManifestJson()).object()["textEngines"].toObject()},{"landlockAbi",abi},{"languages",languages}};
 if(error)report["error"]=error.toJson();
 return report;
}

QStringList TextService::sources(){return {"region","window","monitor","desktop","clipboard","file","history"};}
TextService::TextService(Host host,QObject* parent):QObject(parent),host_(std::move(host)),backend_(std::make_shared<HelperTextBackend>()){
 expiry_.setSingleShot(true);connect(&expiry_,&QTimer::timeout,this,&TextService::dropCache);
}
TextService::~TextService(){shutdown();}
void TextService::setBackend(std::unique_ptr<TextBackend> backend){if(!active()&&backend)backend_=std::move(backend);}
QJsonObject TextService::capabilities(const QHash<QString,Error>& sourceErrors) const {
 auto report=backend_->capabilities();const bool available=report["available"].toBool();QJsonObject perSource;
 for(const auto& source:sources()){
  QJsonObject entry{{"available",available&&!sourceErrors.contains(source)}};
  if(sourceErrors.contains(source))entry["error"]=sourceErrors.value(source).toJson();
  else if(!available)entry["error"]=report["error"];
  perSource[source]=entry;
 }
 report["sources"]=perSource;return report;
}
void TextService::recognize(Request request,JsonReply reply){
 if(!sources().contains(request.source)||!request.acquire||(!request.languages.isEmpty()&&!textjob::validLanguages(request.languages))){reply({"invalid_request","Invalid text recognition parameters",{}},{});return;}
 // Nothing queues: a second press hears that the first is still running.
 if(active()){reply(textjob::failure("text_busy"),{});return;}
 const auto report=backend_->capabilities();
 if(!report["available"].toBool()){const auto error=report["error"].toObject();reply({error["code"].toString("engine_unavailable"),error["message"].toString(),error["details"].toObject()},{});return;}
 // A language a caller names has to be installed; one from Settings may be skipped.
 if(!request.languages.isEmpty()){
  const auto installed=backend_->languages();
  for(const auto& language:std::as_const(request.languages))if(std::none_of(installed.begin(),installed.end(),[&](const textjob::Language& entry){return entry.id==language;})){reply(textjob::failure("language_unavailable",language),{});return;}
 }
 Operation op;op.action=Settings::textActionName();
 if(auto error=host_.accept(op,{request.interactive,request.copy})){reply(error,{});return;}
 // A kept result lasts until the next recognition is accepted.
 dropCache();
 op.phase="accepted";op.copyStatus=request.copy?"pending":"not_requested";
 id_=op.id;op_=op;request_=std::move(request);cancel_=std::make_shared<std::atomic_bool>(false);delivered_=false;waiting_.reset();copyMs_=0;
 const auto id=id_;QPointer<TextService> guard(this);
 // The adapter freezes its target, or shows its selector, before the journal yields.
 if(request_.prepare)request_.prepare(op_,deliverer(id));
 persisting_=true;
 host_.persist(op_,[guard,id,reply](Error error){
  if(!guard||guard->id_!=id){reply(textjob::failure("cancelled"),{});return;}
  guard->persisting_=false;
  if(error){guard->terminate(error,false);reply(error,{});return;}
  guard->host_.observe(guard->op_);reply({},{{"operationId",id},{"state","accepted"}});
  // Acceptance is durable and observable before the source is read.
  QTimer::singleShot(0,guard.data(),[guard,id]{
   if(!guard||guard->id_!=id)return;
   if(guard->cancel_->load()){guard->terminate({},true);return;}
   if(guard->settle(id))return;
   guard->advance(id,OperationState::Acquiring,[guard,id]{guard->request_.acquire(guard->deliverer(id));});
  });
 });
}
TextService::Deliver TextService::deliverer(const QString& id){
 QPointer<TextService> guard(this);
 return [guard,id](Error error,Load load){
  if(guard)QMetaObject::invokeMethod(guard.data(),[guard,id,error,load]{if(guard)guard->delivered(id,error,load);},Qt::QueuedConnection);
 };
}
void TextService::delivered(const QString& id,Error error,Load load){
 if(id_!=id||delivered_)return;
 delivered_=true;waiting_=Delivery{std::move(error),std::move(load)};
 // Before the operation is journaled as acquiring, the step that is running picks it up.
 if(!persisting_&&op_.state()==OperationState::Acquiring)settle(id);
}
bool TextService::settle(const QString& id){
 if(!waiting_)return false;
 auto delivery=*std::exchange(waiting_,std::nullopt);
 acquired(id,std::move(delivery.error),std::move(delivery.load));return true;
}
void TextService::advance(const QString& id,OperationState state,std::function<void()> next){
 op_.advance(state);op_.phase=op_.stateName();persisting_=true;QPointer<TextService> guard(this);
 host_.persist(op_,[guard,id,next=std::move(next)](Error error){
  if(!guard||guard->id_!=id)return;
  guard->persisting_=false;
  if(error){guard->terminate(error,false);return;}
  if(guard->cancel_->load()){guard->terminate({},true);return;}
  guard->host_.observe(guard->op_);
  if(guard->settle(id))return;
  next();
 });
}
void TextService::acquired(const QString& id,Error error,Load load){
 if(id_!=id)return;
 if(cancel_->load()||error.code=="cancelled"||error.code=="source_cancelled"){terminate({},true);return;}
 if(error){terminate(error,false);return;}
 if(!load){terminate({"source_unsupported","This source holds no image to read",{}},false);return;}
 if(op_.state()!=OperationState::Acquiring){terminate({"source_unavailable","The source answered before it was asked",{}},false);return;}
 QPointer<TextService> guard(this);
 // Recognition never starts before the journal says so.
 advance(id,OperationState::Recognizing,[guard,id,load]{
  textjob::Options options;options.words=guard->request_.keepResult;options.languagesRequired=!guard->request_.languages.isEmpty();
  options.languages=options.languagesRequired?guard->request_.languages
   :textjob::defaultLanguages(guard->host_.settings().textRecognitionLanguages,guard->host_.locale?guard->host_.locale():QLocale::system().name(),guard->backend_->languages());
  auto result=std::make_shared<TextResult>();auto failure=std::make_shared<Error>();
  guard->host_.background([backend=guard->backend_,load,options,result,failure,cancel=guard->cancel_]{
   QImage grey;
   {
    // The colour copy goes as soon as the grey one exists.
    auto pixels=load(*failure);if(*failure)return;
    if(pixels.isNull()){*failure={"source_unsupported","This source holds no image to read",{}};return;}
    if(qint64(pixels.width())*pixels.height()>textjob::maximumInputPixels||pixels.width()>textjob::maximumEdge||pixels.height()>textjob::maximumEdge){*failure=textjob::failure("input_excessive");return;}
    grey=TextJob::grey(pixels);
   }
   if(cancel->load()){*failure=textjob::failure("cancelled");return;}
   *result=backend->recognize(grey,options,*failure,cancel);
  },[guard,id,result,failure]{if(guard)guard->recognized(id,*failure,result);});
 });
}
void TextService::recognized(const QString& id,Error error,std::shared_ptr<TextResult> result){
 if(id_!=id)return;
 if(cancel_->load()||error.code=="cancelled"){terminate({},true);return;}
 if(error){terminate(error,false);return;}
 if(!request_.copy){complete(id,result,"not_requested");return;}
 const auto text=textjob::copyText(*result);
 // Nothing found: the clipboard stays as it is.
 if(text.isEmpty()){complete(id,result,"no_result");return;}
 auto timer=std::make_shared<QElapsedTimer>();timer->start();QPointer<TextService> guard(this);
 host_.copy(op_,text,[guard,id,result,timer](Error copyError){
  if(!guard||guard->id_!=id)return;
  guard->copyMs_=timer->elapsed();
  guard->complete(id,result,!copyError?"copied":copyError.code=="copy_suppressed"?"suppressed":"failed");
 });
}
void TextService::complete(const QString& id,std::shared_ptr<TextResult> result,const QString& copyStatus){
 op_.advance(OperationState::Completed);op_.phase="completed";op_.copyStatus=copyStatus;
 QJsonObject record{{"operationId",id},{"state","completed"},{"source",request_.source},{"characterCount",double(result->text.toUcs4().size())},{"codeCount",result->textCodes()},{"copyStatus",copyStatus},
  {"truncated",result->truncated},{"banded",result->banded},{"languagesUsed",strings(result->languagesUsed)},{"languagesSkipped",strings(result->languagesSkipped)},
  {"timings",QJsonObject{{"spawnMs",double(result->spawnMs)},{"engineStartMs",double(result->engineStartMs)},{"recognizeMs",double(result->recognizeMs)},{"copyMs",double(copyMs_)},{"totalMs",double(result->totalMs)}}}};
 if(request_.keepResult)keep(id,*result);
 QString notification;
 if(request_.copy){QStringList missing;for(const auto& language:result->languagesSkipped)missing.append(languageName(language));notification=textjob::notification(*result,copyStatus,missing);}
 end(std::move(record),notification);
}
void TextService::terminate(Error error,bool cancelled){
 if(!active())return;
 // A selector that is still up, or a capture still running, ends with the operation.
 if(!delivered_&&request_.cancelAcquisition&&(request_.prepare||op_.state()==OperationState::Acquiring)){delivered_=true;request_.cancelAcquisition();}
 op_.error=cancelled?Error{}:error;op_.cancelled=cancelled;op_.advance(cancelled?OperationState::Cancelled:OperationState::Failed);op_.phase=op_.stateName();op_.copyStatus=request_.copy?"no_result":"not_requested";
 QJsonObject record{{"operationId",id_},{"state",op_.stateName()},{"source",request_.source},{"copyStatus",op_.copyStatus}};
 if(!cancelled)record["error"]=error.toJson();
 end(std::move(record),{});
}
void TextService::end(QJsonObject record,const QString& notification){
 const auto id=id_;const auto op=op_;
 records_[id]=record;recordOrder_.append(id);while(recordOrder_.size()>terminalRecords)records_.remove(recordOrder_.takeFirst());
 id_.clear();op_={};request_={};cancel_.reset();waiting_.reset();persisting_=false;
 const auto waiting=std::exchange(cancelReplies_,{});
 host_.finish(op);
 QJsonObject answer;inspect(id,answer);
 for(const auto& reply:waiting)reply({},answer);
 emit finished(id,record,notification);
}
void TextService::keep(const QString& id,const TextResult& result){
 // The text first, then codes, then as many words as fit.
 auto words=result.words;bool cut=result.truncated;QByteArray bytes;
 const auto serialize=[&]{return QJsonDocument(QJsonObject{{"text",result.text},{"codes",textjob::codesJson(result.codes)},{"words",textjob::wordsJson(words)},{"truncated",cut}}).toJson(QJsonDocument::Compact);};
 bytes=serialize();
 while(bytes.size()>resultCacheBytes&&!words.isEmpty()){cut=true;words.resize(words.size()/2);bytes=serialize();}
 if(bytes.size()>resultCacheBytes)return; // the helper's own bounds keep text and codes far below this
 bytes.squeeze();cache_=std::move(bytes);cacheId_=id;expiry_.start(lifetimeMs_);
}
void TextService::dropCache(){
 expiry_.stop();
 if(cache_.isNull()&&cacheId_.isEmpty())return;
 cache_=QByteArray();cacheId_.clear();
 if(host_.reclaim)host_.reclaim();
}
Error TextService::inspect(const QString& id,QJsonObject& result) const {
 result={};
 if(active()&&id==id_){result={{"operationId",id},{"state",op_.stateName()},{"source",request_.source}};return {};}
 const auto record=records_.constFind(id);if(record==records_.cend())return notFound();
 result=*record;
 if(result["state"]!=QLatin1String("completed"))return {};
 // The result is attached only while it is kept; nothing else ever held it.
 const bool kept=cacheId_==id&&!cache_.isEmpty();result["resultExpired"]=!kept;
 if(kept){const auto cached=QJsonDocument::fromJson(cache_).object();for(const auto* key:{"text","words","codes"})result[QLatin1String(key)]=cached[QLatin1String(key)];if(cached["truncated"].toBool())result["truncated"]=true;}
 return {};
}
void TextService::cancel(const QString& id,JsonReply reply){
 if(!active()||id!=id_){QJsonObject record;const auto error=inspect(id,record);reply(error,record);return;}
 cancel_->store(true);cancelReplies_.append(std::move(reply));
 if(persisting_||op_.state()!=OperationState::Acquiring||delivered_)return; // the step that is running ends it
 if(request_.cancelAcquisition)request_.cancelAcquisition();
 // An acquisition that cannot be told to stop is left behind; what it delivers later is dropped.
 else terminate({},true);
}
void TextService::shutdown(){
 if(active()){cancel_->store(true);terminate({},true);}
 expiry_.stop();cache_=QByteArray();cacheId_.clear();
}
}
