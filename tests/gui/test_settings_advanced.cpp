#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 // #158: a diagnostics package (logs and history) is uploaded only when Upload is
 // ticked for that save; the box never starts ticked, so a plain Save stays local.
 void diagnosticsPackageOffersAWorkingUpload(){
  QTemporaryDir dir;logging::open(dir.filePath("state/logs"),{},"putgd-test");auto closeLog=qScopeGuard([]{logging::close();});
  logging::info(logApp(),"Synthetic diagnostics record");logging::flush();
  ProviderRegistry providers;auto provider=std::make_shared<test::OpaqueProvider>();providers.add("opaque",provider);
  SettingsService service(dir.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(service.save(test::configuration(dir.path()),{}));
  test::Source source;OperationService operations(dir.filePath("data"),service,providers,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);
  UiSessionFixture fixture(service,operations,history);
  DiagnosticsBundles bundles(operations,dir.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);
  bundles.setDestinationTest([&](const QString&,std::function<void(Error)> done){done({});});
  MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();auto* settings=window.settings();
  auto command=[&](const QJsonObject& params)->std::pair<Error,QJsonObject>{bool done=false;Error error{"test_timeout","No answer",{}};QJsonObject result;settings->diagnosticsCommand(params,[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!putgd::test::waitFor([&]{return done;},15000))qWarning("diagnostics command timed out");return {error,result};};
  auto package=[&]{return settings->diagnosticsState()["package"].toObject();};
  const int uploadsBefore=provider->uploads.load();
  command({{"action","openPackage"}});PUTGD_TRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
  auto* dialog=settings->findChild<QDialog*>("diagnosticsPackageDialog");auto* upload=dialog->findChild<QCheckBox*>("uploadPackage");QVERIFY(upload);QVERIFY(!upload->isChecked());QVERIFY(!package()["upload"].toBool());
  auto [local,localState]=command({{"action","savePackage"}});QVERIFY2(!local,qPrintable(local.message));QVERIFY(!localState["package"].toObject()["saved"].toObject().contains("link"));QCOMPARE(provider->uploads.load(),uploadsBefore);
  command({{"action","closePackage"}});
  command({{"action","openPackage"}});PUTGD_TRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());upload->click();QVERIFY(package()["upload"].toBool());
  auto [started,startedState]=command({{"action","savePackage"}});QVERIFY2(!started,qPrintable(started.message));
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(package().contains("saved"),20000);QVERIFY(package()["saved"].toObject()["link"].toString().startsWith("https://"));QCOMPARE(provider->uploads.load(),uploadsBefore+1);
  command({{"action","closePackage"}});
  command({{"action","openPackage"}});PUTGD_TRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
  QVERIFY(!upload->isChecked());QVERIFY(!package()["upload"].toBool());
  auto [plain,plainState]=command({{"action","savePackage"}});QVERIFY(!plain);QVERIFY(!plainState["package"].toObject()["saved"].toObject().contains("link"));PUTGD_COMPARE_STEADY(provider->uploads.load(),uploadsBefore+1,50);
  command({{"action","closePackage"}});
  operations.shutdown();
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_advanced.moc"
