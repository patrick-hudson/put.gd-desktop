#include "KdeRegionProcess.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>
#include <QFile>
#include <unistd.h>
#include <fcntl.h>

namespace putgd {
namespace {
QJsonObject build() { return QJsonDocument::fromJson(buildManifestJson()).object(); }
Error unavailable(const QString& code) { return {code,"The region helper could not complete selection",{}}; }
QString processStartTicks(qint64 pid) {
 QFile file(QString("/proc/%1/stat").arg(pid));
 if(pid<=0 || !file.open(QIODevice::ReadOnly)) return {};
 const auto bytes=file.read(4097); if(bytes.size()>4096) return {};
 const auto end=bytes.lastIndexOf(')'); if(end<0) return {};
 const auto fields=bytes.mid(end+2).simplified().split(' '); if(fields.size()<20) return {};
 bool ok=false; const auto ticks=fields[19].toULongLong(&ok);
 return ok && ticks>0?QString::number(ticks):QString{};
}
}
KdeRegionProcess::KdeRegionProcess(QObject* parent):KdeRegionProcess(QString{},parent) {}
KdeRegionProcess::KdeRegionProcess(QString helperPath,QObject* parent)
 :QObject(parent),helperPath_(std::move(helperPath)),deadline_(this),stopDeadline_(this) {
 if(helperPath_.isEmpty()) helperPath_=QCoreApplication::applicationDirPath()+"/putgd-kde-region";
 deadline_.setSingleShot(true); stopDeadline_.setSingleShot(true);
 connect(&deadline_,&QTimer::timeout,this,[this]{fail(unavailable(ready_?"capture_timeout":"capture_start_timeout"));});
 connect(&stopDeadline_,&QTimer::timeout,this,[this]{if(process_) process_->kill();});
}
KdeRegionProcess::~KdeRegionProcess() { shutdown(); }
void KdeRegionProcess::shutdown() {
 if(closing_) return;
 closing_=true; reply_={}; deadline_.stop(); stopDeadline_.stop();
 if(control_) control_->stop();
 if(process_) {
  auto* child=process_; child->disconnect(this); child->kill(); child->waitForFinished(1000);
  process_=nullptr; control_=nullptr; child->deleteLater();
 }
 closeFrozen(); frames_.clear(); result_={};
}
QJsonObject KdeRegionProcess::diagnostics() const {
 return {{"active",busy()},{"processId",processId()},{"lastExit",lastExit_},{"cleanupRequired",cleanupRequired_},{"build",build()}};
}
void KdeRegionProcess::closeFrozen() { if(frozenFd_>=0) ::close(frozenFd_); frozenFd_=-1; }
void KdeRegionProcess::start(const QList<CaptureFrame>& frames,std::function<void(QRectF,Error)> callback,QString outcome,const QList<CaptureFrame>& pixels) {
 if(closing_) { callback({},unavailable("cancelled")); return; }
 if(busy()) { callback({},unavailable("busy_selecting")); return; }
 if(cleanupRequired_) { callback({},{"capture_cleanup_pending","Restart put.gd before capturing again: the region helper did not confirm surface cleanup",{}}); return; }
 Error error; const auto layout=kdeRegionLayout(frames,error);
 if(error) { callback({},std::move(error)); return; }
 KdeFrozenFrames frozen;
 if(!pixels.isEmpty()) { frozen=kdeFrozenFrames(frames,pixels,error); if(error) { callback({},std::move(error)); return; } }
 closeFrozen(); frozenFd_=frozen.fd;
 frames_=frames; reply_=std::move(callback); result_={}; failure_={};
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
 connect(child,&QProcess::started,this,[this,child,epoch,layout,outcome,frozen]{
  if(process_!=child || generation_!=epoch) return;
  launchPid_=child->processId(); launchStartTicks_=processStartTicks(launchPid_);
  output_.started(launchPid_,{{"generation",QString::number(generation_)}});
  if(launchStartTicks_.isEmpty()) { fail(unavailable("capture_start_failed")); return; }
  QJsonObject bootstrap{{"build",build()},{"nonce",nonce_},{"generation",QString::number(generation_)},{"layout",layout},{"outcome",outcome}};
  if(frozen.fd>=0) bootstrap["frozen"]=QJsonObject{{"fd",frozen.fd},{"frames",frozen.frames}};
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
void KdeRegionProcess::receive(const QString& type,const QJsonObject& payload) {
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
  begun_=true; QPointer<KdeRegionProcess> guard(this);
  const bool sent=control_->send("begin",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
  if(!guard) return;
  if(!sent) { fail(unavailable("capture_protocol")); return; }
  deadline_.start(120000); return;
 }
 if(type=="cancelled" || type=="error") {
  const auto code=payload["code"].toString();
  const QStringList allowed{"cancelled","display_changed","region_unavailable","capture_geometry","capture_timeout"};
  if(!ready_ || resultReceived_ || payload.size()!=3 || !allowed.contains(code) ||
     (type=="cancelled")!=(code=="cancelled")) {
   fail(unavailable("capture_protocol")); return;
  }
  terminal_=teardownCertified_=true;
  if(!failure_) failure_=unavailable(code);
  deadline_.stop(); if(!stopDeadline_.isActive()) stopDeadline_.start(2000); return;
 }
 if(type=="result") {
  if(failure_ || !ready_ || !begun_ || resultReceived_ || payload.size()!=3 || !payload["rectangle"].isObject()) {
   fail(unavailable("capture_protocol")); return;
  }
  Error error; const auto selection=kdeRegionRectangle(payload["rectangle"].toObject(),frames_,error);
  if(error) { fail(std::move(error)); return; }
  result_=selection; resultReceived_=true; teardownCertified_=true;
  QPointer<KdeRegionProcess> guard(this);
  accepted_=control_->send("accept",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
  if(!guard) return;
  if(!accepted_) { fail(unavailable("capture_protocol")); return; }
  deadline_.stop(); if(!stopDeadline_.isActive()) stopDeadline_.start(2000); return;
 }
 fail(unavailable("capture_protocol"));
}
void KdeRegionProcess::cancel(Error error) {
 if(!busy() || cancelling_) return;
 cancelling_=true; if(!failure_) failure_=std::move(error);
 deadline_.stop();
 if(ready_ && control_) { control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}}); }
 if(!stopDeadline_.isActive()) stopDeadline_.start(2000);
}
void KdeRegionProcess::fail(Error error) {
 if(!busy()) return;
 if(!failure_) failure_=std::move(error);
 cancelling_=true; deadline_.stop();
 if(ready_ && control_) { control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}}); }
 if(!stopDeadline_.isActive()) stopDeadline_.start(2000);
}
void KdeRegionProcess::finish(int code,QProcess::ExitStatus status) {
 if(!process_) return;
 deadline_.stop(); stopDeadline_.stop();
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
