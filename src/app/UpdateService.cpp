#include "UpdateService.h"
#include "AppImageInstall.h"
#include "core/HostEnvironment.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <climits>
#include <utility>
namespace putgd {
namespace {
constexpr qint64 maximumOutput=64*1024;
QJsonValue isoTime(const QDateTime& value) {return value.isValid()?QJsonValue(value.toUTC().toString(Qt::ISODate)):QJsonValue();}
QDateTime parseTime(const QJsonValue& value) {return value.isString()?QDateTime::fromString(value.toString(),Qt::ISODate):QDateTime{};}
}
UpdateService::UpdateService(SettingsService& settings,QString stateRoot,QString program,QObject* parent):QObject(parent),settings_(settings),stateRoot_(stateRoot),statePath_(stateRoot+"/update-check.json"),program_(std::move(program)),
 build_(update::build(QJsonDocument::fromJson(buildManifestJson()).object())),timer_(this),deadline_(this) {
 timer_.setSingleShot(true);connect(&timer_,&QTimer::timeout,this,[this]{run(false);});
 deadline_.setSingleShot(true);deadline_.setInterval(childTimeoutMs);
 connect(&deadline_,&QTimer::timeout,this,[this]{if(process_){logging::warning(logApp(),"Update check took too long and was stopped");process_->kill();}});
 connect(&settings_,&SettingsService::accepted,this,[this]{schedule();emit changed();});
 installDeadline_.setSingleShot(true);installDeadline_.setInterval(installTimeoutMs);
 connect(&installDeadline_,&QTimer::timeout,this,[this]{if(installer_){logging::warning(logApp(),"Update install took too long and was stopped");installer_->kill();}});
 // AppRun's $APPIMAGE: the file an install replaces and a restart starts.
 if(const auto image=qEnvironmentVariable("APPIMAGE");!image.isEmpty()&&QFileInfo(image).isAbsolute())image_=QFileInfo(image).canonicalFilePath();
 restarter_=[](qint64 pid,const QString& image){
  // A host shell, not a program inside this image's mount, which goes away
  // with this process; with the host's environment, not the image's (#326).
  static const QString script=QStringLiteral(
   "i=0; while kill -0 \"$1\" 2>/dev/null; do i=$((i+1)); [ \"$i\" -gt 150 ] && exit 1; sleep 0.2; done; exec \"$2\"");
  return host::startDetached("/bin/sh",{"-c",script,"putgd-restart",QString::number(pid),image});
 };
}
qint64 UpdateService::random(qint64 low,qint64 high) const {return random_?random_(low,high):low+qint64(QRandomGenerator::global()->bounded(quint64(high-low+1)));}
QString UpdateService::channel() const {const auto chosen=settings_.snapshot().updateChannel;return chosen=="stable"||chosen=="nightly"?chosen:build_.channel;}
bool UpdateService::enabled() const {return build_.supported&&settings_.snapshot().checkForUpdates&&!channel().isEmpty();}
void UpdateService::start() {
 startedAt_=now();startupDelayMs_=random(startupMinimumMs,startupMaximumMs);jitter_=random(-jitterMs,jitterMs);
 QFile file(statePath_);
 if(file.open(QIODevice::ReadOnly)&&file.size()<maximumOutput) {
  const auto state=QJsonDocument::fromJson(file.readAll()).object();
  if(state["schemaVersion"]==1) {
   attemptedAt_=parseTime(state["attemptedAt"]);checkedAt_=parseTime(state["checkedAt"]);attemptedChannel_=state["attemptedChannel"].toString();
   result_=state["result"].toObject();error_=state["error"].toObject();
   const auto notified=state["notifiedVersions"].toObject();
   for(const auto& name:{QStringLiteral("stable"),QStringLiteral("nightly")})if(notified[name].isString())notified_[name]=notified[name].toString();
   // Before #344 one version was kept, of whichever channel was checked.
   if(!state.contains("notifiedVersions")&&state["notifiedVersion"].isString()){const auto version=state["notifiedVersion"].toString();notified_[version.contains("-nightly")?"nightly":"stable"]=version;}
   // A clock that went backwards must not postpone the next check for days.
   if(attemptedAt_>now())attemptedAt_={};
   // The saved result compared the feed with the version that ran then. After
   // an update (put.gd's own install or another updater's), compare it with
   // this one, so Settings doesn't offer the version that is running (#293).
   if(result_["latest"].isString()){
    const auto order=update::compareVersions(result_["latest"].toString(),build_.version);
    result_["current"]=build_.version;result_["updateAvailable"]=order&&*order>0;
   }
  }
 }
 // An interrupted install's staging folder goes; once this version runs
 // from the installed path, so does the old image's backup.
 installState_=appimage::settle(stateRoot_,image_,build_.version);
 if(installState_["installed"].toObject()["backupRemoved"].toBool()&&installState_["installed"].toObject()["version"]==build_.version)
  logging::info(logApp(),"Update finished: the new version started, so the old image's backup was removed",{{"version",build_.version}});
 schedule();
}
void UpdateService::schedule() {
 timer_.stop();nextAt_={};
 // Settings can change before start(); start() schedules then.
 if(!startedAt_.isValid()||!enabled())return;
 const auto earliest=startedAt_.addMSecs(startupDelayMs_);
 // A day after the last attempt on this channel, whether it worked or not; an
 // attempt on another channel says nothing about this one, so check soon.
 auto due=attemptedAt_.isValid()&&attemptedChannel_==channel()?attemptedAt_.addMSecs(intervalMs+jitter_):earliest;
 if(due<earliest)due=earliest;
 nextAt_=due;
 timer_.start(int(qBound(qint64(0),now().msecsTo(due),qint64(INT_MAX))));
}
QJsonObject UpdateService::action() const {
 // The check child reads the repository file; the last result carries it.
 if(!build_.supported||result_["channel"]!=channel())return {};
 return result_["action"].toObject();
}
bool UpdateService::notifies(const QJsonObject& action) const {
 // A system updater that follows our repository already tells the user.
 return channel()=="stable"&&action["kind"]!="software-updates";
}
QJsonObject UpdateService::status() const {
 const auto snapshot=settings_.snapshot();
 // Only a result for the current channel is shown.
 const bool current=result_["channel"]==channel()&&!channel().isEmpty();
 const auto offered=action();
 return {{"build",build_.toJson()},{"check",snapshot.checkForUpdates},{"channelSetting",snapshot.updateChannel},{"channel",channel().isEmpty()?QJsonValue():QJsonValue(channel())},
  {"enabled",enabled()},{"supported",build_.supported},{"reason",build_.reason.isEmpty()?QJsonValue():QJsonValue(build_.reason)},
  {"notifies",notifies(offered)},{"action",offered.isEmpty()?QJsonValue():QJsonValue(offered)},{"checking",bool(process_)},{"attemptedAt",isoTime(attemptedAt_)},{"checkedAt",current?isoTime(checkedAt_):QJsonValue()},
  {"nextCheckAt",isoTime(nextAt_)},{"result",current?QJsonValue(result_):QJsonValue()},{"error",error_.isEmpty()?QJsonValue():QJsonValue(error_)},
  {"install",installStatus()}};
}
QString UpdateService::installUnavailable() const {
 if(build_.target!="appimage")return "not_appimage";
 if(!build_.supported)return build_.reason;
 if(image_.isEmpty())return "not_running_from_appimage";
 // Another channel's image is a switch (#344), only to a release the last
 // check of that channel found.
 if(channel()!=build_.channel){
  if(result_["channel"]!=channel())return "not_checked";
  if(!result_["latest"].isString())return "no_release";
 }
 return {};
}
QJsonObject UpdateService::switchOffer() const {
 if(channel()==build_.channel||!installUnavailable().isEmpty())return {};
 const auto version=result_["latest"].toString();const auto order=update::compareVersions(version,build_.version);
 return {{"from",build_.channel},{"to",channel()},{"version",version},{"tag",result_["tag"]},{"older",order&&*order<0}};
}
bool UpdateService::restartRequired() const {
 const auto installed=installState_["installed"].toObject();const auto version=installed["version"].toString();
 return !image_.isEmpty()&&installed["image"].toString()==image_&&!version.isEmpty()&&version!=build_.version;
}
QJsonObject UpdateService::installStatus() const {
 const auto unavailable=installUnavailable();
 auto status=install_;
 if(!status.contains("state"))status["state"]="idle";
 status["available"]=unavailable.isEmpty();
 const auto offer=switchOffer();
 status["kind"]=build_.channel.isEmpty()||channel()==build_.channel?"update":"switch";
 status["switch"]=offer.isEmpty()?QJsonValue():QJsonValue(offer);
 status["reason"]=unavailable.isEmpty()?QJsonValue():QJsonValue(unavailable);
 status["running"]=bool(installer_);
 status["image"]=image_.isEmpty()?QJsonValue():QJsonValue(image_);
 status["restartRequired"]=restartRequired();
 status["installed"]=installState_["installed"].isObject()?installState_["installed"]:QJsonValue();
 status["backup"]=installState_["backup"].toObject()["path"].isString()?installState_["backup"].toObject()["path"]:QJsonValue();
 return status;
}
void UpdateService::install(const QJsonObject& params,JsonReply reply) {
 for(auto it=params.begin();it!=params.end();++it)if(!QStringList{"confirm","method","channel","allowOlder"}.contains(it.key())){reply({"invalid_request","update.install takes confirm, method, channel and allowOlder",{}},{});return;}
 // Nothing installs without an explicit yes.
 if(params["confirm"]!=true){reply({"update_confirm_required","Installing an update needs confirm: true",{}},{});return;}
 const auto method=params["method"].toString("auto");
 if(!QStringList{"auto","zsync","download"}.contains(method)||(params.contains("method")&&!params["method"].isString())){reply({"invalid_request","method is auto, zsync or download",{}},{});return;}
 if((params.contains("channel")&&!params["channel"].isString())||(params.contains("allowOlder")&&!params["allowOlder"].isBool())){reply({"invalid_request","channel is stable or nightly and allowOlder true or false",{}},{});return;}
 if(const auto reason=installUnavailable();!reason.isEmpty()){
  const auto message=reason=="no_release"?"There's no "+channel()+" release yet, so there is nothing to switch to":
   reason=="not_checked"?"Check the "+channel()+" channel first: put.gd only switches to a release it has found":QString("Only an AppImage installs updates itself");
  reply({"update_install_unavailable",message,{{"reason",reason}}},{});return;
 }
 if(installer_){reply({"update_install_busy","An update is already being installed",{}},{});return;}
 if(restartRequired()){reply({"update_restart_required","Restart put.gd to finish the update that is already installed",{{"installed",installState_["installed"].toObject()["version"]}}},{});return;}
 // A switch names where it goes, so a stale confirmation can't land elsewhere,
 // and an older version is installed only when asked for (#344).
 const auto offer=switchOffer();
 if(params.contains("channel")&&params["channel"]!=channel()){
  reply({"update_switch_confirm_required","channel must be the channel put.gd watches, "+channel(),{{"channel",channel()}}},{});return;
 }
 if(!offer.isEmpty()&&!params.contains("channel")){
  reply({"update_switch_confirm_required","This is a switch to "+channel()+" "+offer["version"].toString()+"; confirm it with channel: \""+channel()+"\"",{{"switch",offer}}},{});return;
 }
 if(offer["older"].toBool()&&params["allowOlder"]!=true){
  reply({"update_switch_older",channel()+" "+offer["version"].toString()+" is older than this build ("+build_.version+"); switching to it needs allowOlder: true",{{"switch",offer}}},{});return;
 }
 if(busy_)if(const auto reason=busy_();!reason.isEmpty()){reply({"update_install_busy",reason,{}},{});return;}
 auto* process=new QProcess(this);installer_=process;process->setProcessChannelMode(QProcess::SeparateChannels);
 install_={{"state","running"},{"phase","starting"},{"method",method},{"bytes",0},{"total",0},{"startedAt",isoTime(now())}};
 auto buffer=std::make_shared<QByteArray>();auto last=std::make_shared<QByteArray>();
 connect(process,&QProcess::readyReadStandardOutput,process,[this,process,buffer,last]{
  *buffer+=process->readAllStandardOutput();
  for(qsizetype end;(end=buffer->indexOf('\n'))>=0;) {
   const auto line=buffer->left(end).trimmed();buffer->remove(0,end+1);
   // Progress events, then one result line (keys come sorted, so parse it).
   if(QJsonDocument::fromJson(line).object().contains("schemaVersion"))*last=line;else installLine(line);
  }
  if(buffer->size()>maximumOutput)process->kill();
 });
 connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error){
  if(error!=QProcess::FailedToStart||installer_!=process)return;
  installDeadline_.stop();installer_=nullptr;process->deleteLater();installFinished({},{"update_install_failed","Cannot start the update install",{{"program",program_}}});
 });
 connect(process,&QProcess::finished,this,[this,process,buffer,last](int code,QProcess::ExitStatus status){
  if(installer_!=process)return;
  installDeadline_.stop();*buffer+=process->readAllStandardOutput();
  if(const auto line=buffer->trimmed();QJsonDocument::fromJson(line).object().contains("schemaVersion"))*last=line;
  installer_=nullptr;process->deleteLater();
  installFinished(status==QProcess::NormalExit&&(code==0||code==1)?*last:QByteArray(),{"update_install_failed","The update install stopped without a result",{}});
 });
 QStringList arguments{"--install-update",channel(),"--method",method,"--state",stateRoot_};
 if(!offer.isEmpty())arguments<<"--switch";
 if(offer["older"].toBool())arguments<<"--allow-older";
 if(offer.isEmpty())logging::info(logApp(),"Installing an update",{{"channel",channel()},{"method",method}});
 else logging::info(logApp(),"Switching channels",{{"from",build_.channel},{"to",channel()},{"version",offer["version"]},{"older",offer["older"]},{"method",method}});
 installDeadline_.start();
 process->start(program_,prefix_+arguments,QIODevice::ReadOnly);
 emit changed();
 reply({},status());
}
void UpdateService::installLine(const QByteArray& line) {
 const auto event=QJsonDocument::fromJson(line).object();
 if(event["event"]!="progress")return;
 for(const auto& key:{"phase","method","bytes","total"})install_[key]=event[key];
 emit changed();
}
void UpdateService::installFinished(const QByteArray& last,const Error& failure) {
 const auto reply=QJsonDocument::fromJson(last).object();
 Error error=failure;QJsonObject result;
 if(reply["schemaVersion"]==1&&reply["ok"]==true){error={};result=reply["result"].toObject();}
 else if(reply["schemaVersion"]==1&&reply["ok"]==false){const auto e=reply["error"].toObject();error={e["code"].toString(),e["message"].toString(),e["details"].toObject()};}
 installState_=appimage::readState(stateRoot_);
 install_["finishedAt"]=isoTime(now());
 if(error) {
  install_["state"]="failed";install_["error"]=error.toJson();install_.remove("result");
  logging::warning(logApp(),"Update install failed",{{"code",error.code},{"details",error.details}});
 } else {
  install_["state"]="installed";install_["result"]=result;install_.remove("error");install_["phase"]="done";
  logging::info(logApp(),"Update installed; it starts with the next start of put.gd",{{"version",result["version"]},{"channel",result["channel"]},{"switched",result["switched"]},{"method",result["method"]},{"zsync",result["zsync"]}});
  // The version someone switched to has been seen.
  if(result["switched"].toBool()&&result["channel"].isString()){notified_[result["channel"].toString()]=result["version"].toString();save();}
 }
 emit changed();
 if(!error)emit imageReplaced();
}
void UpdateService::restart(const QJsonObject& params,JsonReply reply) {
 if(params.size()!=1||params["confirm"]!=true){reply({"update_confirm_required","Restarting needs confirm: true",{}},{});return;}
 if(!restartRequired()){reply({"update_restart_unavailable","No installed update is waiting for a restart",{}},{});return;}
 if(installer_){reply({"update_install_busy","An update is being installed",{}},{});return;}
 if(busy_)if(const auto reason=busy_();!reason.isEmpty()){reply({"update_install_busy",reason,{}},{});return;}
 if(!quit_||!restarter_||!restarter_(QCoreApplication::applicationPid(),image_)){reply({"update_restart_failed","Cannot start put.gd again",{}},{});return;}
 const auto version=installState_["installed"].toObject()["version"].toString();
 logging::info(logApp(),"Restarting into the installed update",{{"version",version}});
 reply({},{{"restarting",true},{"version",version}});
 QTimer::singleShot(0,this,[quit=quit_]{quit();});
}
void UpdateService::check(JsonReply reply) {
 if(!build_.supported){reply({"update_check_unavailable","This build does not check for updates",{{"reason",build_.reason}}},{});return;}
 waiting_.append(std::move(reply));
 if(process_){manualRunning_=true;return;}
 run(true);
}
void UpdateService::openSoftwareUpdates(JsonReply reply) {
 if(!build_.supported){reply({"update_check_unavailable","This build does not check for updates",{{"reason",build_.reason}}},{});return;}
 // A short-lived child checks the repository file again and starts the updater.
 auto* process=new QProcess(this);process->setProcessChannelMode(QProcess::SeparateChannels);
 auto output=std::make_shared<QByteArray>();auto done=std::make_shared<JsonReply>(std::move(reply));
 connect(process,&QProcess::readyReadStandardOutput,process,[process,output]{*output+=process->readAllStandardOutput();if(output->size()>maximumOutput)process->kill();});
 connect(process,&QProcess::errorOccurred,this,[process,done](QProcess::ProcessError error){
  if(error!=QProcess::FailedToStart||!*done)return;
  std::exchange(*done,{})({"software_updates_failed","Cannot start the update child",{}},{});process->deleteLater();
 });
 connect(process,&QProcess::finished,this,[process,output,done]{
  *output+=process->readAllStandardOutput();process->deleteLater();if(!*done)return;
  const auto answer=QJsonDocument::fromJson(output->trimmed()).object();const auto e=answer["error"].toObject();
  if(answer["schemaVersion"]==1&&answer["ok"]==true){
   logging::info(logApp(),"Opened the software updater",{{"updater",answer["result"].toObject()["opened"]}});
   std::exchange(*done,{})({},answer["result"].toObject());
  } else std::exchange(*done,{})({e["code"].toString("software_updates_failed"),e["message"].toString("The software updater could not be started"),e["details"].toObject()},{});
 });
 process->start(program_,prefix_+QStringList{"--open-software-updates"},QIODevice::ReadOnly);
}
void UpdateService::run(bool manual) {
 if(process_)return;
 if(!manual&&!enabled()){schedule();return;}
 manualRunning_=manual;
 auto* process=new QProcess(this);process_=process;process->setProcessChannelMode(QProcess::SeparateChannels);
 auto output=std::make_shared<QByteArray>();
 connect(process,&QProcess::readyReadStandardOutput,process,[process,output]{*output+=process->readAllStandardOutput();if(output->size()>maximumOutput)process->kill();});
 connect(process,&QProcess::errorOccurred,this,[this,process,output](QProcess::ProcessError error){
  if(error!=QProcess::FailedToStart||process_!=process)return;
  deadline_.stop();process_=nullptr;process->deleteLater();finished(manualRunning_,{},{"update_check_failed","Cannot start the update check",{{"program",program_}}});
 });
 connect(process,&QProcess::finished,this,[this,process,output](int code,QProcess::ExitStatus status){
  if(process_!=process)return;
  deadline_.stop();*output+=process->readAllStandardOutput();process_=nullptr;process->deleteLater();
  // The child exits 1 for a reported failure; anything else without a reply line is a crash or timeout.
  finished(manualRunning_,status==QProcess::NormalExit&&(code==0||code==1)?*output:QByteArray(),{"update_check_failed","The update check stopped without a result",{}});
 });
 logging::info(logApp(),"Checking for updates",{{"channel",channel()},{"manual",manual}});
 deadline_.start();process->start(program_,prefix_+QStringList{"--check-update",channel()},QIODevice::ReadOnly);
 emit changed();
}
void UpdateService::finished(bool manual,const QByteArray& output,const Error& failure) {
 // The first result after the channel was changed shows in Settings, where it
 // was changed: it counts as seen, like a manual check (#344).
 const auto shown=result_["channel"].toString();const bool channelChanged=!shown.isEmpty()&&shown!=channel();
 manualRunning_=false;attemptedAt_=now();attemptedChannel_=channel();jitter_=random(-jitterMs,jitterMs);
 const auto reply=QJsonDocument::fromJson(output.trimmed()).object();
 Error error=failure;QJsonObject result;
 if(reply["schemaVersion"]==1&&reply["ok"]==true&&reply["result"].isObject()&&reply["result"].toObject()["channel"]==channel()){error={};result=reply["result"].toObject();}
 else if(reply["schemaVersion"]==1&&reply["ok"]==false){const auto e=reply["error"].toObject();error={e["code"].toString(),e["message"].toString(),e["details"].toObject()};}
 if(error) {
  error_=error.toJson();
  logging::warning(logApp(),"Update check failed",{{"code",error.code},{"details",error.details}});
 } else {
  checkedAt_=attemptedAt_;result_=result;error_={};
  const auto latest=result["latest"].toString();const bool available=result["updateAvailable"].toBool();
  logging::info(logApp(),"Update check finished",{{"channel",result["channel"]},{"latest",result["latest"]},{"updateAvailable",available}});
  // Stable announces each new version once. A version someone already saw in
  // Settings is not announced again; nightly is never announced. Each channel
  // remembers its own, so switching neither repeats nor hides one.
  const bool seen=manual||channelChanged;
  if(available&&latest!=notified_.value(channel())) {
   if(!seen&&notifies(result["action"].toObject())&&settings_.snapshot().notifications)emit updateAvailable(result);
   if(seen||channel()=="stable")notified_[channel()]=latest;
  }
 }
 save();schedule();emit changed();
 const auto replies=std::exchange(waiting_,{});
 for(const auto& reply:replies){if(error)reply(error,{});else reply({},status());}
}
void UpdateService::save() {
 QJsonObject notified;for(auto it=notified_.cbegin();it!=notified_.cend();++it)notified[it.key()]=it.value();
 // notifiedVersion is what builds before #344 read: the watched channel's.
 const auto bytes=QJsonDocument(QJsonObject{{"schemaVersion",1},{"attemptedAt",isoTime(attemptedAt_)},{"attemptedChannel",attemptedChannel_},{"checkedAt",isoTime(checkedAt_)},{"result",result_},
  {"error",error_.isEmpty()?QJsonValue():QJsonValue(error_)},{"notifiedVersion",notified_.value(channel())},{"notifiedVersions",notified}}).toJson(QJsonDocument::Compact);
 QSaveFile state(statePath_);state.setDirectWriteFallback(false);
 if(!state.open(QIODevice::WriteOnly)||!state.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||state.write(bytes)!=bytes.size()||!state.commit())
  logging::warning(logApp(),"Cannot save the update check state; the next start checks again");
}
}
