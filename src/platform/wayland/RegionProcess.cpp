#include "platform/ProcessStat.h"
#include "RegionProcess.h"
#include "platform/SessionLock.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>
#include <QFile>
#include <utility>
#include <unistd.h>
#include <fcntl.h>

namespace putgd {
namespace {
QJsonObject build() { return QJsonDocument::fromJson(buildManifestJson()).object(); }
Error unavailable(const QString& code) { return {code,"The region helper could not complete selection",{}}; }

}
struct WaylandRegionProcess::Prepared {
 bool settled=false; QRectF selection; Error error;
 std::function<void(QRectF,Error)> reply; std::function<void(Error)> terminal; std::function<void()> released;
};
WaylandRegionProcess::Preparation::~Preparation() {
 if(consumed_.load() || !owner_) return;
 // A prepared continuation can die on a journal worker. Marshal the cleanup.
 QPointer<WaylandRegionProcess> owner=owner_; auto state=state_;
 QMetaObject::invokeMethod(owner,[owner,state]{ if(owner) owner->abandon(state); },Qt::QueuedConnection);
}
WaylandRegionProcess::WaylandRegionProcess(QObject* parent):WaylandRegionProcess(QString{},parent) {}
WaylandRegionProcess::WaylandRegionProcess(QString helperPath,QObject* parent)
 :QObject(parent),helperPath_(std::move(helperPath)),deadline_(this),stopDeadline_(this) {
 if(helperPath_.isEmpty()) helperPath_=QCoreApplication::applicationDirPath()+"/putgd-region";
 deadline_.setSingleShot(true); stopDeadline_.setSingleShot(true); moveFlush_.setSingleShot(true);
 connect(&moveFlush_,&QTimer::timeout,this,[this]{if(pendingMove_){flushMove();if(busy())moveFlush_.start(33);}});
 connect(&deadline_,&QTimer::timeout,this,[this]{fail(unavailable(ready_?"capture_timeout":"capture_start_timeout"));});
 connect(&stopDeadline_,&QTimer::timeout,this,[this]{if(process_) process_->kill();});
}
WaylandRegionProcess::~WaylandRegionProcess() { shutdown(); }
void WaylandRegionProcess::shutdown() {
 if(closing_) return;
 closing_=true; reply_={}; deadline_.stop(); stopDeadline_.stop();
 if(control_) control_->stop();
 if(process_) {
  auto* child=process_; child->disconnect(this); child->kill(); child->waitForFinished(1000);
  process_=nullptr; control_=nullptr; child->deleteLater();
 }
 closeFrozen(); frames_.clear(); result_={};
}
QJsonObject WaylandRegionProcess::diagnostics() const {
 return {{"active",busy()},{"processId",processId()},{"lastExit",lastExit_},{"cleanupRequired",cleanupRequired_},{"build",build()}};
}
void WaylandRegionProcess::closeFrozen() { if(frozenFd_>=0) ::close(frozenFd_); frozenFd_=-1; }
void WaylandRegionProcess::start(const QList<CaptureFrame>& frames,std::function<void(QRectF,Error)> callback,QString outcome,const QList<CaptureFrame>& pixels,QJsonObject appearance) {
 if(closing_) { callback({},unavailable("cancelled")); return; }
 if(busy()) { callback({},unavailable("busy_selecting")); return; }
 if(cleanupRequired_) { callback({},{"capture_cleanup_pending","Restart put.gd before capturing again: the region helper did not confirm surface cleanup",{}}); return; }
 Error error; const auto layout=waylandRegionLayout(frames,error);
 if(error) { callback({},std::move(error)); return; }
 WaylandFrozenFrames frozen;
 if(!pixels.isEmpty()) { frozen=waylandFrozenFrames(frames,pixels,error); if(error) { callback({},std::move(error)); return; } }
 closeFrozen(); frozenFd_=frozen.fd;
 frames_=frames; reply_=std::move(callback); result_={}; failure_={}; if(!pointerArmed_) pendingPointer_.clear(); pointerArmed_=false; pendingMove_.reset(); moveFlush_.stop();
 ready_=begun_=resultReceived_=accepted_=cancelling_=terminal_=teardownCertified_=false;
 nonce_=QUuid::createUuid().toString(QUuid::WithoutBraces); ++generation_;
 launchPid_=0; launchStartTicks_.clear();
 auto* child=new QProcess(this); process_=child;
 child->setProcessChannelMode(QProcess::SeparateChannels); child->setReadChannel(QProcess::StandardOutput);
 control_=new CaptureControl(*child,child); const auto epoch=generation_;
 // Only this helper inherits the memfd, at its own number; every other child
 // the resident starts still sees it close on exec.
 if(frozenFd_>=0) child->setChildProcessModifier([fd=frozenFd_]{ ::fcntl(fd,F_SETFD,0); });
 connect(child,&QProcess::readyReadStandardError,this,[this,child,epoch]{
  const auto bytes=child->readAllStandardError();
  if(process_==child && generation_==epoch) output_.append(bytes);
 });
 connect(control_,&CaptureControl::frame,this,[this,child,epoch](const QString& type,const QJsonObject& payload){
  if(process_==child && generation_==epoch) receive(type,payload);
 });
 connect(control_,&CaptureControl::failed,this,[this,child,epoch](Error error){
  if(process_==child && generation_==epoch) fail(std::move(error));
 });
 // A live selection counts down in the helper, after the area is chosen (#1363).
 const int delay=pixels.isEmpty()?std::exchange(delaySeconds_,0):0; delaySeconds_=0; delayMs_=delay*1000;
 connect(child,&QProcess::started,this,[this,child,epoch,layout,outcome,frozen,appearance,delay]{
  if(process_!=child || generation_!=epoch) return;
  launchPid_=child->processId(); launchStartTicks_=processStartTicks(launchPid_);
  output_.started(launchPid_,{{"generation",QString::number(generation_)}});
  if(launchStartTicks_.isEmpty()) { fail(unavailable("capture_start_failed")); return; }
  QJsonObject bootstrap{{"appearance",appearance},{"build",build()},{"nonce",nonce_},{"generation",QString::number(generation_)},{"layout",layout},{"outcome",outcome}};
  if(frozen.fd>=0) bootstrap["frozen"]=QJsonObject{{"fd",frozen.fd},{"frames",frozen.frames}};
  if(deferKeyboard_) bootstrap["deferKeyboard"]=true;
  if(delay>0) bootstrap["delay"]=delay;
  // The child holds its own copy now.
  closeFrozen();
  if(!control_->send("bootstrap",bootstrap)) fail(unavailable("capture_protocol"));
 });
 connect(child,&QProcess::errorOccurred,this,[this,child,epoch](QProcess::ProcessError error){
  if(process_!=child || generation_!=epoch) return;
  if(error==QProcess::FailedToStart) { output_.failedToStart(child->errorString()); failure_=unavailable("capture_start_failed"); finish(-1,QProcess::CrashExit); }
 });
 connect(child,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,child,epoch](int code,QProcess::ExitStatus status){
  if(process_==child && generation_==epoch) finish(code,status);
 });
 deadline_.start(10000); child->start(helperPath_,{});
}
WaylandRegionProcess::Ticket WaylandRegionProcess::prepare(const QList<CaptureFrame>& frames,QString outcome,QJsonObject appearance,std::function<void(Error)> terminal,std::function<void()> released) {
 auto ticket=std::make_shared<Preparation>(); ticket->owner_=this;
 auto state=std::make_shared<Prepared>(); ticket->state_=state;
 state->terminal=std::move(terminal); state->released=std::move(released);
 start(frames,[state](QRectF selection,Error error){
  state->settled=true; state->selection=selection; state->error=error;
  auto terminal=std::move(state->terminal); auto released=std::move(state->released); auto reply=std::move(state->reply);
  state->terminal={}; state->released={}; state->reply={};
  // begin() owns delivery once it has run; before that only metadata is reported.
  if(error && !reply && terminal) terminal(error);
  if(released) released();
  if(reply) reply(selection,error);
 },std::move(outcome),{},std::move(appearance));
 return ticket;
}
void WaylandRegionProcess::begin(const Ticket& ticket,std::function<void(QRectF,Error)> callback) {
 if(!ticket || ticket->owner_!=this || ticket->consumed_.exchange(true)) { callback({},unavailable("capture_protocol")); return; }
 const auto state=ticket->state_;
 if(state->settled) { callback(state->selection,state->error); return; }
 state->reply=std::move(callback);
}
void WaylandRegionProcess::relayPointer(const QString& kind,QPointF logical) {
 if(busy() ? (terminal_ || cancelling_) : !pointerArmed_) return;
 if(busy() && begun_) {
  if(kind=="move") { pendingMove_=logical; if(!moveFlush_.isActive()) { flushMove(); moveFlush_.start(33); } return; }
  flushMove(); moveFlush_.stop();
  if(busy() && !terminal_) sendPointer(kind,logical);
  return;
 }
 if(kind=="move" && !pendingPointer_.isEmpty() && pendingPointer_.last().first=="move") pendingPointer_.last().second=logical;
 else pendingPointer_.append({kind,logical});
}
bool WaylandRegionProcess::grantKeyboard(int qtKey,int qtModifiers) {
 if(!busy() || terminal_ || cancelling_ || !begun_ || !deferKeyboard_ || !control_) return false;
 if(!control_->send("takeover",{{"nonce",nonce_},{"generation",QString::number(generation_)},{"key",qtKey},{"modifiers",qtModifiers}})) { fail(unavailable("capture_protocol")); return false; }
 return true;
}
void WaylandRegionProcess::flushMove() {
 if(!pendingMove_) return;
 const auto point=*pendingMove_; pendingMove_.reset();
 sendPointer("move",point);
}
void WaylandRegionProcess::sendPointer(const QString& kind,QPointF logical) {
 if(!control_) return;
 if(!control_->send("pointer",{{"nonce",nonce_},{"generation",QString::number(generation_)},{"kind",kind},{"x",logical.x()},{"y",logical.y()}})) fail(unavailable("capture_protocol"));
}
void WaylandRegionProcess::discard(const Ticket& ticket) {
 if(ticket && ticket->owner_==this && !ticket->consumed_.exchange(true)) abandon(ticket->state_);
}
void WaylandRegionProcess::abandon(const std::shared_ptr<Prepared>& state) {
 // Only an unsettled preparation can own the one running helper.
 if(!closing_ && !state->settled) cancel();
}
void WaylandRegionProcess::receive(const QString& type,const QJsonObject& payload) {
 if(!busy() || terminal_) return;
 if(payload["nonce"]!=nonce_ || payload["generation"]!=QString::number(generation_)) {
  fail(unavailable("capture_protocol")); return;
 }
 if(type=="ready") {
  if(ready_ || payload.size()!=3 || payload["build"].toObject()!=build()) {
   fail(unavailable("capture_incompatible")); return;
  }
  ready_=true;
  if(cancelling_ || failure_) {
   if(control_) control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
   return;
  }
  begun_=true; QPointer<WaylandRegionProcess> guard(this);
  const bool sent=control_->send("begin",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
  if(!guard) return;
  if(!sent) { fail(unavailable("capture_protocol")); return; }
  deadline_.start(120000+delayMs_);
  const auto pending=std::exchange(pendingPointer_,{});
  for(const auto& [kind,point]:pending){ sendPointer(kind,point); if(!guard||!process_||terminal_) return; }
  return;
 }
 if(type=="cancelled" || type=="error") {
  const auto code=payload["code"].toString();
  const QStringList allowed{"cancelled","display_changed","region_unavailable","capture_geometry","capture_timeout","session_locked"};
  if(!ready_ || resultReceived_ || payload.size()!=3 || !allowed.contains(code) ||
     (type=="cancelled")!=(code=="cancelled")) {
   fail(unavailable("capture_protocol")); return;
  }
  terminal_=teardownCertified_=true;
  // The countdown's last frame never reached the screen (#1363): nothing is captured.
  if(!failure_) failure_=code=="session_locked"?countdownNotShown():unavailable(code);
  deadline_.stop(); if(!stopDeadline_.isActive()) stopDeadline_.start(2000); return;
 }
 if(type=="result") {
  if(failure_ || !ready_ || !begun_ || resultReceived_ || payload.size()!=3 || !payload["rectangle"].isObject()) {
   fail(unavailable("capture_protocol")); return;
  }
  Error error; const auto selection=waylandRegionRectangle(payload["rectangle"].toObject(),frames_,error);
  if(error) { fail(std::move(error)); return; }
  result_=selection; resultReceived_=true; teardownCertified_=true;
  QPointer<WaylandRegionProcess> guard(this);
  accepted_=control_->send("accept",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
  if(!guard) return;
  if(!accepted_) { fail(unavailable("capture_protocol")); return; }
  deadline_.stop(); if(!stopDeadline_.isActive()) stopDeadline_.start(2000); return;
 }
 fail(unavailable("capture_protocol"));
}
void WaylandRegionProcess::cancel(Error error) {
 if(!busy() || cancelling_) return;
 cancelling_=true; if(!failure_) failure_=std::move(error);
 deadline_.stop();
 if(ready_ && control_) { control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}}); }
 if(!stopDeadline_.isActive()) stopDeadline_.start(2000);
}
void WaylandRegionProcess::fail(Error error) {
 if(!busy()) return;
 if(!failure_) failure_=std::move(error);
 cancelling_=true; deadline_.stop();
 if(ready_ && control_) { control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}}); }
 if(!stopDeadline_.isActive()) stopDeadline_.start(2000);
}
void WaylandRegionProcess::finish(int code,QProcess::ExitStatus status) {
 if(!process_) return;
 deadline_.stop(); stopDeadline_.stop();
 moveFlush_.stop(); pendingMove_.reset(); pendingPointer_.clear(); pointerArmed_=false;
 emit ended();
 if(begun_ && launchPid_>0 && !teardownCertified_) cleanupRequired_=true;
 lastExit_={{"generation",QString::number(generation_)},{"code",code},
            {"status",status==QProcess::NormalExit?"normal":"crash"},
            {"processId",launchPid_},{"startTicks",launchStartTicks_},{"teardownCertified",teardownCertified_}};
 if(output_.pid()>0) { output_.append(process_->readAllStandardError()); output_.finished(code,status,cancelling_); }
 if(!failure_ && (status!=QProcess::NormalExit || code!=0 || !resultReceived_ || !accepted_))
  failure_=unavailable(status==QProcess::CrashExit?"capture_crashed":"capture_protocol");
 if(control_) control_->stop();
 auto* child=process_; child->disconnect(this); child->deleteLater();
 process_=nullptr; control_=nullptr; closeFrozen();
 frames_.clear(); auto cb=std::move(reply_); reply_={};
 const auto error=failure_; const auto selection=error?QRectF{}:result_; result_={};
 if(cb && !closing_) cb(selection,error);
}
}
