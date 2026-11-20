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
 // A configuration written before scroll capture has neither of its preferences. Settings shows
 // their defaults without adding them, so after a saved edit nothing reads as unsaved and an
 // explicit action still runs (f44, nightly 26.1120.5780: Upload clipboard was refused).
 void untouchedScrollCaptureKeysLeaveNothingUnsaved(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=threeDestinations();auto preferences=doc["preferences"].toObject();preferences.remove("scrollCaptureMaxHeight");preferences.remove("scrollCaptureFromTop");doc["preferences"]=preferences;
  QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();
  auto* notifications=window.settings()->findChild<QCheckBox*>("notifications");QVERIFY(notifications);
  const bool shown=notifications->isChecked();notifications->click();
  PUTGD_TRY_COMPARE(service.document()["preferences"].toObject()["notifications"].toBool(),!shown);
  PUTGD_TRY_VERIFY(window.settings()->flushAutosave()&&!window.settings()->actionSaveError());
  const auto saved=service.document()["preferences"].toObject();
  QVERIFY(!saved.contains("scrollCaptureMaxHeight")&&!saved.contains("scrollCaptureFromTop"));
 }
 // The status band ends in the same state whether the window opened at a width
 // or was resized to it, either way, and gives way in order: the delay control
 // stays whole, "Settings save automatically" goes before the version, then
 // the output summary shortens (W7 of #1532: with the delay control, a long
 // destination and a full-length version, 1,060 px dropped the version and
 // showed the explanation).
 void statusBandFitsALiveResizeAsAFreshLayout(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles.first().toObject();profile["label"]="Tray acceptance HTTP destination with a long label";profiles[0]=profile;doc["profiles"]=profiles;
  doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId",profile["id"]},{"publicationMode","default"}};
  auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;auto preferences=doc["preferences"].toObject();preferences["captureDelaySeconds"]=5;doc["preferences"]=preferences;
  QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  // A development build's version as it reads on a real desktop, wider than "Settings save automatically".
  const auto longVersion=[&]{fixture.session.applySnapshot({{"settings",QJsonObject{{"document",fixture.session.document()},{"metadata",fixture.session.metadata()}}},{"diagnostics",fixture.session.diagnostics()},{"build",QJsonObject{{"version","26.1114.5029-dev+gb91a65c2ee63"},{"dirty",false}}},{"residentEpoch",fixture.session.residentEpoch()},{"operationsReady",true}});};
  const auto band=[](MainWindow& window){
   auto* summary=window.findChild<QToolButton*>("captureOutputSummary");auto* delay=window.findChild<QToolButton*>("captureDelayControl");
   const bool explanation=window.findChild<QLabel*>("autosaveExplanation")->isVisibleTo(&window),version=window.findChild<QLabel*>("appVersion")->isVisibleTo(&window)||window.findChild<QWidget*>("appVersionGroup")->isVisibleTo(&window);
   return QStringList{QString("window %1").arg(window.width()),QString("summary %1").arg(summary->isVisibleTo(&window)?summary->width():0),QString("delay whole %1").arg(delay->width()>=delay->sizeHint().width()),QString("explanation %1").arg(explanation),QString("version %1").arg(version)};
  };
  const auto ordered=[](const QStringList& state){return !state.contains("explanation 1")||state.contains("version 1");};
  for(const auto* page:{"settings","destinations"}){
   const auto open=[&](MainWindow& window,int width){window.navigate(page);window.resize(width,880);window.show();longVersion();PUTGD_TRY_VERIFY(window.findChild<QLabel*>("appVersion")->text().startsWith("26.1114"));};
   MainWindow resized(fixture.session);open(resized,1440);
   for(const int width:{1440,1320,1200,1100,1060,1200,1440}){
    resized.resize(width,880);longVersion();
    MainWindow fresh(fixture.session);open(fresh,width);
    PUTGD_TRY_COMPARE(band(resized),band(fresh));
    const auto state=band(fresh);QVERIFY2(ordered(state),qPrintable(page+state.join(", ")));QVERIFY2(!state.contains("summary 0")&&state.contains("delay whole 1"),qPrintable(page+state.join(", ")));
   }
  }
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_general.moc"
