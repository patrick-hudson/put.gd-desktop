#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include "app/SetupService.h"
#include "core/BuildInfo.h"
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QSignalSpy>
using namespace putgd;
using namespace putgd::test;

class UiEndpointsTest : public QObject {
 Q_OBJECT
 struct Fixture {
  QTemporaryDir root;
  ProviderRegistry providers;
  SettingsService settings{root.filePath("config.json"),[](const QString& provider,const QJsonObject& value){
   if(provider=="opaque"&&value==QJsonObject{{"workspace","fixture-workspace"}})return Error{};
   return Error{"config_invalid","Synthetic provider settings required",{}};
  }};
  Source source;
  OperationService operations{root.filePath("data"),settings,providers,source};
  HistoryService history{operations};
  LocalServer server{settings,operations,history};
  bool saveProfiles(int count=1) {
   auto document=configuration(root.path(),true);QJsonArray profiles;
   for(int i=0;i<count;++i)profiles.append(QJsonObject{{"id",count==1?"test":QString("profile%1").arg(i)},{"label","Synthetic destination"},{"provider","opaque"},{"settings",QJsonObject{{"workspace","fixture-workspace"}}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}});
   document["profiles"]=profiles;return bool(settings.save(document,settings.revision()));
  }
 };
 static void request(LocalServer& server,const QString& method,const QJsonObject& params,JsonReply reply) {
  server.dispatch({{"schemaVersion",1},{"requestId","ui-endpoint-test"},{"session",server.socketPath()},{"method",method},{"params",params}},std::move(reply));
 }
 static QJsonObject wire(LocalServer& server,const QString& method,const QJsonObject& params) {
  QLocalSocket socket;socket.connectToServer(server.socketPath());if(!socket.waitForConnected(1000))return {};
  socket.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"requestId","wire-setup-test"},{"session",server.socketPath()},{"method",method},{"params",params}}).toJson(QJsonDocument::Compact)+'\n');
  QElapsedTimer timer;timer.start();QByteArray response;
  while(timer.elapsed()<3000&&!response.contains('\n')){QCoreApplication::processEvents();response+=socket.readAll();QTest::qWait(1);}
  return QJsonDocument::fromJson(response).object();
 }
private slots:
 void snapshotIncludesResidentIdentitySettingsAndDiagnostics() {
  Fixture f;QVERIFY(f.settings.save(configuration(f.root.path(),true),{}));QTRY_VERIFY(f.operations.ready());
  Error error;QJsonObject result;bool done=false;
  request(f.server,"ui.snapshot",{},[&](Error e,QJsonObject value){error=e;result=value;done=true;});
  QVERIFY(done);QVERIFY2(!error,qPrintable(error.code));
  QCOMPARE(result["build"].toObject(),QJsonDocument::fromJson(buildManifestJson()).object());
  QVERIFY(!result["residentEpoch"].toString().isEmpty());
  QVERIFY(result["operationsReady"].toBool());
  QCOMPARE(result["settings"].toObject()["document"].toObject(),f.settings.document());
  QCOMPARE(result["settings"].toObject()["metadata"].toObject(),f.settings.metadata());
  QCOMPARE(result["diagnostics"].toObject(),f.operations.diagnostics());
  QCOMPARE(f.source.reads,0);QCOMPARE(f.source.copies,0);
 }
 void saveReturnsAcceptedDocumentAndMetadataWithConflictProtection() {
  Fixture f;QVERIFY(f.settings.save(configuration(f.root.path(),true),{}));
  const auto revision=f.settings.revision();auto candidate=f.settings.document();
  auto preferences=candidate["preferences"].toObject();preferences["notifications"]=false;candidate["preferences"]=preferences;
  Error error;QJsonObject result;bool done=false;
  request(f.server,"settings.save",{{"document",candidate},{"expectedRevision",revision}},[&](Error e,QJsonObject value){error=e;result=value;done=true;});
  QTRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(result["document"].toObject(),candidate);
  QCOMPARE(result["metadata"].toObject(),f.settings.metadata());QVERIFY(!f.settings.snapshot().notifications);
  done=false;request(f.server,"settings.save",{{"document",candidate},{"expectedRevision",revision}},[&](Error e,QJsonObject){error=e;done=true;});
  QTRY_VERIFY(done);QCOMPARE(error.code,QString("config_conflict"));
 }
 void setupEndpointsWithoutServiceReportUnavailable() {
  Fixture f;
  for(const auto& method:QStringList{"setup.storeCredentials","setup.unlockCredentials","setup.testDestination","setup.revealCredentialSecret","setup.inspect"}) {
   QJsonObject params{{"profileId","test"},{"expectedRevision","saved-revision"}};
   if(method=="setup.storeCredentials")params["credentials"]=QJsonObject{{"accessKey","synthetic-access"},{"secretKey","synthetic-secret"}};
   if(method=="setup.inspect")params={{"id","missing"}};
   Error error;bool done=false;request(f.server,method,params,[&](Error e,QJsonObject){error=e;done=true;});
   QVERIFY(done);QCOMPARE(error.code,QString("setup_unavailable"));
  }
 }
 void sessionCredentialsBecomeResidentJobsWithoutPersistedSecrets() {
  Fixture f;QVERIFY(f.saveProfiles());CredentialStore credentials;SetupService setup(f.settings,credentials,{});
  QSignalSpy changed(&setup,&SetupService::changed);Error error;QJsonObject accepted;
  const QJsonObject input{{"accessKey","synthetic-access"},{"secretKey","synthetic-secret"},{"sessionToken","synthetic-token"}};
  setup.start("storeCredentials","test",f.settings.revision(),input,[&](Error e,QJsonObject value){error=e;accepted=value;});
  QVERIFY2(!error,qPrintable(error.code));QCOMPARE(accepted.keys(),QStringList{"setupId"});const auto id=accepted["setupId"].toString();QVERIFY(!id.isEmpty());
  QJsonObject record;setup.inspect(id,[&](Error e,QJsonObject value){error=e;record=value;});QVERIFY(!error);QCOMPARE(record["state"].toString(),QString("running"));
  setup.start("storeCredentials","test",f.settings.revision(),input,[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("setup_busy"));
  QTRY_VERIFY(changed.count()>=2);setup.inspect(id,[&](Error e,QJsonObject value){error=e;record=value;});QVERIFY(!error);QCOMPARE(record["state"].toString(),QString("completed"));
  QCOMPARE(record["kind"].toString(),QString("storeCredentials"));QCOMPARE(record["profileId"].toString(),QString("test"));QVERIFY(record["error"].isObject());
  const auto reference=f.settings.snapshot().profiles["test"]["credentials"].toObject();const auto cached=credentials.cached("test",reference);QVERIFY(cached);QCOMPARE(cached->sessionToken,QString("synthetic-token"));QVERIFY(!credentials.cached("other",reference));
  const auto bytes=QJsonDocument(record).toJson()+QJsonDocument(f.settings.document()).toJson();QVERIFY(!bytes.contains("synthetic-access"));QVERIFY(!bytes.contains("synthetic-secret"));QVERIFY(!bytes.contains("synthetic-token"));
  QFile saved(f.settings.path());QVERIFY(saved.open(QIODevice::ReadOnly));QVERIFY(!saved.readAll().contains("synthetic-secret"));
 }
 void credentialRevealRejectsStaleMissingAndNonS3Destinations() {
  Fixture f;QVERIFY(f.saveProfiles());CredentialStore credentials;SetupService setup(f.settings,credentials,{});f.server.setSetupService(&setup);
  Error error;QJsonObject result;
  auto reveal=[&](const QString& id,const QString& revision){
   request(f.server,"setup.revealCredentialSecret",{{"profileId",id},{"expectedRevision",revision}},[&](Error e,QJsonObject value){error=e;result=value;});
   QVERIFY(result.isEmpty());
  };
  reveal("test","stale");QCOMPARE(error.code,QString("config_conflict"));
  reveal("missing",f.settings.revision());QCOMPARE(error.code,QString("profile_not_found"));
  reveal("test",f.settings.revision());QCOMPARE(error.code,QString("setup_unsupported"));
 }
 void setupRequiresCurrentValidSavedProfileAndStrictCredentialInput() {
  Fixture f;QVERIFY(f.saveProfiles());CredentialStore credentials;int calls=0;
  SetupService setup(f.settings,credentials,[&](const QString&,const QJsonObject&,auto){++calls;});Error error;
  const auto reply=[&](Error e,QJsonObject){error=e;};
  setup.start("testDestination","test","stale",{},reply);QCOMPARE(error.code,QString("config_conflict"));
  setup.start("testDestination","missing",f.settings.revision(),{},reply);QCOMPARE(error.code,QString("profile_not_found"));
  for(const auto& input:QList<QJsonObject>{
       {{"accessKey","synthetic-access"},{"secretKey","synthetic-secret"},{"reference","arbitrary-reference"}},
       {{"accessKey",42},{"secretKey","synthetic-secret"}},
       {{"accessKey","synthetic-access"},{"secretKey","synthetic-secret"},{"sessionToken",42}},
       {{"accessKey","synthetic-access"},{"secretKey",QString(4097,'s')}}}) {
   setup.start("storeCredentials","test",f.settings.revision(),input,reply);QCOMPARE(error.code,QString("invalid_request"));
  }
  setup.start("unlockCredentials","test",f.settings.revision(),{{"reference","untrusted"}},reply);QCOMPARE(error.code,QString("invalid_request"));
  const auto accepted=f.settings.revision();QFile file(f.settings.path());QVERIFY(file.open(QIODevice::WriteOnly));file.write("{invalid");file.close();QVERIFY(f.settings.reload().error);
  setup.start("testDestination","test",accepted,{},reply);QVERIFY(error);QCOMPARE(calls,0);
 }
 void delayedDestinationJobCompletesOnOwnerThreadAndIgnoresRepeatedCompletion() {
  Fixture f;QVERIFY(f.saveProfiles());CredentialStore credentials;std::function<void(Error)> finish;int tests=0;
  QJsonObject supplied;QString profileId;
  SetupService setup(f.settings,credentials,[&](const QString& id,const QJsonObject& profile,auto callback){++tests;profileId=id;supplied=profile;finish=std::move(callback);});
  QSignalSpy changed(&setup,&SetupService::changed);bool wrongThread=false;
  connect(&setup,&SetupService::changed,&setup,[&]{wrongThread|=QThread::currentThread()!=setup.thread();});
  Error error;QString id;setup.start("testDestination","test",f.settings.revision(),{},[&](Error e,QJsonObject value){error=e;id=value["setupId"].toString();});
  QVERIFY(!error);QVERIFY(!id.isEmpty());QCOMPARE(tests,1);QCOMPARE(profileId,QString("test"));QCOMPARE(supplied,f.settings.snapshot().profiles["test"]);
  setup.start("testDestination","test",f.settings.revision(),{},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("setup_busy"));QCOMPARE(tests,1);
  std::thread worker([finish]{finish({"synthetic_failure","Synthetic destination test failed",{}});finish({});});worker.join();
  QTRY_COMPARE(changed.count(),2);QVERIFY(!wrongThread);QJsonObject record;setup.inspect(id,[&](Error e,QJsonObject value){error=e;record=value;});QVERIFY(!error);QCOMPARE(record["state"].toString(),QString("failed"));QCOMPARE(record["error"].toObject()["code"].toString(),QString("synthetic_failure"));
 }
 void completionsAfterServiceDestructionAreSafe() {
  Fixture f;QVERIFY(f.saveProfiles());CredentialStore credentials;std::function<void(Error)> finish;
  auto setup=std::make_unique<SetupService>(f.settings,credentials,[&](const QString&,const QJsonObject&,auto cb){finish=std::move(cb);});
  Error error;setup->start("testDestination","test",f.settings.revision(),{},[&](Error e,QJsonObject){error=e;});QVERIFY(!error);QVERIFY(finish);setup.reset();
  std::thread worker([finish]{finish({});});worker.join();QCoreApplication::processEvents();
 }
 void completedJobsAreBoundedWhileActiveJobsSurvive() {
  Fixture f;QVERIFY(f.saveProfiles());CredentialStore credentials;QList<std::function<void(Error)>> finishes;
  SetupService setup(f.settings,credentials,[&](const QString&,const QJsonObject&,auto cb){finishes.append(std::move(cb));});
  QString activeId;Error error;setup.start("testDestination","test",f.settings.revision(),{},[&](Error e,QJsonObject value){error=e;activeId=value["setupId"].toString();});QVERIFY(!error);
  QString first,last;for(int i=0;i<70;++i) {
   setup.start("unlockCredentials","test",f.settings.revision(),{},[&](Error e,QJsonObject value){error=e;last=value["setupId"].toString();});QVERIFY(!error);if(i==0)first=last;
   QJsonObject record;QTRY_VERIFY(([&]{setup.inspect(last,[&](Error e,QJsonObject value){error=e;record=value;});return record["state"]=="failed";})());
   QCOMPARE(record["error"].toObject()["code"].toString(),QString("credential_setup_required"));
  }
  setup.inspect(first,[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("setup_not_found"));
  setup.inspect(last,[&](Error e,QJsonObject){error=e;});QVERIFY(!error);
  QJsonObject active;setup.inspect(activeId,[&](Error e,QJsonObject value){error=e;active=value;});QVERIFY(!error);QCOMPARE(active["state"].toString(),QString("running"));
 }
 void activeJobsAreBounded() {
  Fixture f;QVERIFY(f.saveProfiles(32));CredentialStore credentials;
  SetupService setup(f.settings,credentials,[](const QString&,const QJsonObject&,auto){});Error error;
  for(int i=0;i<32;++i){setup.start("testDestination",QString("profile%1").arg(i),f.settings.revision(),{},[&](Error e,QJsonObject){error=e;});QVERIFY(!error);}
  setup.start("unlockCredentials","profile0",f.settings.revision(),{},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("setup_busy"));
 }
 void snapshotEpochIsStableAndAugmentedDiagnosticsAreUsed() {
  Fixture f;const auto epoch=f.server.residentEpoch();QVERIFY(!epoch.isEmpty());
  LocalServer second(f.settings,f.operations,f.history);QVERIFY(second.residentEpoch()!=epoch);
  const QJsonObject diagnostics{{"login",QJsonObject{{"supported",true}}},{"shortcuts",QJsonObject{{"region",true}}}};
  f.server.setUiDiagnostics([diagnostics]{return diagnostics;});
  Error error;QJsonObject result;request(f.server,"ui.snapshot",{},[&](Error e,QJsonObject value){error=e;result=value;});
  QVERIFY(!error);QCOMPARE(result["residentEpoch"].toString(),epoch);QCOMPARE(result["diagnostics"].toObject(),diagnostics);
  request(f.server,"ui.snapshot",{{"unexpected",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("invalid_request"));
 }
 void wireSetupJobsSurviveClientDepartureAndUseSavedProfile() {
  Fixture f;QVERIFY(f.saveProfiles());QVERIFY(f.server.listen(f.root.filePath("runtime")));CredentialStore credentials;
  std::function<void(Error)> finish;QJsonObject tested;
  SetupService setup(f.settings,credentials,[&](const QString&,const QJsonObject& profile,auto callback){tested=profile;finish=std::move(callback);});f.server.setSetupService(&setup);
  const QJsonObject target{{"profileId","test"},{"expectedRevision",f.settings.revision()}};
  const auto accepted=wire(f.server,"setup.testDestination",target);QVERIFY(accepted["ok"].toBool());QVERIFY(finish);
  const auto id=accepted["result"].toObject()["setupId"].toString();QVERIFY(!id.isEmpty());QCOMPARE(tested,f.settings.snapshot().profiles["test"]);
  auto record=wire(f.server,"setup.inspect",{{"id",id}});QVERIFY(record["ok"].toBool());QCOMPARE(record["result"].toObject()["state"].toString(),QString("running"));
  finish({});QCoreApplication::processEvents();record=wire(f.server,"setup.inspect",{{"id",id}});QVERIFY(record["ok"].toBool());QCOMPARE(record["result"].toObject()["state"].toString(),QString("completed"));
  auto input=target;input["credentials"]=QJsonObject{{"accessKey","synthetic-wire-access"},{"secretKey","synthetic-wire-secret"}};
  const auto stored=wire(f.server,"setup.storeCredentials",input);QVERIFY(stored["ok"].toBool());
  const auto reference=tested["credentials"].toObject();QVERIFY(credentials.cached("test",reference));QCOMPARE(credentials.cached("test",reference)->secretKey,QString("synthetic-wire-secret"));
  const auto snapshot=wire(f.server,"ui.snapshot",{});QVERIFY(snapshot["ok"].toBool());QVERIFY(!QJsonDocument(snapshot).toJson().contains("synthetic-wire-secret"));
  const auto unlock=wire(f.server,"setup.unlockCredentials",target);QVERIFY(unlock["ok"].toBool());
  const auto unlocked=wire(f.server,"setup.inspect",{{"id",unlock["result"].toObject()["setupId"]}});QVERIFY(unlocked["ok"].toBool());QCOMPARE(unlocked["result"].toObject()["state"].toString(),QString("failed"));
 }
 void wireValidatesSetupAndSaveFieldsAndDropsDestroyedService() {
  Fixture f;QVERIFY(f.saveProfiles());CredentialStore credentials;auto setup=std::make_unique<SetupService>(f.settings,credentials,SetupService::DestinationTester{});f.server.setSetupService(setup.get());
  const auto revision=f.settings.revision();
  const QList<QPair<QString,QJsonObject>> invalid{
   {"setup.testDestination",{{"profileId","test"},{"expectedRevision",revision},{"profile",QJsonObject{}}}},
   {"setup.unlockCredentials",{{"profileId","test"},{"expectedRevision",revision},{"reference","not-saved"}}},
   {"setup.storeCredentials",{{"profileId","test"},{"expectedRevision",revision},{"credentials",QJsonObject{{"accessKey","synthetic"},{"secretKey","synthetic"},{"reference","not-saved"}}}}},
   {"setup.inspect",{{"id",42}}},
   {"setup.revealCredentialSecret",{{"profileId","test"},{"expectedRevision",revision},{"reference","untrusted"}}},
   {"setup.revealCredentialSecret",{{"profileId",42},{"expectedRevision",revision}}},
   {"settings.save",{{"document",QJsonArray{}},{"expectedRevision",revision}}},
   {"settings.save",{{"document",f.settings.document()},{"expectedRevision",42}}}
  };
  Error error;for(const auto& [method,params]:invalid){request(f.server,method,params,[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("invalid_request"));}
  setup.reset();request(f.server,"setup.inspect",{{"id","missing"}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("setup_unavailable"));
 }
};
QTEST_GUILESS_MAIN(UiEndpointsTest)
#include "test_ui_endpoints.moc"
