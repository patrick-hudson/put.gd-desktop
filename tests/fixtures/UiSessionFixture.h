#pragma once
#include "U2Fixtures.h"
#include "app/LocalServer.h"
#include "app/SetupService.h"
#include "secrets/CredentialStore.h"
#include "ui/UiSession.h"
#include <QPointer>
#include <QSet>
#include <QTimer>
namespace putgd::test {
// The wire transport has a separate suite. Views still dispatch the production
// resident endpoint asynchronously; this fixture does not fabricate its replies.
class DispatchSessionClient final:public SessionClient {
public:
 QHash<QString,Error> failOnce;
 QHash<QString,int> replyDelayMs;
 std::function<void(const QString&,const QJsonObject&)> beforeDispatch;
 explicit DispatchSessionClient(LocalServer& server):SessionClient(server.socketPath()),server_(server){}
 RequestId request(const QString& method,const QJsonObject& params,QObject* receiver,Reply reply)override{
  const auto id=++next_;QPointer<QObject> guard(receiver);
  QTimer::singleShot(0,this,[this,id,method,params,guard,reply]{if(!guard||abandoned_.remove(id))return;if(beforeDispatch)beforeDispatch(method,params);if(auto error=failOnce.take(method)){reply(error,{});return;}server_.dispatch({{"schemaVersion",1},{"requestId",QString::number(id)},{"session",server_.socketPath()},{"method",method},{"params",params}},[this,id,method,guard,reply](Error e,QJsonObject data){auto deliver=[this,id,guard,reply,e,data]{if(guard&&!abandoned_.remove(id))reply(e,data);};const auto delay=replyDelayMs.value(method);if(delay>0)QTimer::singleShot(delay,this,deliver);else deliver();});});return id;
 }
 void abandon(RequestId id)override{abandoned_.insert(id);}
private:LocalServer& server_;RequestId next_=0;QSet<RequestId> abandoned_;
};
class UiSessionFixture {
 ProviderRegistry registry_;Source source_;QTemporaryDir data_;
 std::unique_ptr<OperationService> ownedOperations_;std::unique_ptr<HistoryService> ownedHistory_;CredentialStore ownedSecrets_;
public:
 LocalServer server;SetupService setup;DispatchSessionClient client;UiSession session;
 OperationService& operations(){return *operations_;}
 UiSessionFixture(SettingsService& settings,CredentialStore& secrets,SetupService::DestinationTester tester={}):
  ownedOperations_(std::make_unique<OperationService>(data_.path(),settings,registry_,source_)),ownedHistory_(std::make_unique<HistoryService>(*ownedOperations_)),
  server(settings,*ownedOperations_,*ownedHistory_),setup(settings,secrets,tester?std::move(tester):SetupService::DestinationTester([](const QString&,const QJsonObject&,auto reply){reply({});})),client(server),session(client){initialize(settings,*ownedOperations_,*ownedHistory_);}
 UiSessionFixture(SettingsService& settings,OperationService& operations,HistoryService& history):server(settings,operations,history),setup(settings,ownedSecrets_,[](const QString&,const QJsonObject&,auto reply){reply({});}),client(server),session(client){initialize(settings,operations,history);}
private:
 OperationService* operations_=nullptr;
 void initialize(SettingsService& settings,OperationService& operations,HistoryService& history){
  operations_=&operations;
  server.setSetupService(&setup);server.dispatch({{"schemaVersion",1},{"requestId","bootstrap"},{"session",server.socketPath()},{"method","ui.snapshot"},{"params",QJsonObject{}}},[this](Error e,QJsonObject data){Q_ASSERT(!e);session.applySnapshot(data);});
  QObject::connect(&settings,&SettingsService::statusChanged,&session,&UiSession::invalidate);
  QObject::connect(&operations,&OperationService::initialized,&session,&UiSession::invalidate);
  QObject::connect(&operations,&OperationService::changed,&session,&UiSession::invalidate);
  QObject::connect(&history,&HistoryService::changed,&session,&UiSession::invalidate);
 }
};
}
