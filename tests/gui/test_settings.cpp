#include "ui/SettingsWindow.h"
#include "ui/ShortcutEditor.h"
#include "fixtures/UiSessionFixture.h"
#include "ui/ViewState.h"
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSignalSpy>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QMessageBox>
#include <QPointer>
#include <QDialog>
#include <QPaintEvent>
#include "storage/s3/S3Provider.h"
using namespace putgd;using namespace putgd::test;
class SettingsPaintObserver final:public QObject {
public:int paints=0;QRegion region;
 bool eventFilter(QObject*,QEvent* event) override {if(event->type()==QEvent::Paint){++paints;region|=static_cast<QPaintEvent*>(event)->region();}return false;}
};
class SettingsGui:public QObject {
 Q_OBJECT

private slots:
 void savingOnlyAShortcutPreservesEveryUploadDestination() {
  QTemporaryDir directory;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService settings(directory.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});
  QVERIFY(settings.save(Settings::developmentTemplate(),{}));
  const auto before=settings.document()["actions"].toObject();
  UiSessionFixture fixture(settings,secrets);QTRY_VERIFY(fixture.session.ready());
  SettingsWindow window(fixture.session);
  window.findChild<QLineEdit*>("shortcutRegion")->setText("Ctrl+1");
  window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_COMPARE(settings.snapshot().actions["region"].shortcut,QString("Ctrl+1"));
  const auto after=settings.document()["actions"].toObject();
  for(const auto& name:Settings::actionNames()){
   QCOMPARE(after[name].toObject()["profileId"],before[name].toObject()["profileId"]);
   QCOMPARE(after[name].toObject()["destination"],before[name].toObject()["destination"]);
   if(name!="region")QCOMPARE(after[name],before[name]);
  }
 }
 void simpleDestinationAndCredentialFieldsShareAlignment(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();QTest::qWait(20);
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* advanced=window.findChild<QToolButton*>("advancedSettings");
  QVERIFY(bucket->isVisible());QVERIFY(access->isVisible());QCOMPARE(bucket->mapTo(&window,QPoint{}).x(),access->mapTo(&window,QPoint{}).x());
  advanced->setChecked(true);QTest::qWait(20);QCOMPARE(bucket->mapTo(&window,QPoint{}).x(),access->mapTo(&window,QPoint{}).x());
  advanced->setChecked(false);QTest::qWait(20);QCOMPARE(bucket->mapTo(&window,QPoint{}).x(),access->mapTo(&window,QPoint{}).x());
 }
 void emptySavedDestinationsShowOnlyTheAddAction(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));
  QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  QTRY_VERIFY(fixture.session.ready());SettingsWindow window(fixture.session);window.show();
  QVERIFY2(!window.findChild<QLineEdit*>("s3Bucket")->isVisible(),"An empty saved destination list must not show an inactive editor");
  QVERIFY(!window.findChild<QLineEdit*>("credentialAccessKey")->isVisible());
  auto* destination=window.findChild<QWidget*>("destinationEditor");auto* credentials=window.findChild<QWidget*>("credentialsEditor");
  QVERIFY(destination);QVERIFY(credentials);QVERIFY(!destination->isVisible());QVERIFY(!credentials->isVisible());
  auto* add=window.findChild<QPushButton*>("addS3Destination");QVERIFY(add->isVisible());
  const auto revision=service.revision();add->click();
  QVERIFY(destination->isVisible());QVERIFY(credentials->isVisible());QCOMPARE(service.revision(),revision);
  window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-draft-bucket");
  window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  window.findChild<QPushButton*>("removeDestination")->click();
  QVERIFY(!destination->isVisible());QVERIFY(!credentials->isVisible());QVERIFY(add->isVisible());QCOMPARE(service.revision(),revision);
 }
 void resettingDraftWaitsForTheResidentBeforeCheckpointing(){
  for(const auto& button:QStringList{"cancelSettings","reloadSettings"}){
   QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QTRY_VERIFY(fixture.session.ready());SettingsWindow window(fixture.session);window.show();
   const auto savedRoot=service.document()["archive"].toObject()["root"].toString();window.findChild<QLineEdit*>("archiveRoot")->setText(d.filePath("unsaved"));window.findChild<QPushButton*>(button)->click();
   QVERIFY2(window.saveViewState().contains("stateError"),"A pending reset must not checkpoint stale controls as clean");QVERIFY(!window.isEnabled());
   QTRY_VERIFY(window.isEnabled());QCOMPARE(window.findChild<QLineEdit*>("archiveRoot")->text(),savedRoot);QVERIFY(!window.saveViewState()["dirty"].toBool());
  }
 }
 void failedDraftResetPreservesDirtyControls(){
  for(const auto& button:QStringList{"cancelSettings","reloadSettings"}){
   QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QTRY_VERIFY(fixture.session.ready());SettingsWindow window(fixture.session);window.show();
   auto* root=window.findChild<QLineEdit*>("archiveRoot");root->setText(d.filePath("unsaved"));fixture.client.failOnce[button=="cancelSettings"?"ui.snapshot":"settings.reload"]={"client_unavailable","Synthetic reset failure",{}};
   window.findChild<QPushButton*>(button)->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("client_unavailable"));QVERIFY(window.isVisible());QVERIFY(window.isEnabled());QCOMPARE(root->text(),d.filePath("unsaved"));QVERIFY(window.saveViewState()["dirty"].toBool());
  }
 }
 void quitWithoutTrayIsExplicitAndDoesNotSaveDraft(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  bool trayAvailable=false;fixture.server.setUiDiagnostics([&]{return QJsonObject{{"trayAvailable",trayAvailable}};});
  bool ready=false;fixture.session.refreshSnapshot([&](Error error){QVERIFY(!error);ready=true;});QTRY_VERIFY(ready);
  SettingsWindow window(fixture.session);window.show();auto* quit=window.findChild<QPushButton*>("quitApplication");QVERIFY(quit);QVERIFY(quit->isVisible());QCOMPARE(quit->text(),QString("Quit put.gd"));QVERIFY(quit->toolTip().contains("pending"));
  const auto revision=service.revision();const auto document=service.document();window.findChild<QLineEdit*>("archiveRoot")->setText(d.filePath("unsaved"));QSignalSpy requests(&window,&SettingsWindow::quitRequested);
  quit->click();QCOMPARE(requests.size(),1);QCOMPARE(service.revision(),revision);QCOMPARE(service.document(),document);QVERIFY(window.saveViewState()["dirty"].toBool());QVERIFY(window.isVisible());
  trayAvailable=true;ready=false;fixture.session.refreshSnapshot([&](Error error){QVERIFY(!error);ready=true;});QTRY_VERIFY(ready);QVERIFY(quit->isHidden());
  window.close();QCOMPARE(requests.size(),1);
 }
 void quitFallbackRequiresExplicitMissingTrayDiagnostic(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();auto* quit=window.findChild<QPushButton*>("quitApplication");QVERIFY(quit);QVERIFY(quit->isHidden());
 }
 void acceptedSetupIdSurvivesAnUncertainInspectionReply(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));int starts=0;std::function<void(Error)> finish;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});fixture.client.failOnce["setup.inspect"]={"client_unavailable","Synthetic lost inspection reply",{}};SettingsWindow window(fixture.session);window.findChild<QPushButton*>("testDestination")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("client_unavailable"));const auto saved=window.saveViewState();QCOMPARE(saved["setupJobs"].toArray().size(),1);SettingsWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));QVERIFY(finish);finish({});QTRY_VERIFY(reopened.findChild<QLabel*>("settingsStatus")->text().contains("Setup completed"));QCOMPARE(starts,1);
 }
 void transientSetupInspectionRecoversWithoutReplayingAcceptedWork(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  int starts=0;std::function<void(Error)> finish;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});
  fixture.client.failOnce["setup.inspect"]={"client_unavailable","Synthetic lost inspection reply",{}};
  SettingsWindow window(fixture.session);
  auto* test=window.findChild<QPushButton*>("testDestination");
  auto* status=window.findChild<QLabel*>("settingsStatus");
  test->click();
  QTRY_VERIFY(status->text().contains("client_unavailable"));
  QCOMPARE(starts,1);QVERIFY(finish);QVERIFY(!test->isEnabled());
  window.findChild<QLineEdit*>("s3Bucket")->setText("unsaved-bucket");
  window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-unsubmitted-secret");
  finish({});
  QTRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(status->text().contains("Setup completed"));QVERIFY(test->isEnabled());
  QCOMPARE(starts,1);
  QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),QString("unsaved-bucket"));
  QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-unsubmitted-secret"));
  QVERIFY(window.saveViewState()["dirty"].toBool());
  QVERIFY(!QJsonDocument(window.saveViewState()).toJson().contains("synthetic-unsubmitted-secret"));
 }
 void expiredSetupResultDoesNotStarveLaterAcceptedWork(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  int starts=0;bool hold=false;std::function<void(Error)> finish;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;if(hold)finish=reply;else reply({});});
  auto start=[&]{QString id;fixture.setup.start("testDestination","development-s3",service.revision(),{},[&](Error error,QJsonObject data){QVERIFY(!error);id=data["setupId"].toString();});return id;};
  auto completed=[&](const QString& id){bool done=false;fixture.setup.inspect(id,[&](Error error,QJsonObject data){done=!error&&data["state"]=="completed";});return done;};
  QString expired;
  for(int i=0;i<65;++i){const auto id=start();QVERIFY(!id.isEmpty());if(i==0)expired=id;QTRY_VERIFY(completed(id));}
  Error missing;fixture.setup.inspect(expired,[&](Error error,QJsonObject){missing=error;});QCOMPARE(missing.code,QString("setup_not_found"));
  hold=true;const auto pending=start();QVERIFY(!pending.isEmpty());QVERIFY(finish);QCOMPARE(starts,66);
  SettingsWindow window(fixture.session);auto state=window.saveViewState();
  state["setupJobs"]=QJsonArray{QJsonObject{{"setupId",expired},{"kind","testDestination"},{"profileId","development-s3"}},QJsonObject{{"setupId",pending},{"kind","testDestination"},{"profileId","development-s3"}}};
  QVERIFY(window.restoreViewState(state));
  auto* test=window.findChild<QPushButton*>("testDestination");
  QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("setup_not_found"));
  QTRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QCOMPARE(window.saveViewState()["setupJobs"].toArray().first().toObject()["setupId"].toString(),pending);
  QVERIFY(!test->isEnabled());
  finish({});
  QTRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(test->isEnabled());QCOMPARE(starts,66);
 }
 void destinationTestButtonStaysDisabledUntilEveryAcceptedTestFinishes(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto second=profiles.first().toObject();second["id"]="second-s3";profiles.append(second);document["profiles"]=profiles;QVERIFY(service.save(document,{}));
  int starts=0;QHash<QString,std::function<void(Error)>> finish;
  UiSessionFixture fixture(service,secrets,[&](const QString& profile,auto,auto reply){++starts;finish[profile]=reply;});
  QJsonArray jobs;
  for(const auto& profile:QStringList{"development-s3","second-s3"})fixture.setup.start("testDestination",profile,service.revision(),{},[&](Error error,QJsonObject data){QVERIFY(!error);jobs.append(QJsonObject{{"setupId",data["setupId"]},{"kind","testDestination"},{"profileId",profile}});});
  QCOMPARE(starts,2);QCOMPARE(finish.size(),2);
  finish["development-s3"]({});
  SettingsWindow window(fixture.session);auto state=window.saveViewState();state["setupJobs"]=jobs;QVERIFY(window.restoreViewState(state));
  QTRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QVERIFY(!window.findChild<QPushButton*>("testDestination")->isEnabled());
  finish["second-s3"]({});
  QTRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(window.findChild<QPushButton*>("testDestination")->isEnabled());QCOMPARE(starts,2);
 }
 void ownSaveRetainsEditsMadeWhileItsAsyncReplyIsPending(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);auto* root=window.findChild<QLineEdit*>("archiveRoot");root->setText(d.filePath("submitted"));window.findChild<QPushButton*>("saveSettings")->click();QVERIFY(validateUiState({{"schemaVersion",1},{"settings",window.saveViewState()}}));root->setText(d.filePath("newer-unsaved"));QTRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),d.filePath("submitted"));QTRY_VERIFY(window.findChild<QPushButton*>("saveSettings")->isEnabled());QCOMPARE(root->text(),d.filePath("newer-unsaved"));QVERIFY(!window.saveViewState()["conflict"].toBool());QVERIFY(window.saveViewState()["dirty"].toBool());
 }
 void stateCodecRejectsSecretFieldsAndOversizeWithoutEchoingContents(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);auto saved=window.saveViewState();QVERIFY(!saved.contains("stateError"));auto doc=saved["draft"].toObject();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto credentials=profile["credentials"].toObject();credentials["secretKey"]="synthetic-nested-secret";profile["credentials"]=credentials;profiles[0]=profile;doc["profiles"]=profiles;saved["draft"]=doc;auto error=validateUiState({{"schemaVersion",1},{"settings",saved}});QVERIFY(error);QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-nested-secret"));
  auto* raw=window.findChild<QPlainTextEdit*>("providerSettingsJson");raw->setPlainText("{\"bucket\":\"draft\",\"secretKey\":\"synthetic-raw-secret\"}");QVERIFY(window.saveViewState().contains("stateError"));QVERIFY(!QJsonDocument(window.saveViewState()).toJson().contains("synthetic-raw-secret"));QCOMPARE(validateUiState({{"schemaVersion",1},{"padding",QString(maximumUiStateBytes,'x')}}).code,QString("ui_state_too_large"));
 }
 void recreatedDraftDetectsValidAndInvalidExternalChanges(){
  for(bool invalid:{false,true}){
   QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject state;
   {SettingsWindow window(fixture.session);window.findChild<QLineEdit*>("archiveRoot")->setText(d.filePath("unsaved"));window.findChild<QLineEdit*>("maxFileBytes")->setText("1.00000000001");state=window.saveViewState();}
   if(invalid){QFile file(service.path());QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("broken json");file.close();QVERIFY(!service.reload());}else{auto changed=service.document();auto archive=changed["archive"].toObject();archive["root"]=d.filePath("external");changed["archive"]=archive;QVERIFY(service.save(changed,service.revision()));}
   QTRY_COMPARE(fixture.session.metadata()["observedRevision"],service.metadata()["observedRevision"]);
   SettingsWindow restored(fixture.session);QVERIFY(restored.restoreViewState(state));QCOMPARE(restored.findChild<QLineEdit*>("archiveRoot")->text(),d.filePath("unsaved"));QCOMPARE(restored.findChild<QLineEdit*>("maxFileBytes")->text(),QString("1.00000000001"));QVERIFY(!restored.findChild<QPushButton*>("saveSettings")->isEnabled());QVERIFY(restored.findChild<QLabel*>("settingsStatus")->text().contains("config_conflict"));
  }
 }
 void profileDraftsRoundTripRemovalOrderAndMalformedNonsecretText(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject state;
  {SettingsWindow window(fixture.session);window.findChild<QLineEdit*>("s3Bucket")->setText("first");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("000bad");window.findChild<QPlainTextEdit*>("providerSettingsJson")->setPlainText("{\"bucket\":");window.findChild<QPushButton*>("addS3Destination")->click();window.findChild<QLineEdit*>("s3Bucket")->setText("second");window.findChild<QPushButton*>("addS3Destination")->click();window.findChild<QPushButton*>("removeDestination")->click();state=window.saveViewState();}
  QVERIFY(!state.contains("stateError"));auto doc=state["draft"].toObject();auto profiles=doc["profiles"].toArray();QCOMPARE(profiles.size(),2);const QJsonValue firstProfile=profiles[0];profiles[0]=profiles[1];profiles[1]=firstProfile;doc["profiles"]=profiles;state["draft"]=doc;auto editors=state["profileEditors"].toArray();const QJsonValue first=editors[0];editors[0]=editors[1];editors[1]=first;state["profileEditors"]=editors;state["profileIndex"]=1;
  SettingsWindow restored(fixture.session);QVERIFY(restored.restoreViewState(state));QCOMPARE(restored.findChild<QLineEdit*>("s3Bucket")->text(),QString("first"));QCOMPARE(restored.findChild<QLineEdit*>("s3ConnectTimeoutMs")->text(),QString("000bad"));QCOMPARE(restored.findChild<QPlainTextEdit*>("providerSettingsJson")->toPlainText(),QString("{\"bucket\":"));
  restored.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(0);QCOMPARE(restored.findChild<QLineEdit*>("s3Bucket")->text(),QString("second"));
 }
 void checkpointRejectsUnclassifiedProviderFieldsWithoutExportingThem(){
  QTemporaryDir d;CredentialStore secrets;ProviderRegistry registry;registry.add("opaque",std::make_shared<OpaqueProvider>());SettingsService service(d.filePath("config.json"),[&](auto id,auto settings){return registry.validate(id,settings);});QVERIFY(service.save(configuration(d.path()),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QPlainTextEdit*>("providerSettingsJson")->setPlainText("{\"secretKey\":\"synthetic-private-draft\"}");auto state=window.saveViewState();QVERIFY(validateUiState({{"schemaVersion",1},{"settings",state}}));QVERIFY(!QJsonDocument(state).toJson().contains("synthetic-private-draft"));QVERIFY(window.findChild<QPlainTextEdit*>("providerSettingsJson")->toPlainText().contains("synthetic-private-draft"));
  QVERIFY(validateUiState({{"schemaVersion",1},{"credentialSecretKey","injected"}}));
 }
 void acceptedSetupIsInspectedAfterRecreationWithoutRepeatingMutation(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});auto doc=Settings::developmentTemplate();QVERIFY(service.save(doc,{}));int starts=0;std::function<void(Error)> finish;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});QJsonObject state;
  {SettingsWindow window(fixture.session);window.findChild<QPushButton*>("testDestination")->click();QTRY_VERIFY(!window.saveViewState()["setupJobs"].toArray().isEmpty());state=window.saveViewState();}
  QCOMPARE(starts,1);SettingsWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(state));QVERIFY(finish);finish({});QTRY_VERIFY(reopened.findChild<QLabel*>("settingsStatus")->text().contains("Setup completed"));QCOMPARE(starts,1);
 }
 void recreatedSettingsRetainsRawDraftAndExcludesCredentialEntry(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  QJsonObject saved;
  {SettingsWindow window(fixture.session);window.findChild<QLineEdit*>("maxFiles")->setText("not an integer");window.findChild<QLineEdit*>("s3Bucket")->setText("unsaved-bucket");window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-entry-secret");window.findChild<QLineEdit*>("credentialAccessKey")->setText("synthetic-entry-access");window.findChild<QLineEdit*>("credentialSessionToken")->setText("synthetic-entry-token");window.findChild<QToolButton*>("advancedSettings")->setChecked(true);window.findChild<QListWidget*>("settingsNavigation")->setCurrentRow(3);saved=window.saveViewState();}
  QVERIFY(!QJsonDocument(saved).toJson().contains("synthetic-entry-"));
  QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",saved}}));
  SettingsWindow restored(fixture.session);QVERIFY(restored.restoreViewState(saved));
  QCOMPARE(restored.findChild<QLineEdit*>("maxFiles")->text(),QString("not an integer"));
  QCOMPARE(restored.findChild<QLineEdit*>("s3Bucket")->text(),QString("unsaved-bucket"));
  QCOMPARE(restored.findChild<QListWidget*>("settingsNavigation")->currentRow(),3);
  QVERIFY(restored.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty());
 }
 void explicitCloseReleasesResourcesAndKeepsConflictDraft() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);SettingsPaintObserver paint;window.installEventFilter(&paint);
  window.show();QTRY_VERIFY(window.testAttribute(Qt::WA_WState_Created));QTRY_VERIFY(paint.paints>0);QVERIFY(window.backingStore());
  auto* root=window.findChild<QLineEdit*>("archiveRoot");auto* advanced=window.findChild<QToolButton*>("advancedSettings");auto* navigation=window.findChild<QListWidget*>("settingsNavigation");auto* save=window.findChild<QPushButton*>("saveSettings");auto* status=window.findChild<QLabel*>("settingsStatus");QVERIFY(root&&advanced&&navigation&&save&&status);advanced->setChecked(true);navigation->setCurrentRow(0);root->setText(d.filePath("unsaved-draft"));
  auto changed=service.document();auto archive=changed["archive"].toObject();archive["root"]=d.filePath("external");changed["archive"]=archive;QVERIFY(service.save(changed,service.revision()));QTRY_VERIFY(!save->isEnabled());const auto conflict=status->text();const auto size=window.size();
  for(int cycle=0;cycle<3;++cycle){
   QVERIFY(window.close());QTRY_VERIFY_WITH_TIMEOUT(!window.testAttribute(Qt::WA_WState_Created),500);QCOMPARE(window.internalWinId(),WId(0));QTRY_VERIFY_WITH_TIMEOUT(!window.backingStore(),500);
   QCOMPARE(window.findChild<QLineEdit*>("archiveRoot"),root);QCOMPARE(root->text(),d.filePath("unsaved-draft"));QCOMPARE(status->text(),conflict);QTRY_VERIFY(!save->isEnabled());QVERIFY(advanced->isChecked());QCOMPARE(navigation->currentRow(),0);
   paint.paints=0;paint.region=QRegion{};window.show();QTRY_VERIFY(window.testAttribute(Qt::WA_WState_Created));QTRY_VERIFY(paint.paints>0);QVERIFY(paint.region.boundingRect().contains(window.rect()));QCOMPARE(window.size(),size);
  }
 }
 void resourceReleaseGuardsReopenMinimizeDialogsAndTeardown() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();QTRY_VERIFY(window.testAttribute(Qt::WA_WState_Created));
  window.hide();window.show();QTest::qWait(20);QVERIFY(window.testAttribute(Qt::WA_WState_Created));QVERIFY(window.isVisible());QVERIFY(window.close());window.show();QTest::qWait(20);QVERIFY(window.testAttribute(Qt::WA_WState_Created));QVERIFY(window.backingStore());
  window.showMinimized();QTest::qWait(20);QVERIFY(window.isMinimized());QVERIFY(window.testAttribute(Qt::WA_WState_Created));QVERIFY(window.backingStore());window.hide();QTest::qWait(20);QVERIFY(window.testAttribute(Qt::WA_WState_Created));QVERIFY(window.close());QTRY_VERIFY_WITH_TIMEOUT(!window.backingStore(),500);window.showNormal();QTRY_VERIFY(!window.isMinimized());
  {QDialog dialog(&window);dialog.setWindowModality(Qt::ApplicationModal);dialog.open();QTRY_VERIFY(dialog.isVisible());window.hide();QTest::qWait(20);QVERIFY(window.testAttribute(Qt::WA_WState_Created));QVERIFY(dialog.isVisible());dialog.close();}
  window.show();window.hide();QTRY_VERIFY_WITH_TIMEOUT(!window.testAttribute(Qt::WA_WState_Created),500);
  QPointer<SettingsWindow> dying=new SettingsWindow(fixture.session);dying->show();QTRY_VERIFY(dying->testAttribute(Qt::WA_WState_Created));dying->hide();delete dying.data();QVERIFY(dying.isNull());QTest::qWait(20);
 }
 void nativeStateSurvivesCloseRecreation() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  for(auto state:{Qt::WindowMaximized,Qt::WindowFullScreen,Qt::WindowMinimized}){
   window.showNormal();window.setWindowState(state);window.show();QTest::qWait(20);
   QVERIFY(window.windowHandle());QCOMPARE(window.windowHandle()->windowStates(),Qt::WindowStates(state));
   QVERIFY(window.close());QTRY_VERIFY_WITH_TIMEOUT(!window.backingStore(),500);
   window.show();QTest::qWait(20);QVERIFY(window.windowHandle());
   QCOMPARE(window.windowState()&~Qt::WindowActive,Qt::WindowStates(state));
   QCOMPARE(window.windowHandle()->windowStates(),Qt::WindowStates(state));
  }
 }
 void freshMissingConfigIsNormalSetup() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();bool finished=false;
  service.reloadAsync([&](Result result){QVERIFY(!result);QCOMPARE(result.error.code,QString("config_missing"));finished=true;});
  QTRY_VERIFY(finished);auto* status=window.findChild<QLabel*>("settingsStatus");QCOMPARE(status->property("feedbackTone").toString(),QString("neutral"));QCOMPARE(status->text(),QString("Changes apply after saving."));
  QCOMPARE(service.metadata()["validation"].toObject()["error"].toObject()["code"].toString(),QString("config_missing"));
 }
 void advancedFilenameTemplatesUseSharedConfiguration() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");advanced->setChecked(true);
  auto* mode=window.findChild<QComboBox*>("namingMode");QVERIFY(mode);QVERIFY(mode->findData("template")>=0);mode->setCurrentIndex(mode->findData("template"));
  for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"}){auto* field=window.findChild<QLineEdit*>(key);QVERIFY2(field,qPrintable(key));QVERIFY(field->toolTip().contains("{uuid}"));QVERIFY(window.findChild<QToolButton*>(key+"Help"));}
  window.findChild<QLineEdit*>("fileTemplate")->setText("Share-{name}.{ext}");window.findChild<QLineEdit*>("captureTemplate")->setText("Capture-{yyyy}-{MM}-{dd}-{uuid}.{ext}");
  auto* budget=window.findChild<QLineEdit*>("pendingImageBytes");QVERIFY(budget);QCOMPARE(budget->text(),QString("64"));budget->setText("32");
  window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.snapshot().namingMode,QString("template"));QCOMPARE(service.snapshot().captureTemplate,QString("Capture-{yyyy}-{MM}-{dd}-{uuid}.{ext}"));QCOMPARE(service.snapshot().limits.pendingImageBytes,qint64(33554432));
  auto revision=service.revision();window.findChild<QLineEdit*>("captureTemplate")->setText("{unknown}");window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("config_invalid"));QCOMPARE(service.revision(),revision);
 }
 void simpleSetupStartsWithoutDevelopmentAndSavesCompleteDestination() {
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);int tests=0;
  SettingsService service(d.filePath("config.json"),[&](auto id,auto value){return id=="s3"?provider.validateSettings(value):Error{"provider_unavailable","Unknown provider",{}};});
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto){++tests;});SettingsWindow window(fixture.session);window.show();
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");QVERIFY2(advanced,"Setup needs an explicit Advanced settings expansion");QVERIFY(!advanced->isChecked());
  QVERIFY(!window.findChild<QListWidget*>("settingsNavigation")->isVisible());QVERIFY(service.revision().isEmpty());QVERIFY(!QFile::exists(service.path()));QVERIFY(service.document()["profiles"].toArray().isEmpty());for(const auto& action:Settings::actionNames())QVERIFY(service.snapshot().actions[action].profileId.isEmpty());QCOMPARE(tests,0);
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QVERIFY(bucket->isVisible());QVERIFY2(bucket->isEnabled(),"Fresh setup must expose an editable destination without an Add step");bucket->setText("synthetic-bucket");
  window.findChild<QLineEdit*>("s3Region")->setText("eu-west-1");QVERIFY(!window.findChild<QComboBox*>("credentialMode")->isVisible());QVERIFY(!window.findChild<QLineEdit*>("credentialSessionToken")->isVisible());QVERIFY(!window.findChild<QComboBox*>("destinationProfile")->isVisible());
  auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");auto* token=window.findChild<QLineEdit*>("credentialSessionToken");for(auto* field:{access,secret,token})QCOMPARE(field->echoMode(),QLineEdit::Password);access->setText("synthetic-access");secret->setText("synthetic-secret");
  QVERIFY(!window.findChild<QLineEdit*>("s3Endpoint")->isVisible());window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(!service.revision().isEmpty());
  const auto profile=service.document()["profiles"].toArray().first().toObject();const auto settings=profile["settings"].toObject();
  QCOMPARE(settings["endpoint"].toString(),QString("https://s3.eu-west-1.amazonaws.com"));QCOMPARE(settings["aclMode"].toString(),QString("omit"));QCOMPARE(settings["storageClass"].toString(),QString("STANDARD"));QVERIFY(settings["publicUrlBase"].isNull());
  for(const auto& action:Settings::actionNames())QCOMPARE(service.snapshot().actions[action].profileId,profile["id"].toString());
  QVERIFY(profile["id"].toString()!="development-s3");QCOMPARE(tests,0);const auto encoded=QJsonDocument(service.document()).toJson();QVERIFY(!encoded.contains("synthetic-access"));QVERIFY(!encoded.contains("synthetic-secret"));QVERIFY(!encoded.contains("synthetic-token"));QVERIFY(!secrets.cached(profile["id"].toString(),profile["credentials"].toObject()));
  advanced->setChecked(true);QVERIFY(window.findChild<QComboBox*>("credentialMode")->isVisible());QVERIFY(token->isVisible());window.findChild<QComboBox*>("credentialMode")->setCurrentText("session");token->setText("synthetic-token");advanced->setChecked(false);window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().first().toObject()["credentials"].toObject()["mode"].toString(),QString("session"));
  const auto sessionProfile=service.document()["profiles"].toArray().first().toObject();QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-token"));QVERIFY(!secrets.cached(sessionProfile["id"].toString(),sessionProfile["credentials"].toObject()));window.findChild<QPushButton*>("storeCredentials")->click();QTRY_VERIFY(secrets.cached(sessionProfile["id"].toString(),sessionProfile["credentials"].toObject()));QVERIFY(access->text().isEmpty());QVERIFY(secret->text().isEmpty());QVERIFY(token->text().isEmpty());QCOMPARE(tests,0);
 }
 void advancedExpansionPreservesDraftAndCustomEndpoint() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();
  auto* toggle=window.findChild<QToolButton*>("advancedSettings");QVERIFY(toggle);
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("draft-bucket");window.findChild<QPushButton*>("addS3Destination")->click();auto* profiles=window.findChild<QComboBox*>("destinationProfile");QVERIFY(profiles->isVisible());profiles->setCurrentIndex(0);QCOMPARE(bucket->text(),QString("draft-bucket"));toggle->setFocus();QTest::keyClick(toggle,Qt::Key_Space);QVERIFY(toggle->isChecked());
  auto* endpoint=window.findChild<QLineEdit*>("s3Endpoint");endpoint->setText("https://objects.example.invalid");toggle->click();QCOMPARE(bucket->text(),QString("draft-bucket"));
  window.findChild<QLineEdit*>("s3Region")->setText("eu-north-1");QCOMPARE(endpoint->text(),QString("https://objects.example.invalid"));toggle->click();QCOMPARE(endpoint->text(),QString("https://objects.example.invalid"));QVERIFY(service.revision().isEmpty());
 }
 void regionSuggestionsAllowCustomRegionsWithoutChangingCustomEndpoints() {
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto id,auto value){return id=="s3"?provider.validateSettings(value):Error{"provider_unavailable","Unknown provider",{}};});UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();
  auto* region=window.findChild<QComboBox*>("s3RegionChoices");QVERIFY2(region,"Region needs searchable suggestions");QVERIFY(region->isEditable());QVERIFY(region->completer());QVERIFY(region->findText("us-east-2")>=0);QVERIFY(region->findText("ap-southeast-7")>=0);QVERIFY(region->findText("us-gov-west-1")>=0);
  auto* field=window.findChild<QLineEdit*>("s3Region");QCOMPARE(region->lineEdit(),field);field->setText("us-east-2");QCOMPARE(window.findChild<QLineEdit*>("s3Endpoint")->text(),QString("https://s3.us-east-2.amazonaws.com"));
  window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-bucket");window.findChild<QLineEdit*>("s3Endpoint")->setText("https://objects.example.invalid");field->setText("custom-region");window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(!service.revision().isEmpty());
  QCOMPARE(field->text(),QString("custom-region"));QCOMPARE(service.document()["profiles"].toArray().first().toObject()["settings"].toObject()["endpoint"].toString(),QString("https://objects.example.invalid"));
 }
 void cancelDiscardsDraftClearsCredentialsAndCloses() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();const auto document=service.document();const auto revision=service.revision();
  auto* root=window.findChild<QLineEdit*>("archiveRoot");root->setText(d.filePath("unsaved"));for(const auto& name:QStringList{"credentialAccessKey","credentialSecretKey","credentialSessionToken"})window.findChild<QLineEdit*>(name)->setText("synthetic-unsaved");
  auto* cancel=window.findChild<QPushButton*>("cancelSettings");QVERIFY2(cancel,"Settings needs a Cancel action");cancel->click();QTRY_VERIFY(!window.isVisible());QCOMPARE(service.document(),document);QCOMPARE(service.revision(),revision);QCOMPARE(root->text(),document["archive"].toObject()["root"].toString());for(const auto& name:QStringList{"credentialAccessKey","credentialSecretKey","credentialSessionToken"})QVERIFY(window.findChild<QLineEdit*>(name)->text().isEmpty());
  window.show();QCOMPARE(root->text(),document["archive"].toObject()["root"].toString());
 }
 void everySettingHasDetailedKeyboardAccessibleHelp() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  const QStringList names{"fileTemplate","captureTemplate","imageTemplate","textTemplate","pendingImageBytes","archiveRoot","keepUploadCopies","folderImages","folderVideos","folderText","folderOther","namingMode","collisionPolicy","destinationProfile","destinationId","destinationLabel","s3Bucket","s3Region","s3Endpoint","s3ObjectPrefix","s3StorageClass","s3PublicUrlBase","s3ConnectTimeoutMs","s3RequestTimeoutMs","s3LowSpeedBytesPerSecond","s3LowSpeedTimeSeconds","s3AclMode","s3Addressing","providerSettingsJson","credentialMode","credentialReference","credentialAccessKey","credentialSecretKey","credentialSessionToken","notifications","launchAtLogin","maxFiles","pendingOperations","concurrentUploads","maxFileBytes","maxBatchBytes","pendingArtifactBytes","clipboardTextBytes","imagePixels","imageBytes","failedArtifactDays","recentActivityLimit","previewCacheBytes","historyRetentionDays","shortcutRegion","shortcutWindow","shortcutDesktop","shortcutClipboard","destinationRegion","destinationWindow","destinationDesktop","destinationClipboard","profileRegion","profileWindow","profileDesktop","profileClipboard"};
  for(const auto& name:names){auto* control=window.findChild<QWidget*>(name);QVERIFY2(control,qPrintable(name));QVERIFY2(control->toolTip().size()>40,qPrintable("Missing detailed help: "+name));QToolButton* help=nullptr;for(auto* button:window.findChildren<QToolButton*>())if(button->property("settingHelpFor").toString()==name)help=button;QVERIFY2(help,qPrintable("Missing info button: "+name));QVERIFY(help->focusPolicy()!=Qt::NoFocus);QVERIFY(help->accessibleName().contains("Help"));}
  window.findChild<QListWidget*>("settingsNavigation")->setCurrentRow(1);QToolButton* help=nullptr;for(auto* button:window.findChildren<QToolButton*>())if(button->property("settingHelpFor").toString()=="s3Bucket")help=button;QVERIFY(help);help->setFocus();QTest::keyClick(help,Qt::Key_Space);QPointer<QMessageBox> dialog=window.findChild<QMessageBox*>("settingHelpDialog");QTRY_VERIFY(dialog&&dialog->isVisible());QCOMPARE(dialog->informativeText(),window.findChild<QLineEdit*>("s3Bucket")->toolTip());dialog->close();QTRY_VERIFY(dialog.isNull());
 }
 void savingShowsSuccessAndReloadIsExplicit() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();auto* local=window.findChild<QPushButton*>("setupLocalSimple");QCOMPARE(local->text(),QString("Save locally"));QVERIFY(local->toolTip().contains("four"));auto* reload=window.findChild<QPushButton*>("reloadSettings");QCOMPARE(reload->text(),QString("Reload from file"));QVERIFY(reload->toolTip().contains("Discard"));
  local->click();window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(!service.revision().isEmpty());auto* status=window.findChild<QLabel*>("settingsStatus");QTRY_VERIFY(status->text().contains("Settings saved"));QCOMPARE(status->property("feedbackTone").toString(),QString("success"));window.findChild<QLineEdit*>("archiveRoot")->setText("relative/path");window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(status->property("feedbackTone").toString(),QString("error"));QVERIFY(!status->text().contains("Settings saved"));
 }
 void sizeInputsSaveFractionalMiBAndPreserveExactAcceptedBytes() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));auto doc=configuration(d.path(),true);auto limits=doc["limits"].toObject();limits["maxBatchBytes"]=qint64(2147483649LL);doc["limits"]=limits;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* field=window.findChild<QLineEdit*>("maxFileBytes");QVERIFY2(field->accessibleName().contains("MiB"),"Size controls must declare binary units");field->setText("1.5");window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_COMPARE(service.document()["limits"].toObject()["maxFileBytes"].toInteger(),qint64(1572864));QCOMPARE(service.document()["limits"].toObject()["maxBatchBytes"].toInteger(),qint64(2147483649LL));
  window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QPushButton*>("saveSettings")->isEnabled());QCOMPARE(service.document()["limits"].toObject()["maxBatchBytes"].toInteger(),qint64(2147483649LL));
  auto revision=service.revision();for(const auto& invalid:QStringList{"0.0000001","999999999999999999999999","bad"}){field->setText(invalid);window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QPushButton*>("saveSettings")->isEnabled());QCOMPARE(service.revision(),revision);QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("config_invalid"));}
 }

 void shortcutStatesAreReadableAndExactDiagnosticsRemainAvailable() {
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.resize(700,450);editor.show();const auto draft=editor.actions({});
  const QJsonObject diagnostics{{"available",true},{"bindings",QJsonObject{
   {"region",QJsonObject{{"requested","Ctrl+Shift+1"},{"actual","Ctrl+Alt+R"},{"bound",true}}},
   {"window",QJsonObject{{"requested","Ctrl+Shift+2"},{"actual",QJsonArray{"Ctrl+Alt+W"}},{"bound",true},{"state","shortcut_conflict"}}},
   {"desktop",QJsonObject{{"requested","Ctrl+Shift+3"},{"bound",false},{"error","helper_unavailable"}}},
   {"clipboard",QJsonObject{{"requested",""},{"bound",false},{"error","disabled"}}}}},{"nativeExtra","kept exactly"}};
  editor.setStatus(diagnostics);
  auto status=[&](const QString& action){return editor.findChild<QLabel*>("shortcut"+action+"Status");};
  QVERIFY2(status("Region"),"Each action needs a readable binding status beside its editor");QVERIFY(status("Region")->text().contains("Active"));QVERIFY(status("Region")->text().contains("Ctrl+Alt+R"));
  QVERIFY(status("Window"));QVERIFY(status("Window")->text().contains("Conflict"));QVERIFY(!status("Window")->text().contains("Active"));
  QVERIFY(status("Desktop"));QVERIFY(status("Desktop")->text().contains("Unavailable"));QVERIFY(status("Clipboard"));QVERIFY(status("Clipboard")->text().contains("Disabled"));QCOMPARE(editor.actions({}),draft);
  auto* details=editor.findChild<QPlainTextEdit*>("shortcutDetails");auto* toggle=editor.findChild<QToolButton*>("shortcutDetailsToggle");QVERIFY(details);QVERIFY(toggle);QVERIFY(!details->isVisible());toggle->setFocus();QTest::keyClick(toggle,Qt::Key_Space);QVERIFY(details->isVisible());QCOMPARE(QJsonDocument::fromJson(details->toPlainText().toUtf8()).object(),diagnostics);
  editor.setStatus({{"available",true},{"bindings",QJsonObject{{"region",QJsonObject{{"requested","Ctrl+Shift+1"},{"actual",QJsonArray{"Ctrl+Alt+R","Meta+R"}},{"bound",true},{"state","bound"}}},{"window",QJsonObject{{"requested","Ctrl+Shift+2"},{"bound",false},{"state","pending"}}}}}});
  QVERIFY(status("Region")->text().contains("Ctrl+Alt+R"));QVERIFY(status("Region")->text().contains("Meta+R"));QVERIFY(status("Window")->text().contains("Binding"));QVERIFY(!status("Desktop")->text().contains("Active"));
  editor.setStatus({{"available",false},{"bindings",QJsonObject{}}});for(auto action:QStringList{"Region","Window","Desktop","Clipboard"})QVERIFY(status(action)->text().contains("Unavailable"));QCOMPARE(editor.actions({}),draft);
 }

 void sectionsAreKeyboardReachableAndFooterStaysInsideCompactWindow() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.resize(1000,700);window.show();QTest::qWait(20);
  QVERIFY2(window.width()<=1000&&window.height()<=700,"Settings must fit a 1000×700 logical-pixel window");
  auto* navigation=window.findChild<QListWidget*>("settingsNavigation");QVERIFY2(navigation,"Settings sections need keyboard-accessible navigation");
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");QVERIFY(advanced);advanced->setChecked(true);navigation->setCurrentRow(0);
  auto* pages=window.findChild<QStackedWidget*>("settingsPages");QVERIFY(pages);QCOMPARE(navigation->count(),4);
  navigation->setFocus();QTRY_VERIFY(navigation->hasFocus());
  for(int i=0;i<4;++i){navigation->setCurrentRow(i);QCOMPARE(pages->currentIndex(),i);auto* scroll=qobject_cast<QScrollArea*>(pages->currentWidget());QVERIFY(scroll);QVERIFY(scroll->widgetResizable());QVERIFY(scroll->viewport()->width()>=500);}
  navigation->setCurrentRow(0);QTest::qWait(20);
  auto* captureScroll=qobject_cast<QScrollArea*>(pages->currentWidget());QVERIFY(captureScroll);
  for(const auto& name:Settings::actionNames()){
   auto* action=window.findChild<QPushButton*>("invoke"+name.left(1).toUpper()+name.mid(1));QVERIFY(action);QVERIFY(action->isVisible());
   const QRect bounds(action->mapTo(captureScroll->viewport(),QPoint{}),action->size());
   QVERIFY2(captureScroll->viewport()->rect().contains(bounds),qPrintable("Capture action requires scrolling: "+name));
  }
  QTest::keyClick(navigation,Qt::Key_Down);QCOMPARE(pages->currentIndex(),1);
  auto* save=window.findChild<QPushButton*>("saveSettings");QVERIFY(save->isVisible());QVERIFY(window.rect().contains(save->mapTo(&window,save->rect().bottomRight())));
 }

 void destinationsCanBeCreatedAndRemovedWithoutChangingOtherProfiles() {
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto id,auto value){return id=="s3"?provider.validateSettings(value):Error{"provider_unavailable","Unknown provider",{}};});QVERIFY(service.save(configuration(d.path(),true),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* add=window.findChild<QPushButton*>("addS3Destination");QVERIFY2(add,"Setup must allow a new S3 destination without importing development settings");add->click();
  window.findChild<QLineEdit*>("destinationId")->setText("personal");window.findChild<QLineEdit*>("destinationLabel")->setText("Personal upload");window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-bucket");window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),1);
  auto first=service.document()["profiles"].toArray().first().toObject();QCOMPARE(first["id"].toString(),QString("personal"));QVERIFY(first["settings"].toObject()["publicUrlBase"].isNull());
  add->click();window.findChild<QLineEdit*>("destinationId")->setText("second");window.findChild<QLineEdit*>("s3Bucket")->setText("second-synthetic-bucket");window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),2);QCOMPARE(service.document()["profiles"].toArray().first().toObject(),first);
  auto* remove=window.findChild<QPushButton*>("removeDestination");QVERIFY(remove);remove->click();window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray().first().toObject(),first);
 }
 void reloadingInvalidFileKeepsValidationVisible() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);auto revision=service.revision();QFile file(service.path());QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("invalid json");file.close();QVERIFY(!service.reload());
  QSignalSpy completed(&service,&SettingsService::statusChanged);window.findChild<QPushButton*>("reloadSettings")->click();QTRY_VERIFY(!completed.isEmpty());QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("config_invalid"));QCOMPARE(service.revision(),revision);
 }
 void guiActionsAreAvailableWithoutTray() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);QSignalSpy actions(&window,&SettingsWindow::actionRequested);
  for(auto name:Settings::actionNames()){auto* button=window.findChild<QPushButton*>("invoke"+name.left(1).toUpper()+name.mid(1));QVERIFY(button);button->click();QCOMPARE(actions.takeFirst().first().toString(),name);}QVERIFY(service.revision().isEmpty());
 }
 void s3SetupOptionalUrlAndSessionModeUseSharedValidation() {
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto id,auto value){return id=="s3"?provider.validateSettings(value):Error{"provider_unavailable","Unknown provider",{}};});UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QPushButton*>("importDevelopmentS3")->click();window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-bucket");window.findChild<QLineEdit*>("s3PublicUrlBase")->clear();window.findChild<QComboBox*>("s3AclMode")->setCurrentText("omit");window.findChild<QComboBox*>("credentialMode")->setCurrentText("session");window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_VERIFY(!service.revision().isEmpty());auto profile=service.document()["profiles"].toArray().first().toObject();QCOMPARE(profile["settings"].toObject()["bucket"].toString(),QString("synthetic-bucket"));QVERIFY(profile["settings"].toObject()["publicUrlBase"].isNull());QCOMPARE(profile["settings"].toObject()["aclMode"].toString(),QString("omit"));QVERIFY(profile["credentials"].toObject()["reference"].isNull());QCOMPARE(profile["credentials"].toObject()["mode"].toString(),QString("session"));
  auto revision=service.revision();window.findChild<QLineEdit*>("s3Endpoint")->setText("bad-endpoint");window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("s3_settings_invalid"));QCOMPARE(service.revision(),revision);
 }
 void invalidExternalFilePreservesAcceptedStateAndBlocksStaleSave() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);const auto revision=service.revision();window.findChild<QLineEdit*>("archiveRoot")->setText(d.filePath("draft"));QFile file(service.path());QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("invalid json");file.close();QVERIFY(!service.reload());QCOMPARE(service.revision(),revision);window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("config_conflict"));QFile preserved(service.path());QVERIFY(preserved.open(QIODevice::ReadOnly));QCOMPARE(preserved.readAll(),QByteArray("invalid json"));
 }
 void localSetupAndAtomicValidation() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.show();auto* local=window.findChild<QPushButton*>("setupLocalSimple");QVERIFY(local);QVERIFY(local->isVisible());local->click();
  auto* save=window.findChild<QPushButton*>("saveSettings");QVERIFY(save);save->click();QTRY_VERIFY(!service.revision().isEmpty());
  QCOMPARE(service.snapshot().actions["region"].destination,QString("local"));
  auto old=service.document();auto* root=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(root);root->setText("relative/path");save->click();
  QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("absolute"));QCOMPARE(service.document(),old);
 }
 void externalAcceptancePreservesDraftAndRequiresReload() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");QVERIFY(advanced);advanced->setChecked(true);auto* root=window.findChild<QLineEdit*>("archiveRoot");root->setText(d.filePath("draft"));advanced->setChecked(false);auto changed=service.document();auto archive=changed["archive"].toObject();archive["root"]=d.filePath("external");changed["archive"]=archive;QVERIFY(service.save(changed,service.revision()));
  QCOMPARE(root->text(),d.filePath("draft"));QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("conflict",Qt::CaseInsensitive));QVERIFY(!window.findChild<QPushButton*>("saveSettings")->isEnabled());
  window.findChild<QPushButton*>("reloadSettings")->click();QTRY_COMPARE(root->text(),d.filePath("external"));
 }
 void actionAndFolderChangesRemainIndependent() {
  QTemporaryDir d;ProviderRegistry providers;providers.add("opaque",std::make_shared<OpaqueProvider>());SettingsService service(d.filePath("config.json"),[&](auto id,auto s){return providers.validate(id,s);});QVERIFY(service.save(configuration(d.path()),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto shownProfile=QJsonDocument::fromJson(window.findChild<QPlainTextEdit*>("providerSettingsJson")->toPlainText().toUtf8()).object();QVERIFY2(shownProfile.contains("workspace"),"Generic provider form must load its actual workspace settings");QCOMPARE(shownProfile.size(),1);
  window.findChild<QComboBox*>("destinationRegion")->setCurrentIndex(1);window.findChild<QLineEdit*>("folderVideos")->setText(d.filePath("video"));window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_VERIFY2(service.snapshot().actions["region"].destination=="local",qPrintable(window.findChild<QLabel*>("settingsStatus")->text()));QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("upload"));QCOMPARE(service.snapshot().archive.folders["videos"],d.filePath("video"));QVERIFY(service.snapshot().archive.folders["images"].isEmpty());QCOMPARE(service.snapshot().collisionPolicy,QString("rename"));
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings.moc"
