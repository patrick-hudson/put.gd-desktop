#include "UpdateService.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include <QFile>
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
UpdateService::UpdateService(SettingsService& settings,QString stateRoot,QString program,QObject* parent):QObject(parent),settings_(settings),statePath_(stateRoot+"/update-check.json"),program_(std::move(program)),
 build_(update::build(QJsonDocument::fromJson(buildManifestJson()).object())),timer_(this),deadline_(this) {
 timer_.setSingleShot(true);connect(&timer_,&QTimer::timeout,this,[this]{run(false);});
 deadline_.setSingleShot(true);deadline_.setInterval(childTimeoutMs);
 connect(&deadline_,&QTimer::timeout,this,[this]{if(process_){logging::warning(logApp(),"Update check took too long and was stopped");process_->kill();}});
 connect(&settings_,&SettingsService::accepted,this,[this]{schedule();emit changed();});
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
   attemptedAt_=parseTime(state["attemptedAt"]);checkedAt_=parseTime(state["checkedAt"]);
   result_=state["result"].toObject();error_=state["error"].toObject();notifiedVersion_=state["notifiedVersion"].toString();
   // A clock that went backwards must not postpone the next check for days.
   if(attemptedAt_>now())attemptedAt_={};
  }
 }
 schedule();
}
void UpdateService::schedule() {
 timer_.stop();nextAt_={};
 // Settings can change before start(); start() schedules then.
 if(!startedAt_.isValid()||!enabled())return;
 const auto earliest=startedAt_.addMSecs(startupDelayMs_);
 // A result for another channel says nothing about this one: check soon.
 auto due=attemptedAt_.isValid()&&result_["channel"]==channel()?attemptedAt_.addMSecs(intervalMs+jitter_):earliest;
 if(due<earliest)due=earliest;
 nextAt_=due;
 timer_.start(int(qBound(qint64(0),now().msecsTo(due),qint64(INT_MAX))));
}
QJsonObject UpdateService::status() const {
 const auto snapshot=settings_.snapshot();
 // Only a result for the current channel is shown.
 const bool current=result_["channel"]==channel()&&!channel().isEmpty();
 return {{"build",build_.toJson()},{"check",snapshot.checkForUpdates},{"channelSetting",snapshot.updateChannel},{"channel",channel().isEmpty()?QJsonValue():QJsonValue(channel())},
  {"enabled",enabled()},{"supported",build_.supported},{"reason",build_.reason.isEmpty()?QJsonValue():QJsonValue(build_.reason)},
  {"notifies",channel()=="stable"},{"checking",bool(process_)},{"attemptedAt",isoTime(attemptedAt_)},{"checkedAt",current?isoTime(checkedAt_):QJsonValue()},
  {"nextCheckAt",isoTime(nextAt_)},{"result",current?QJsonValue(result_):QJsonValue()},{"error",error_.isEmpty()?QJsonValue():QJsonValue(error_)}};
}
void UpdateService::check(JsonReply reply) {
 if(!build_.supported){reply({"update_check_unavailable","This build does not check for updates",{{"reason",build_.reason}}},{});return;}
 waiting_.append(std::move(reply));
 if(process_){manualRunning_=true;return;}
 run(true);
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
 manualRunning_=false;attemptedAt_=now();jitter_=random(-jitterMs,jitterMs);
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
  // Settings is not announced again; nightly is never announced.
  if(available&&latest!=notifiedVersion_) {
   if(!manual&&channel()=="stable"&&settings_.snapshot().notifications)emit updateAvailable(result);
   if(manual||channel()=="stable")notifiedVersion_=latest;
  }
 }
 save();schedule();emit changed();
 const auto replies=std::exchange(waiting_,{});
 for(const auto& reply:replies){if(error)reply(error,{});else reply({},status());}
}
void UpdateService::save() {
 const auto bytes=QJsonDocument(QJsonObject{{"schemaVersion",1},{"attemptedAt",isoTime(attemptedAt_)},{"checkedAt",isoTime(checkedAt_)},{"result",result_},
  {"error",error_.isEmpty()?QJsonValue():QJsonValue(error_)},{"notifiedVersion",notifiedVersion_}}).toJson(QJsonDocument::Compact);
 QSaveFile state(statePath_);state.setDirectWriteFallback(false);
 if(!state.open(QIODevice::WriteOnly)||!state.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||state.write(bytes)!=bytes.size()||!state.commit())
  logging::warning(logApp(),"Cannot save the update check state; the next start checks again");
}
}
