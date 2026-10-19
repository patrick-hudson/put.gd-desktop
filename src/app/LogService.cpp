#include "LogService.h"
#include "core/Log.h"
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <climits>
namespace putgd {
LogService::LogService(SettingsService& settings,QString stateRoot,QObject* parent):QObject(parent),settings_(settings),statePath_(stateRoot+"/debug-logging.json"),daily_(this),expiry_(this) {
 daily_.setInterval(24*60*60*1000);connect(&daily_,&QTimer::timeout,this,[]{logging::cleanup();});daily_.start();
 expiry_.setSingleShot(true);connect(&expiry_,&QTimer::timeout,this,&LogService::expire);
 connect(&settings_,&SettingsService::accepted,this,&LogService::apply);
}
void LogService::apply() {
 const auto snapshot=settings_.snapshot();
 logging::setRetention({snapshot.logDays,snapshot.logBytes});
 if(!snapshot.debugLogging) {
  if(active_)logging::info(logApp(),"Debug logging turned off");
  QFile::remove(statePath_);enabledAt_={};active_=false;expiry_.stop();logging::setDebug(false);
  return;
 }
 if(!enabledAt_.isValid()) {
  QFile file(statePath_);
  if(file.open(QIODevice::ReadOnly)&&file.size()<4096)enabledAt_=QDateTime::fromString(QJsonDocument::fromJson(file.readAll()).object()["enabledAt"].toString(),Qt::ISODateWithMs);
  // A start time in the future would keep debug logging on indefinitely.
  if(!enabledAt_.isValid()||enabledAt_>now()) {
   enabledAt_=now();
   QSaveFile state(statePath_);state.setDirectWriteFallback(false);
   const auto bytes=QJsonDocument(QJsonObject{{"enabledAt",enabledAt_.toString(Qt::ISODateWithMs)}}).toJson(QJsonDocument::Compact);
   if(!state.open(QIODevice::WriteOnly)||!state.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||state.write(bytes)!=bytes.size()||!state.commit())
    logging::warning(logApp(),"Cannot record when debug logging was turned on; it ends 24 hours after this start");
  }
 }
 const auto expires=enabledAt_.addMSecs(debugLifetimeMs);
 if(now()>=expires){expire();return;}
 if(!active_)logging::info(logApp(),"Debug logging turned on",{{"expiresAt",expires.toString(Qt::ISODateWithMs)}});
 active_=true;logging::setDebug(true,expires);
 expiry_.start(int(qMin<qint64>(now().msecsTo(expires)+1,INT_MAX)));
}
void LogService::expire() {
 expiry_.stop();
 if(active_||settings_.snapshot().debugLogging)logging::info(logApp(),"Debug logging turned itself off after 24 hours");
 active_=false;logging::setDebug(false);
 // Turn the setting off too, so the choice shows as off everywhere. A failed
 // patch (an invalid or changing file) leaves logging off and retries on the
 // next accepted change.
 if(settings_.snapshot().debugLogging&&settings_.metadata()["validation"].toObject()["ok"].toBool())
  settings_.patchAsync({{"preferences",QJsonObject{{"diagnostics",QJsonObject{{"debugLogging",false}}}}}},settings_.revision(),[](Result){});
}
QJsonObject LogService::debugStatus() const {
 return {{"enabled",active_},{"enabledAt",enabledAt_.isValid()?QJsonValue(enabledAt_.toString(Qt::ISODateWithMs)):QJsonValue()},
  {"expiresAt",enabledAt_.isValid()?QJsonValue(enabledAt_.addMSecs(debugLifetimeMs).toString(Qt::ISODateWithMs)):QJsonValue()}};
}
}
