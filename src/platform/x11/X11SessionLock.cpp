#include "X11SessionLock.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QPointer>
#include <memory>
namespace putgd {
namespace {
constexpr int answerMs=500;
// Only silence counts as locked: a missing service, object or session (a
// resident outside a logind session) is an answer that nothing is locked.
bool silent(const QDBusError& error){return error.type()==QDBusError::NoReply||error.type()==QDBusError::Timeout;}
}
void querySessionLocked(QObject* context,std::function<void(bool)> done){
 struct State {int waiting=0;bool locked=false;std::function<void(bool)> done;};
 auto state=std::make_shared<State>();state->done=std::move(done);
 QPointer<QObject> guard(context);
 auto settle=[state,guard](bool locked){
  state->locked=state->locked||locked;
  if(--state->waiting==0&&guard)state->done(state->locked);
 };
 QList<std::pair<QDBusConnection,QDBusMessage>> calls;
 for(const auto& [service,path]:{std::pair{QStringLiteral("org.cinnamon.ScreenSaver"),QStringLiteral("/org/cinnamon/ScreenSaver")},{QStringLiteral("org.freedesktop.ScreenSaver"),QStringLiteral("/org/freedesktop/ScreenSaver")}}){
  auto message=QDBusMessage::createMethodCall(service,path,service,"GetActive");message.setAutoStartService(false);
  calls.append({QDBusConnection::sessionBus(),message});
 }
 auto system=QDBusConnection::systemBus();
 if(system.isConnected()){
  auto message=QDBusMessage::createMethodCall("org.freedesktop.login1","/org/freedesktop/login1/session/auto","org.freedesktop.DBus.Properties","Get");
  message.setArguments({QStringLiteral("org.freedesktop.login1.Session"),QStringLiteral("LockedHint")});message.setAutoStartService(false);
  calls.append({system,message});
 }
 state->waiting=int(calls.size());
 for(auto& [bus,message]:calls){
  if(!bus.isConnected()){settle(false);continue;}
  auto* watcher=new QDBusPendingCallWatcher(bus.asyncCall(message,answerMs),context);
  QObject::connect(watcher,&QDBusPendingCallWatcher::finished,context,[watcher,settle]{
   watcher->deleteLater();const QDBusMessage reply=watcher->reply();
   if(reply.type()==QDBusMessage::ErrorMessage){settle(silent(QDBusError(reply)));return;}
   const auto arguments=reply.arguments();if(arguments.isEmpty()){settle(false);return;}
   auto value=arguments.first();
   if(value.canConvert<QDBusVariant>())value=value.value<QDBusVariant>().variant();
   settle(value.toBool());
  });
 }
}
}
