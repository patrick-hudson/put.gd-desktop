#include "app/UiProcessController.h"
#include "app/LocalServer.h"
#include "app/SetupService.h"
#include "ui/ViewState.h"
#include "fixtures/U2Fixtures.h"
#include <csignal>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
class UiSplitServices:public QObject {
 Q_OBJECT
private slots:
 void helperCrashDoesNotCancelAnAcceptedUploadOrSetupJob(){
  QTemporaryDir dir;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(dir.filePath("config.json"),[&](const QString& provider,const QJsonObject& value){return providers.validate(provider,value);});QVERIFY(settings.save(configuration(dir.path()),{}));
  Source source;source.payload=textPayload();OperationService operations(dir.filePath("data"),settings,providers,source);HistoryService history(operations);
  LocalServer server(settings,operations,history);QVERIFY(server.listen(dir.filePath("runtime")));PUTGD_TRY_VERIFY(operations.ready());
  provider->deferUploads=true;provider->dispatcher=QCoreApplication::instance();
  CredentialStore secrets;std::function<void(Error)> setupCompletion;
  SetupService setup(settings,secrets,[&](const QString&,const QJsonObject&,auto reply){setupCompletion=std::move(reply);});server.setSetupService(&setup);
  QString setupId;setup.start("testDestination","test",settings.revision(),{},[&](Error e,QJsonObject result){QVERIFY(!e);setupId=result["setupId"].toString();});QVERIFY(!setupId.isEmpty());QVERIFY(setupCompletion);
  UiProcessController ui(server.socketPath(),server.residentEpoch(),PUTGD_PRODUCT_UI_PATH,validateUiState);
  bool shown=false;Error uiError;ui.activate("activity",[&](Error e){uiError=e;shown=true;});PUTGD_TRY_VERIFY_WITH_TIMEOUT(shown,10000);QVERIFY2(!uiError,qPrintable(uiError.code));
  QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);operationId=result["operationId"].toString();});PUTGD_TRY_VERIFY(!operationId.isEmpty());PUTGD_TRY_COMPARE(provider->completions.size(),1);
  const auto pid=ui.status()["pid"].toInteger();QVERIFY(pid>0);QCOMPARE(::kill(pid_t(pid),SIGKILL),0);PUTGD_TRY_COMPARE(ui.status()["pid"].toInteger(),qint64(0));
  provider->completions.takeFirst()();setupCompletion({});
  PUTGD_TRY_COMPARE(source.copies,1);QCOMPARE(provider->uploads.load(),1);
  QJsonObject result;operations.inspect(operationId,[&](Error e,QJsonObject value){QVERIFY(!e);result=value;});PUTGD_TRY_VERIFY(!result.isEmpty());QCOMPARE(result["state"].toString(),QString("completed"));
  setup.inspect(setupId,[&](Error e,QJsonObject value){QVERIFY(!e);result=value;});QCOMPARE(result["state"].toString(),QString("completed"));
  shown=false;ui.activate("about",[&](Error e){uiError=e;shown=true;});PUTGD_TRY_VERIFY_WITH_TIMEOUT(shown,10000);QVERIFY2(!uiError,qPrintable(uiError.code));QCOMPARE(provider->uploads.load(),1);
  bool stopped=false;ui.shutdown([&]{stopped=true;});PUTGD_TRY_VERIFY(stopped);QCOMPARE(ui.status()["lastExit"].toObject()["status"].toString(),QString("normal"));QCOMPARE(ui.status()["lastExit"].toObject()["code"].toInt(-1),0);
 }
};
QTEST_GUILESS_MAIN(UiSplitServices)
#include "test_ui_split_services.moc"
