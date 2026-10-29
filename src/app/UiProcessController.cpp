#include "UiProcessController.h"
#include "core/BuildInfo.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <utility>
namespace putgd {
namespace {
Error unavailable(const QString& code="ui_unavailable") {
 if(code=="ui_incompatible")return {code,"The installed UI version does not match the running app. Restart put.gd after pending uploads finish.",{}};
 return {code,"The window could not be opened. Try opening it again from the tray.",{}};
}
QJsonObject build(){return QJsonDocument::fromJson(buildManifestJson()).object();}
}
UiProcessController::UiProcessController(QString socket,QString epoch,QString helper,Validator validator,QObject* parent)
 :QObject(parent),socketPath_(std::move(socket)),epoch_(std::move(epoch)),helperPath_(std::move(helper)),validate_(std::move(validator)) {
 startup_.setSingleShot(true);startup_.setInterval(10000);
 stopDeadline_.setSingleShot(true);stopDeadline_.setInterval(2000);
 closeDeadline_.setSingleShot(true);closeDeadline_.setInterval(10000);
 connect(&closeDeadline_,&QTimer::timeout,this,[this]{fail(unavailable("ui_close_timeout"),"close");});
 activationDeadline_.setSingleShot(true);activationDeadline_.setInterval(12000);
 connect(&startup_,&QTimer::timeout,this,[this]{fail(unavailable("ui_start_timeout"),"startup");});
 connect(&activationDeadline_,&QTimer::timeout,this,[this]{fail(unavailable("ui_show_timeout"),"activation");});
 connect(&stopDeadline_,&QTimer::timeout,this,[this]{if(process_)killHelper("ui_stop_timeout",{{"deadline","stop"},{"deadlineMs",stopDeadline_.interval()}});else completeShutdown();});
 commandDeadline_.setSingleShot(true);commandDeadline_.setInterval(10000);
 connect(&commandDeadline_,&QTimer::timeout,this,[this]{failCommands({"ui_command_timeout","The window did not answer in time. Check it before retrying.",{{"outcomeUnknown",true}}});});
 // A helper that exits cannot answer the commands it was sent.
 connect(this,&UiProcessController::helperPidChanged,this,[this](qint64 pid){if(!pid)failCommands({"ui_view_closed","The window closed before answering.",{{"outcomeUnknown",true}}});});
}
UiProcessController::~UiProcessController(){
 restoreShortcuts();
 if(control_)control_->stop();
 if(process_){disconnect(process_,nullptr,this,nullptr);process_->kill();process_->waitForFinished(1000);}
}
void UiProcessController::activate(const QString& view,Reply reply){
 if(stopping_){if(reply)reply(unavailable("ui_stopping"));return;}
 // Internal tray recovery carries the specific error to its settings control.
 if(!view.startsWith("historyCancelAll:")&&view!="historyFailed"&&!view.startsWith("actionRecovery:")&&!QStringList{"settings","updates","history","activity","destinations","about","editFile","captureSettings"}.contains(view)){
  if(reply)reply(unavailable("ui_view"));
  return;
 }
 int queued=0;for(const auto& callbacks:pending_)queued+=callbacks.size();
 for(const auto& activation:activations_)queued+=activation.replies.size();
 if(queued>=64){if(reply)reply(unavailable("ui_busy"));return;}
 // Keep one activation per view while its acknowledgement is outstanding.
 if(phase_==Phase::Ready)for(auto it=activations_.begin();it!=activations_.end();++it)
  if(it->view==view){it->replies.append(std::move(reply));return;}
 pending_[view].append(std::move(reply));
 if(!activationDeadline_.isActive())activationDeadline_.start();
 if(!process_&&!finishing_)launch();
 else if(phase_==Phase::Ready)flushActivations();
}
void UiProcessController::command(const QString& name,const QJsonObject& params,CommandReply reply){
 // Commands act on views the user already opened; they never start the helper.
 if(stopping_||phase_!=Phase::Ready||!control_){reply({"ui_view_closed","Open Settings before driving it.",{}},{});return;}
 if(commands_.size()>=8){reply(unavailable("ui_busy"),{});return;}
 QPointer<UiProcessController> guard(this);
 const auto sequence=control_->send("command",{{"version",1},{"command",name},{"params",params}});
 if(!guard)return;
 if(sequence.isEmpty()){reply(unavailable(),{});return;}
 commands_.insert(sequence,std::move(reply));commandDeadline_.start();
}
void UiProcessController::failCommands(const Error& error){
 commandDeadline_.stop();auto pending=std::exchange(commands_,{});
 for(auto& reply:pending)if(reply)reply(error,{});
}
void UiProcessController::launch(){
 if(process_||stopping_||finishing_||pending_.isEmpty())return;
 phase_=Phase::Starting;const auto generation=++processGeneration_;
 auto* child=new QProcess(this);process_=child;
 child->setProcessChannelMode(QProcess::SeparateChannels);
 child->setReadChannel(QProcess::StandardOutput);
 control_=new UiControl(*child,child);
 connect(control_,&UiControl::frame,this,&UiProcessController::receive);
 connect(control_,&UiControl::failed,this,[this](Error error){fail(std::move(error));});
 connect(child,&QProcess::readyReadStandardError,this,[this,child,generation]{
  // Only toolkit diagnostics use stderr; the protocol and any credential input
  // travel on the private pipes. The log redacts and bounds what arrives.
  const auto bytes=child->readAllStandardError();
  if(process_==child&&processGeneration_==generation)output_.append(bytes);
 });
 connect(child,&QProcess::started,this,[this,child,generation]{
  if(process_!=child||processGeneration_!=generation)return;
  output_.started(child->processId(),{{"generation",qint64(generation)}});
  QPointer<UiProcessController> guard(this);
  emit helperPidChanged(child->processId());
  if(!guard||process_!=child||stopping_)return;
  control_->send("bootstrap",{{"socketPath",socketPath_},{"residentEpoch",epoch_},{"build",build()},{"state",state_}});
 });
 connect(child,&QProcess::errorOccurred,this,[this,child,generation](QProcess::ProcessError error){
  if(process_!=child||processGeneration_!=generation)return;
  if(error==QProcess::FailedToStart){
   output_.failedToStart(child->errorString());
   QPointer<UiProcessController> guard(this);fail(unavailable("ui_start_failed"));
   if(!guard)return;
   finished(child,generation,-1,QProcess::CrashExit);
  }
 });
 connect(child,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
  [this,child,generation](int code,QProcess::ExitStatus status){finished(child,generation,code,status);});
 startup_.start();child->start(helperPath_,{});
}
void UiProcessController::flushActivations(){
 if(phase_!=Phase::Ready||!control_)return;
 auto pending=std::exchange(pending_,{});
 for(auto it=pending.begin();it!=pending.end();++it){
  QPointer<UiProcessController> guard(this);
  const auto sequence=control_->send("activate",{{"view",it.key()}});
  if(!guard||sequence.isEmpty()||phase_!=Phase::Ready){
   // These batches have not entered activations_ yet. Finish them from local
   // storage even if a failure callback destroys the supervisor.
   for(;it!=pending.end();++it)for(auto& reply:it.value())if(reply)reply(unavailable());
   return;
  }
  activations_.insert(sequence,{it.key(),std::move(it.value())});
 }
}
void UiProcessController::invalidate(){++invalidationGeneration_;sendInvalidation();}
void UiProcessController::sendInvalidation(){
 if(phase_!=Phase::Ready||!control_||!invalidationSequence_.isEmpty()||sentInvalidation_==invalidationGeneration_)return;
 sentInvalidation_=invalidationGeneration_;
 invalidationSequence_=control_->send("invalidated",{});
}
Error UiProcessController::storeState(const QJsonObject& state){
 const auto bytes=QJsonDocument(state).toJson(QJsonDocument::Compact).size();
 if(bytes>8*1024*1024)return {"ui_state_limit","Close some windows or reduce the draft before closing this window.",{}};
 if(auto error=validate_(state);error)return error;
 state_=state;stateBytes_=bytes;return {};
}
void UiProcessController::receive(const QString& type,const QString& sequence,const QJsonObject& payload){
 if(stopping_)return;
 if(type=="shortcutRecording"){
  if(phase_!=Phase::Ready||payload.size()!=1||!payload["active"].isBool()||recordingRequests_.size()>=8){fail(unavailable("ui_protocol"));return;}
  recordingRequests_.append({sequence,payload["active"].toBool()});updateShortcutRecording();return;
 }
 if(type=="incompatible"){fail(unavailable("ui_incompatible"));return;}
 if(type=="ready"){
  if(phase_!=Phase::Starting||payload["build"].toObject()!=build()||payload["residentEpoch"]!=epoch_){fail(unavailable("ui_incompatible"));return;}
  startup_.stop();phase_=Phase::Ready;QPointer<UiProcessController> guard(this);flushActivations();if(guard)sendInvalidation();return;
 }
 if(type=="shown"||type=="activationFailed"){
  if(phase_==Phase::Closing&&closeConfirmed_)return;
  const auto replyTo=payload["replyTo"].toString();
  auto it=activations_.find(replyTo);
  if((phase_!=Phase::Ready&&phase_!=Phase::Closing)||it==activations_.end()||payload["view"]!=it->view){fail(unavailable("ui_protocol"));return;}
  auto replies=std::move(it->replies);activations_.erase(it);
  if(phase_==Phase::Closing&&type=="activationFailed"){
   pending_[payload["view"].toString()].append(std::move(replies));return;
  }
  if(activations_.isEmpty()&&pending_.isEmpty())activationDeadline_.stop();
  const auto error=type=="shown"?Error{}:unavailable();
  for(auto& reply:replies)if(reply)reply(error);
  return;
 }
 if(type=="commandResult"){
  auto it=commands_.find(payload["replyTo"].toString());
  if((phase_!=Phase::Ready&&phase_!=Phase::Closing)||it==commands_.end()||payload.size()!=3||!payload["error"].isObject()||!payload["result"].isObject()){fail(unavailable("ui_protocol"));return;}
  auto reply=std::move(it.value());commands_.erase(it);if(commands_.isEmpty())commandDeadline_.stop();
  const auto value=payload["error"].toObject();const Error error{value["code"].toString(),value["message"].toString(),value["details"].toObject()};
  reply(error,error?QJsonObject{}:payload["result"].toObject());return;
 }
 if(type=="observed"){
  if((phase_!=Phase::Ready&&phase_!=Phase::Closing)||invalidationSequence_.isEmpty()||payload["replyTo"]!=invalidationSequence_){fail(unavailable("ui_protocol"));return;}
  invalidationSequence_.clear();sendInvalidation();return;
 }
 if(type=="closeCancel"||type=="closeConfirmed"){
  if((phase_!=Phase::Closing&&!(type=="closeCancel"&&phase_==Phase::Ready))||closeConfirmed_||closeSequence_.isEmpty()||payload["replyTo"]!=closeSequence_){fail(unavailable("ui_protocol"));return;}
  closeDeadline_.stop();
  if(type=="closeCancel"){
   phase_=Phase::Ready;closeSequence_.clear();
   control_->send("closeCancelled",{{"replyTo",sequence}});
   flushActivations();sendInvalidation();
  }else{
   closeConfirmed_=true;
   for(auto& activation:activations_)pending_[activation.view].append(std::move(activation.replies));
   activations_.clear();stopDeadline_.start();
  }
  return;
 }
 if(type=="checkpoint"||type=="closeRequested"){
  if(phase_!=Phase::Ready){fail(unavailable("ui_protocol"));return;}
  if(type=="closeRequested"){closeSequence_=sequence;closeConfirmed_=false;}
  auto error=payload["state"].isObject()?storeState(payload["state"].toObject()):unavailable("ui_state_invalid");
  if(error){control_->send(type=="checkpoint"?"checkpointRejected":"closeRejected",{{"replyTo",sequence},{"error",error.toJson()}});return;}
  if(type=="closeRequested"){
   phase_=Phase::Closing;
   closeSequence_=sequence;closeConfirmed_=false;
   control_->send("closeAccepted",{{"replyTo",sequence}});
   // The helper remains immutable until confirmation or acknowledged cancellation.
   // Only confirmed closure permits the ordinary two-second forced-exit fallback.
   closeDeadline_.start();
  }else control_->send("checkpointAccepted",{{"replyTo",sequence}});
  return;
 }
 fail(unavailable("ui_protocol"));
}
void UiProcessController::updateShortcutRecording(){
 if(recordingRequestActive_||recordingRequests_.isEmpty()||!control_)return;
 recordingRequestActive_=true;const auto request=recordingRequests_.first();const auto generation=recordingGeneration_;
 // Retain ownership even while native suspension is pending so helper failure
 // always cancels that attempt and restores the saved bindings.
 if(request.active)shortcutRecording_=true;
 QPointer<UiProcessController> guard(this);
 auto complete=[guard,generation,request](Error error){
  if(!guard||guard->recordingGeneration_!=generation)return;
  guard->recordingRequestActive_=false;guard->recordingRequests_.removeFirst();
  if(!request.active||error)guard->shortcutRecording_=false;
  if(guard->control_)guard->control_->send("shortcutRecordingChanged",{{"replyTo",request.sequence},{"active",!error&&request.active},{"error",error.toJson()}});
  if(guard)guard->updateShortcutRecording();
 };
 if(!shortcutRecordingHandler_){complete({"shortcut_recording_unavailable","Global shortcut recording is unavailable in this session.",{}});return;}
 shortcutRecordingHandler_(request.active,[guard,generation,request,complete](Error error){
  if(!guard||guard->recordingGeneration_!=generation)return;
  if(error&&request.active){
   // A partial native failure may have released some grabs. Restore all saved
   // bindings before reporting rejection or processing a later request.
   guard->shortcutRecordingHandler_(false,[complete,error](Error restoreError)mutable{if(restoreError)error.details["restoreError"]=restoreError.toJson();complete(error);});
  }else complete(error);
 });
}
void UiProcessController::restoreShortcuts(){
 ++recordingGeneration_;recordingRequests_.clear();recordingRequestActive_=false;
 if(!std::exchange(shortcutRecording_,false)||!shortcutRecordingHandler_)return;
 QPointer<UiProcessController> guard(this);
 shortcutRecordingHandler_(false,[guard](Error error){
  if(guard&&error&&error.code!="shortcut_recording_interrupted")QMetaObject::invokeMethod(guard,[guard,error]{if(guard&&!guard->shortcutRecording_)emit guard->failure(error);},Qt::QueuedConnection);
 });
}
void UiProcessController::failActivations(Error error){
 auto pending=std::exchange(pending_,{});auto active=std::exchange(activations_,{});
 activationDeadline_.stop();
 for(auto& callbacks:pending)for(auto& reply:callbacks)if(reply)reply(error);
 for(auto& activation:active)for(auto& reply:activation.replies)if(reply)reply(error);
}
void UiProcessController::fail(Error error,const QString& deadline){
 if(phase_==Phase::Failed||stopping_)return;
 const QString failedFrom=QString::fromLatin1(phaseName());
 const int deadlineMs=deadline=="startup"?startup_.interval():deadline=="activation"?activationDeadline_.interval():deadline=="close"?closeDeadline_.interval():0;
 restoreShortcuts();
 phase_=Phase::Failed;startup_.stop();activationDeadline_.stop();closeDeadline_.stop();
 if(control_)control_->stop();
 if(process_&&process_->state()!=QProcess::NotRunning) {
  QJsonObject context{{"code",error.code},{"phase",failedFrom}};
  if(!deadline.isEmpty()){context["deadline"]=deadline;context["deadlineMs"]=deadlineMs;}
  killHelper(error.code,context);
 }
 QPointer<UiProcessController> guard(this);failActivations(error);
 if(!guard)return;
 failCommands(error);
 if(guard)emit failure(error);
}
void UiProcessController::finished(QProcess* child,quint64 generation,int code,QProcess::ExitStatus exitStatus){
 if(process_!=child||processGeneration_!=generation)return;
 restoreShortcuts();
 lastExit_={{"generation",qint64(generation)},{"code",code},{"status",exitStatus==QProcess::NormalExit?"normal":"crash"}};
 const bool expected=(phase_==Phase::Closing&&closeConfirmed_&&code==0&&exitStatus==QProcess::NormalExit)||phase_==Phase::Failed||stopping_;
 if(output_.pid()>0){output_.append(child->readAllStandardError());output_.finished(code,exitStatus,expected);}
 startup_.stop();stopDeadline_.stop();closeDeadline_.stop();
 finishing_=true;
 QPointer<UiProcessController> guard(this);
 // Fail the old requests while the old process pointer still prevents launch.
 // Replies and PID observers may explicitly request a replacement immediately.
 if(!expected){fail(unavailable("ui_crashed"));if(!guard)return;}
 else if(phase_==Phase::Closing){
  for(auto& activation:activations_)pending_[activation.view].append(std::move(activation.replies));
  activations_.clear();
 }
 if(control_)control_->stop();
 control_=nullptr;process_=nullptr;
 child->disconnect(this);child->deleteLater();
 invalidationSequence_.clear();closeSequence_.clear();closeConfirmed_=false;sentInvalidation_=invalidationGeneration_;
 emit helperPidChanged(0);
 if(!guard)return;
 finishing_=false;
 if(pending_.isEmpty()&&activations_.isEmpty())activationDeadline_.stop();
 if(stopping_){completeShutdown();return;}
 phase_=Phase::Stopped;
 // Only an explicit request made while the previous helper was exiting may
 // start another process. A spontaneous crash never starts a restart loop.
 if(!pending_.isEmpty())launch();
}
void UiProcessController::shutdown(std::function<void()> done){
 if(stopping_){if(done){auto previous=std::move(stopped_);stopped_=[previous=std::move(previous),done=std::move(done)]{if(previous)previous();done();};}return;}
 restoreShortcuts();
 stopping_=true;stopped_=std::move(done);phase_=Phase::Stopping;startup_.stop();closeDeadline_.stop();
 QPointer<UiProcessController> guard(this);failActivations(unavailable("ui_stopping"));
 if(!guard)return;
 failCommands(unavailable("ui_stopping"));
 if(!guard)return;
 if(!process_){completeShutdown();return;}
 if(process_->state()==QProcess::Running&&control_)control_->send("shutdown");
 else killHelper("ui_stopping");
 stopDeadline_.start();
}
void UiProcessController::completeShutdown(){
 phase_=Phase::Stopped;auto callback=std::exchange(stopped_,{});if(callback)callback();
}
const char* UiProcessController::phaseName()const{
 switch(phase_){case Phase::Stopped:break;case Phase::Starting:return "starting";case Phase::Ready:return "ready";case Phase::Closing:return "closing";case Phase::Stopping:return "stopping";case Phase::Failed:return "failed";}
 return "stopped";
}
void UiProcessController::killHelper(const QString& reason,QJsonObject context){
 // Logged before the kill (#250): which views were waiting, so a deadline kill
 // can be matched to what the user asked for.
 if(!context.contains("phase"))context["phase"]=QString::fromLatin1(phaseName());
 QJsonArray pending,activations;
 for(auto it=pending_.begin();it!=pending_.end();++it)pending.append(it.key());
 for(const auto& activation:activations_)activations.append(activation.view);
 context["pendingViews"]=pending;context["activations"]=activations;
 if(!commands_.isEmpty())context["commands"]=int(commands_.size());
 if(!closeSequence_.isEmpty())context["closeConfirmed"]=closeConfirmed_;
 output_.killing(reason,context);
 process_->kill();
}
QJsonObject UiProcessController::status()const{
 return {{"state",phaseName()},{"pid",process_?process_->processId():0},{"checkpointBytes",stateBytes_},{"lastExit",lastExit_}};
}
QJsonObject UiProcessController::viewState()const{
 // Only the derived capture-routing and appearance presentations leave the
 // last accepted checkpoint; drafts and destination editors stay with the helper.
 QJsonObject result{{"state",status()["state"]}};
 const auto settings=state_["settings"].toObject();
 if(settings.contains("captureRouting"))result["settings"]=QJsonObject{{"section",settings["section"]},{"captureRouting",settings["captureRouting"]}};
 // The appearance in use (#60): setting, desktop preference, scheme and tokens.
 if(state_.contains("appearance"))result["appearance"]=state_["appearance"];
 return result;
}
}
