#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 // Opening Settings on an existing configuration (mixed providers, an action
 // routed to a non-default destination) rewrites nothing: routes stay as saved.
 void openingExistingConfigurationPreservesExactFileAndRoutes(){
  QTemporaryDir dir;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();
  auto proton=profiles[1].toObject();proton["provider"]="proton-drive";proton["settings"]=ProtonProvider::defaults();profiles[1]=proton;
  auto backblaze=profiles[2].toObject();backblaze["provider"]="backblaze-b2";auto settings=b2::defaults();settings["bucket"]="synthetic-bucket";backblaze["settings"]=settings;profiles[2]=backblaze;
  doc["profiles"]=profiles;
  QVERIFY(service.save(doc,{}));
  QFile file(service.path());QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();
  const auto revision=service.revision();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  int writes=0;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="settings.save"||method=="settings.patch")++writes;};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QCOMPARE(window.findChild<QListWidget*>("destinationList")->count(),doc["profiles"].toArray().size());
  const auto state=window.settings()->saveViewState();QVERIFY(!state.contains("stateError"));
  MainWindow reopened(fixture.session);QVERIFY(reopened.settings()->restoreViewState(state));
  fixture.session.invalidate();PUTGD_COMPARE_STEADY(writes,0,650);
  QCOMPARE(writes,0);QCOMPARE(service.document(),doc);QCOMPARE(service.revision(),revision);
  QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),bytes);
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_general.moc"
