#include "SettingsGuiFixture.h"
#include <QDBusContext>
#include <QDBusConnection>
namespace {
// A KWallet on the private bus answers QtKeychain the way kwalletd does, so a
// keychain read is a real asynchronous job (#600). Reads are counted.
class StartupWallet final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.kde.KWallet")
public:
 QByteArray stored;int reads=0;
public slots:
 QString networkWallet(){return "isolated-synthetic-wallet";}
 int open(const QString&,qlonglong,const QString&){return 7;}
 int entryType(int,const QString&,const QString&,const QString&){return 2;}
 QByteArray readEntry(int,const QString&,const QString&,const QString&){++reads;return stored;}
};
}
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 void destinationCardsKeepConnectionTestAfterCredentials(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();window.navigate("destinations");putgd::test::settle(30);window.settings();
  auto* credentials=window.findChild<QWidget*>("credentialsEditor");auto* where=window.findChild<QWidget*>("destinationEditor");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* link=window.findChild<QLineEdit*>("s3PublicUrlBase");auto* test=window.findChild<QPushButton*>("testDestination");QVERIFY(credentials&&where&&access&&link&&test);
  PUTGD_TRY_VERIFY(credentials->isVisible());QVERIFY(where->mapTo(&window,QPoint()).y()<credentials->mapTo(&window,QPoint()).y());QVERIFY(link->mapTo(&window,QPoint()).y()<access->mapTo(&window,QPoint()).y());QVERIFY(credentials->isAncestorOf(test));QVERIFY(window.findChild<QScrollArea*>("destinationScroll")->widget()->isAncestorOf(test));QVERIFY(test->isVisible());QVERIFY(test->width()<=test->sizeHint().width()+4);
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");advanced->setChecked(true);PUTGD_TRY_VERIFY(window.findChild<QLineEdit*>("s3StorageClass")->isVisible());QVERIFY(advanced->mapTo(&window,QPoint()).y()<link->mapTo(&window,QPoint()).y());QVERIFY(where->isAncestorOf(advanced));QVERIFY(where->isAncestorOf(window.findChild<QLineEdit*>("s3StorageClass")));
  advanced->setChecked(false);auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("backblaze-b2"));QVERIFY(where->isAncestorOf(advanced));provider->setCurrentIndex(provider->findData("proton-drive"));PUTGD_VERIFY_STEADY(!where->isAncestorOf(advanced),30);QVERIFY(window.findChild<QWidget*>("advancedConnectionRow")->parentWidget()->isAncestorOf(window.findChild<QSpinBox*>("protonExpiration")));auto* folder=window.findChild<QComboBox*>("protonFolder");QWidget* folderLabel=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==folder)folderLabel=label;QVERIFY(folderLabel);PUTGD_TRY_VERIFY(folderLabel->minimumWidth()<=Ui::scaled(240));const int gutter=folderLabel->minimumWidth();advanced->setChecked(true);putgd::test::settle(30);PUTGD_TRY_VERIFY(folderLabel->minimumWidth()>=gutter);advanced->setChecked(false);PUTGD_TRY_COMPARE(folderLabel->minimumWidth(),gutter);
 }
 void storeEligibilityPreservesTypedSecrets_data(){
  QTest::addColumn<bool>("password");QTest::newRow("credentials")<<false;QTest::newRow("link-password")<<true;
 }
 void storeEligibilityPreservesTypedSecrets(){
  QFETCH(bool,password);QTemporaryDir dir;auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();
  profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
  if(password){profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();}profiles[0]=profile;doc["profiles"]=profiles;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* store=window.findChild<QPushButton*>(password?"storeProtonPassword":"storeCredentials");auto* field=window.findChild<QLineEdit*>(password?"protonSharePassword":"credentialSecretKey");
  auto* reason=window.findChild<QLabel*>(password?"passwordStoreReason":"credentialStoreReason");auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* token=window.findChild<QLineEdit*>("credentialSessionToken");
  field->setText("synthetic-typed-secret");access->setText("synthetic-access");token->setText("synthetic-token");int starts=0;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method.startsWith("setup.store"))++starts;};
  fixture.client.replyDelayMs["settings.save"]=250;store->setFocus();label->setText("Changed destination");
  QVERIFY(!store->isEnabled());QVERIFY(!reason->isHidden());QVERIFY(reason->text().contains("saving"));
  QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(starts,0);QCOMPARE(field->text(),QString("synthetic-typed-secret"));
  window.settings()->flushAutosave();QVERIFY(!store->isEnabled());QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(field->text(),QString("synthetic-typed-secret"));
  PUTGD_TRY_VERIFY(store->isEnabled());QVERIFY(reason->isHidden());
  // An unfinished destination edit blocks Store before either group is read.
  label->setFocus();label->setText("Unfinished destination");QVERIFY(!store->isEnabled());QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(field->text(),QString("synthetic-typed-secret"));QCOMPARE(starts,0);
  label->setText("Changed destination");QMetaObject::invokeMethod(label,"editingFinished",Qt::DirectConnection);PUTGD_TRY_VERIFY(store->isEnabled());
  fixture.client.failOnce["settings.save"]={"synthetic_save_refused","Synthetic save refusal",{}};label->setText("Retry destination");QMetaObject::invokeMethod(label,"editingFinished",Qt::DirectConnection);window.settings()->flushAutosave();PUTGD_TRY_VERIFY(window.settings()->saveViewState()["autosaveBlocked"].toBool());QVERIFY(!store->isEnabled());QVERIFY(reason->text().contains("Retry saving"));QCOMPARE(field->text(),QString("synthetic-typed-secret"));window.findChild<QPushButton*>("retryDestinationAutosave")->click();PUTGD_TRY_VERIFY(store->isEnabled());
  auto external=service.document();auto prefs=external["preferences"].toObject();prefs["notifications"]=!prefs["notifications"].toBool();external["preferences"]=prefs;QVERIFY(service.save(external,service.revision()));
  PUTGD_TRY_VERIFY(window.settings()->saveViewState()["conflict"].toBool());QVERIFY(!store->isEnabled());QVERIFY(reason->text().contains("conflict"));
  QVERIFY(!window.findChild<QPushButton*>("reloadCredentialConflict"));QVERIFY(!window.findChild<QPushButton*>("reloadPasswordConflict"));QVERIFY(reason->text().contains("Resolve the settings conflict before saving credentials"));auto* reload=window.findChild<QPushButton*>("reloadDestinationConflict");QVERIFY(reload&&!reload->isHidden());auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* banner=window.findChild<QWidget*>("destinationStatusRow");QVERIFY(!scroll->isAncestorOf(banner));scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());PUTGD_TRY_VERIFY(banner->isVisible());QVERIFY(window.rect().contains(QRect(banner->mapTo(&window,QPoint()),banner->size())));
  QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(starts,0);QCOMPARE(field->text(),QString("synthetic-typed-secret"));QCOMPARE(access->text(),QString("synthetic-access"));QCOMPARE(token->text(),QString("synthetic-token"));
  const auto confirmation=acceptNextConfirmation();reload->click();QVERIFY(confirmation->startsWith("Reload settings from file\n"));PUTGD_TRY_VERIFY(!window.settings()->saveViewState()["conflict"].toBool());QVERIFY(field->text().isEmpty());PUTGD_TRY_VERIFY(reload->isHidden());
 }
 void storeClearsSecretsOnlyAfterAdmission_data(){storeEligibilityPreservesTypedSecrets_data();}
 void storeClearsSecretsOnlyAfterAdmission(){
  QFETCH(bool,password);QTemporaryDir dir;auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
  if(password){profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();}profiles[0]=profile;doc["profiles"]=profiles;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  fixture.setup.setProviderSetup([](const QString&,const Destination&,const QJsonObject& input,JsonReply reply,auto){reply({},{{"reference",input["reference"]}});});
  MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* store=window.findChild<QPushButton*>(password?"storeProtonPassword":"storeCredentials");auto* field=window.findChild<QLineEdit*>(password?"protonSharePassword":"credentialSecretKey");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");
  const auto method=password?QString("setup.storeSharePassword"):QString("setup.storeCredentials");field->setText("synthetic-secret");access->setText("synthetic-access");fixture.client.failOnce[method]={"synthetic_refusal","Synthetic setup refusal",{}};
  store->click();QCOMPARE(field->text(),QString("synthetic-secret"));PUTGD_TRY_COMPARE(statusCode(window),QString("synthetic_refusal"));QVERIFY(store->isEnabled());QCOMPARE(field->text(),QString("synthetic-secret"));
  fixture.client.replyDelayMs[method]=250;store->click();QVERIFY(!store->isEnabled());QCOMPARE(field->text(),QString("synthetic-secret"));PUTGD_TRY_VERIFY(field->text().isEmpty());if(!password)QVERIFY(access->text().isEmpty());
  QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-secret"));QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains("synthetic-secret"));
 }
 void choosingKeychainFromSessionCreatesOnlyAReference(){
  QTemporaryDir dir;auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);int stores=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="setup.storeCredentials")++stores;};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* mode=window.findChild<QComboBox*>("credentialMode");window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-kept-input");mode->setCurrentIndex(mode->findData("keychain"));
  PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["credentials"].toObject()["mode"].toString(),QString("keychain"));QCOMPARE(window.findChild<QLineEdit*>("credentialReference")->text(),QString("putgd/development-s3"));QCOMPARE(stores,0);QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-kept-input"));QVERIFY(!window.findChild<QLabel*>("credentialStorageNotice")->isHidden());
 }
 void protonConnectionTestAllowsMakingTheDestinationDefault_data(){
  QTest::addColumn<bool>("rootFolder");QTest::addColumn<bool>("editAfterTest");
  QTest::newRow("top-level-folder")<<false<<false;QTest::newRow("my-files-root")<<true<<false;QTest::newRow("changed-folder-needs-retest")<<false<<true;
 }
 void protonConnectionTestAllowsMakingTheDestinationDefault(){
  QFETCH(bool,rootFolder);QFETCH(bool,editAfterTest);
  QTemporaryDir dir;CredentialStore secrets;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=configuration(dir.path(),true);auto account=ProtonProvider::defaults();
  account["accountRootUid"]="synthetic-root";account["folderUid"]=rootFolder?"synthetic-root":"synthetic-folder";
  doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Proton"},{"provider","proton-drive"},{"settings",account},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};
  QVERIFY(service.save(doc,{}));int uploads=0,inspections=0;JsonReply finish;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++uploads;reply({"test_unsupported","S3-only tester",{}});});
  fixture.setup.setProviderSetup([&](const QString& kind,const Destination& destination,const QJsonObject& input,JsonReply reply,auto){
   QCOMPARE(kind,QString("testDestination"));QCOMPARE(destination.settings,account);QVERIFY(input.isEmpty());++inspections;finish=std::move(reply);
  });
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* test=window.findChild<QPushButton*>("testDestination");QVERIFY(test->isVisible());test->click();
  PUTGD_TRY_COMPARE(inspections,1);QVERIFY(!test->isEnabled());
  auto* status=window.findChild<QLabel*>("destinationStatus");QVERIFY(status->text().contains("account and selected folder"));
  finish({},{{"testScope","account-folder"}});
  PUTGD_TRY_VERIFY(status->text().contains("Account and selected folder are accessible"));QVERIFY(!status->text().contains("upload, public link"));QCOMPARE(uploads,0);
  if(editAfterTest){auto* folder=window.findChild<QComboBox*>("protonFolder");folder->addItem("Another folder","synthetic-other");folder->setCurrentIndex(folder->count()-1);PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString(),QString("synthetic-other"));}
  bool warned=false;QTimer dismiss;connect(&dismiss,&QTimer::timeout,this,[&]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){warned=true;box->button(QMessageBox::Yes)->click();}});dismiss.start(10);
  window.findChild<QPushButton*>("makeDefaultDestination")->click();
  PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("synthetic-proton"));
  QCOMPARE(warned,editAfterTest);QCOMPARE(service.snapshot().actions["region"].profileId,QString("synthetic-proton"));
 }
 void protonConnectionTestPropagatesProviderErrors_data(){
  QTest::addColumn<QString>("expected");
  for(const auto& code:QStringList{"proton_setup_required","proton_account_changed","proton_folder_unavailable","authentication_required"})QTest::newRow(qPrintable(code))<<code;
 }
 void protonConnectionTestPropagatesProviderErrors(){
  QFETCH(QString,expected);
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=configuration(dir.path(),true);auto account=ProtonProvider::defaults();account["accountRootUid"]="synthetic-root";account["folderUid"]="synthetic-folder";
  doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Proton"},{"provider","proton-drive"},{"settings",account},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};QVERIFY(service.save(doc,{}));
  int uploads=0,inspections=0;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++uploads;reply({});});
  fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply reply,auto){++inspections;QCOMPARE(kind,QString("testDestination"));reply({expected,"Synthetic provider failure",{}},{});});
  QString id;fixture.setup.start("testDestination","synthetic-proton",service.revision(),{},[&](Error error,QJsonObject data){QVERIFY(!error);id=data["setupId"].toString();});
  QJsonObject job;auto inspect=[&]{fixture.setup.inspect(id,[&](Error error,QJsonObject data){QVERIFY(!error);job=data;});return job["state"]=="failed";};
  PUTGD_TRY_VERIFY(inspect());QCOMPARE(job["error"].toObject()["code"].toString(),expected);QCOMPARE(uploads,0);QCOMPARE(inspections,1);
 }

 void destinationsUseFriendlyEnumsAndKeepCredentialsOutOfConfigAndCheckpoint(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(dir.filePath("config.json"),[&](auto id,auto settings){return id=="s3"?s3.validateSettings(settings):proton.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();QVERIFY(!window.findChild<QLineEdit*>("destinationId")->isVisible());auto* acl=window.findChild<QComboBox*>("s3AclMode");QCOMPARE(acl->currentData().toString(),QString("public-read"));QVERIFY(!acl->currentText().contains("public-read"));window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-never-export-me");window.findChild<QLineEdit*>("credentialAccessKey")->setText("synthetic-access");const auto encoded=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!encoded.contains("synthetic-never-export-me"));QVERIFY(!encoded.contains("synthetic-access"));QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-never-export-me"));
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["provider"].toString(),QString("proton-drive"));window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->setValue(321.123);QMetaObject::invokeMethod(window.findChild<QDoubleSpinBox*>("protonRequestTimeout"),"editingFinished");PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["requestTimeoutMs"].toInt(),321123);provider->setCurrentIndex(provider->findData("s3"));QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-never-export-me"));window.settings()->clearCredentialInputs();QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty());
 }
 void optionalDefaultsStayAbsentAndDoNotBlockDestinationSetup(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto naming=doc["naming"].toObject();for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"})naming.remove(key);doc["naming"]=naming;auto limits=doc["limits"].toObject();limits.remove("pendingImageBytes");doc["limits"]=limits;auto preferences=doc["preferences"].toObject();preferences.remove("uploadClipboardBehavior");doc["preferences"]=preferences;QVERIFY(service.save(doc,{}));int tests=0;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++tests;reply({});});MainWindow window(fixture.session);window.settings();auto* pending=window.findChild<QLineEdit*>("pendingImageBytes");QVERIFY(pending);QVERIFY(!pending->property("defaultText").toString().isEmpty());PUTGD_TRY_COMPARE(pending->text(),pending->property("defaultText").toString());window.findChild<QCheckBox*>("notifications")->setChecked(false);PUTGD_TRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QVERIFY(!service.document()["naming"].toObject().contains("fileTemplate"));QVERIFY(!service.document()["limits"].toObject().contains("pendingImageBytes"));QVERIFY(!service.document()["preferences"].toObject().contains("uploadClipboardBehavior"));window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_COMPARE(tests,1);
 }
 void finishedGeneralEditSavesAfterDelayedDestinationSetupCompletes(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));std::function<void(Error)> complete;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){complete=reply;});MainWindow window(fixture.session);window.settings();window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(complete));window.findChild<QCheckBox*>("notifications")->setChecked(false);putgd::test::settle(550);PUTGD_TRY_VERIFY(service.document()["preferences"].toObject()["notifications"].toBool());complete({});PUTGD_TRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());
 }
 void startupUnlockRefreshesTheOpenDestination(){
  if(qEnvironmentVariable("PUTGD_ISOLATED_KEYCHAIN_TEST")!="1")QSKIP("Requires its dedicated private-bus invocation");
  qputenv("XDG_CURRENT_DESKTOP","KDE");qputenv("KDE_SESSION_VERSION","4");
  StartupWallet wallet;wallet.stored=QJsonDocument(QJsonObject{{"accessKey","AKIA-synthetic-startup"},{"secretKey","synthetic-startup-secret"}}).toJson(QJsonDocument::Compact);
  auto bus=QDBusConnection::sessionBus();QVERIFY(bus.registerService("org.kde.kwalletd"));
  QVERIFY(bus.registerObject("/modules/kwalletd",&wallet,QDBusConnection::ExportAllSlots));
  auto unregister=qScopeGuard([&]{bus.unregisterObject("/modules/kwalletd");bus.unregisterService("org.kde.kwalletd");});
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd-synthetic-startup"}};profiles[0]=profile;doc["profiles"]=profiles;
  auto preferences=doc["preferences"].toObject();preferences["unlockCredentialsAtStartup"]=true;doc["preferences"]=preferences;
  SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));
  UiSessionFixture fixture(service,secrets);
  // As in the resident's main(): a setup job's change invalidates the interface.
  QObject::connect(&fixture.setup,&SetupService::changed,&fixture.session,&UiSession::invalidate);
  MainWindow window(fixture.session);window.settings();window.navigate("destinations");window.resize(1440,960);window.show();
  auto* hint=window.findChild<QLabel*>("storedCredentialHint");auto* unlock=window.findChild<QPushButton*>("unlockCredentials");QVERIFY(hint&&unlock);
  PUTGD_TRY_COMPARE(hint->text(),QString("Locked in the system keychain"));PUTGD_TRY_VERIFY(!unlock->isHidden());
  // Opening the page asked for the hint only; it never reads the keychain.
  QCOMPARE(wallet.reads,0);
  // The resident's startup read, started after the page opened.
  QCOMPARE(fixture.setup.unlockSavedCredentials().size(),1);
  PUTGD_TRY_VERIFY(fixture.setup.savedCredentialsSettled());QVERIFY(wallet.reads>=1);
  PUTGD_TRY_VERIFY(hint->text().startsWith(QString::fromUtf8("AKIA…")));
  QVERIFY(hint->text().contains("In the system keychain"));QVERIFY(!hint->text().contains("AKIA-synthetic-startup"));
  PUTGD_TRY_VERIFY(unlock->isHidden());
  // Only the four-character prefix leaves the resident; the view state keeps neither key.
  const auto state=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!state.contains("AKIA-synthetic-startup"));QVERIFY(!state.contains("synthetic-startup-secret"));
 }
 void storedCredentialHintsNeverExportFullKeysOrOverwriteInputs(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));secrets.setSession("development-s3",{"AKIA-synthetic-full-access","synthetic-full-secret","synthetic-token"});UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* hint=window.findChild<QLabel*>("storedCredentialHint");PUTGD_TRY_VERIFY(hint->text().contains("AKIA"));QVERIFY(hint->text().contains("Secret key stored"));auto* access=window.findChild<QLineEdit*>("credentialAccessKey");QVERIFY(access->text().isEmpty());// The status row identifies the key; the field only invites a replacement.
QVERIFY(!access->placeholderText().isEmpty());QVERIFY(!access->placeholderText().contains("AKIA"));QVERIFY(hint->text().contains("For this session"));QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->echoMode(),QLineEdit::Password);const auto state=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!state.contains("AKIA"));QVERIFY(!state.contains("synthetic-full-secret"));bool done=false;QJsonObject reply;fixture.session.request("setup.credentialHint",{{"profileId","development-s3"},{"expectedRevision",service.revision()}},this,[&](Error error,QJsonObject value){QVERIFY(!error);reply=value;done=true;});PUTGD_TRY_VERIFY(done);const auto encoded=QJsonDocument(reply).toJson();QVERIFY(!encoded.contains("synthetic-full-access"));QVERIFY(!encoded.contains("synthetic-full-secret"));QVERIFY(!encoded.contains("synthetic-token"));
 }
 void connectionTestShowsItsResultOnTheDestinationPage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto second=profiles[0].toObject();
  second["id"]="second";second["label"]="Second destination";profiles.append(second);document["profiles"]=profiles;
  QVERIFY(service.save(document,{}));CredentialStore secrets;std::function<void(Error)> complete;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){complete=reply;});MainWindow window(fixture.session);
  window.navigate("destinations");window.show();QLabel* status=nullptr;
  for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="Destination status")status=label;
  QVERIFY(status);auto* test=window.findChild<QPushButton*>("testDestination");test->click();PUTGD_TRY_VERIFY(bool(complete));
  complete({});PUTGD_TRY_VERIFY(test->isEnabled());PUTGD_TRY_VERIFY(status->text().contains("Connection successful"));
  QVERIFY(status->isVisible());QCOMPARE(status->property("feedbackTone").toString(),QString("success"));
  auto* destinations=window.findChild<QListWidget*>("destinationList");destinations->setCurrentRow(1);QVERIFY(status->text().isEmpty());destinations->setCurrentRow(0);
  complete={};test->click();PUTGD_TRY_VERIFY(bool(complete));QVERIFY(status->text().contains("Testing upload"));
  complete({"s3_permission_denied","S3 denied the test upload. Check IAM and bucket policy permissions.",{}});
  PUTGD_TRY_VERIFY(test->isEnabled());PUTGD_TRY_VERIFY(status->text().contains("IAM and bucket policy"));
  QCOMPARE(status->property("feedbackTone").toString(),QString("error"));
 }
 void savedSecretCanBeShownHiddenAndNeverRetainedAsAnInput(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();
  profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;
  auto second=profile;second["id"]="second";profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  secrets.setSession("development-s3",{"synthetic-access","synthetic-saved-secret",{}});
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");QPushButton* show=nullptr;
  for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Show secret access key")show=button;
  QVERIFY(show);QVERIFY(secret->text().isEmpty());show->click();
  PUTGD_TRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));QCOMPARE(secret->echoMode(),QLineEdit::Normal);
  QVERIFY(secret->isReadOnly());QCOMPARE(show->text(),QString("Hide"));
  QVERIFY(!window.settings()->hasCredentialInputs());
  QVERIFY(window.settings()->autosaveText().isEmpty());
  QVERIFY(!QJsonDocument(window.saveViewState()).toJson().contains("synthetic-saved-secret"));
  QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains("synthetic-saved-secret"));
  QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-saved-secret"));
  show->click();QVERIFY(secret->text().isEmpty());QVERIFY(!secret->isReadOnly());QCOMPARE(secret->echoMode(),QLineEdit::Password);
  secret->setFocus();QTest::keyClicks(secret,"x");show->click();
  QTest::keyClick(secret,Qt::Key_Z,Qt::ControlModifier);QTest::keyClick(secret,Qt::Key_Z,Qt::ControlModifier);
  QVERIFY(secret->text().isEmpty());show->click();
  secret->setText("synthetic-typed-secret");show->click();QCOMPARE(secret->echoMode(),QLineEdit::Normal);
  show->click();QCOMPARE(secret->text(),QString("synthetic-typed-secret"));QCOMPARE(secret->echoMode(),QLineEdit::Password);
  secret->clear();show->click();PUTGD_TRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(1);
  QVERIFY(secret->text().isEmpty());QCOMPARE(show->text(),QString("Show"));list->setCurrentRow(0);QVERIFY(secret->text().isEmpty());
  fixture.client.replyDelayMs["setup.revealCredentialSecret"]=150;
  show->click();show->click();putgd::test::settle(250);PUTGD_TRY_VERIFY(secret->text().isEmpty());PUTGD_TRY_COMPARE(secret->echoMode(),QLineEdit::Password);
  show->click();list->setCurrentRow(1);list->setCurrentRow(0);putgd::test::settle(250);PUTGD_TRY_VERIFY(secret->text().isEmpty());
  show->click();PUTGD_TRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));window.settings()->clearCredentialInputs();
  QVERIFY(secret->text().isEmpty());QCOMPARE(show->text(),QString("Show"));QVERIFY(!show->isChecked());QVERIFY(!secret->isReadOnly());
  show->click();window.navigate("settings");putgd::test::settle(250);PUTGD_TRY_VERIFY(secret->text().isEmpty());window.navigate("destinations");
  secrets.forget("development-s3",profile["credentials"].toObject());show->click();
  PUTGD_TRY_COMPARE(statusCode(window),QString("credential_setup_required"));
  QVERIFY(secret->text().isEmpty());QVERIFY(!show->isChecked());QVERIFY(!secret->isReadOnly());
 }
 void uiStateStrictlyRejectsUnexpectedAndSecretFields(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto state=QJsonObject{{"schemaVersion",1},{"settings",window.settings()->saveViewState()},{"main",window.saveViewState()}};QVERIFY(!validateUiState(state));auto main=state["main"].toObject();main["secretKey"]="synthetic-secret";state["main"]=main;auto error=validateUiState(state);QVERIFY(error);QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-secret"));
 }
 void acceptedSetupIdSurvivesAnUncertainInspectionReply(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));int starts=0;std::function<void(Error)> finish;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});fixture.client.failOnce["setup.inspect"]={"client_unavailable","Synthetic lost inspection reply",{}};SettingsWindow window(fixture.session);window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(statusCode(window)=="client_unavailable");const auto saved=window.saveViewState();QCOMPARE(saved["setupJobs"].toArray().size(),1);SettingsWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));QVERIFY(finish);finish({});PUTGD_TRY_VERIFY(reopened.findChild<QLabel*>("settingsStatus")->text().contains("Connection successful"));QCOMPARE(starts,1);
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
  PUTGD_TRY_COMPARE(status->property("errorCode").toString(),QString("client_unavailable"));
  QCOMPARE(starts,1);QVERIFY(finish);QVERIFY(!test->isEnabled());
  window.findChild<QLineEdit*>("s3Bucket")->setText("unsaved-bucket");
  window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-unsubmitted-secret");
  finish({});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(status->text().contains("original destination settings"));QVERIFY(test->isEnabled());
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
  for(int i=0;i<65;++i){const auto id=start();QVERIFY(!id.isEmpty());if(i==0)expired=id;PUTGD_TRY_VERIFY(completed(id));}
  Error missing;fixture.setup.inspect(expired,[&](Error error,QJsonObject){missing=error;});QCOMPARE(missing.code,QString("setup_not_found"));
  hold=true;const auto pending=start();QVERIFY(!pending.isEmpty());QVERIFY(finish);QCOMPARE(starts,66);
  SettingsWindow window(fixture.session);auto state=window.saveViewState();
  state["setupJobs"]=QJsonArray{QJsonObject{{"setupId",expired},{"kind","testDestination"},{"profileId","development-s3"}},QJsonObject{{"setupId",pending},{"kind","testDestination"},{"profileId","development-s3"}}};
  QVERIFY(window.restoreViewState(state));
  auto* test=window.findChild<QPushButton*>("testDestination");
  PUTGD_TRY_COMPARE(statusCode(window),QString("setup_not_found"));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QCOMPARE(window.saveViewState()["setupJobs"].toArray().first().toObject()["setupId"].toString(),pending);
  QVERIFY(!test->isEnabled());
  finish({});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
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
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QVERIFY(!window.findChild<QPushButton*>("testDestination")->isEnabled());
  // Controls are disabled one by one, not their sections: labels stay enabled.
  QVERIFY(!window.findChild<QLineEdit*>("s3Bucket")->isEnabled());QVERIFY(!window.findChild<QPushButton*>("storeCredentials")->isEnabled());QVERIFY(window.findChild<QWidget*>("destinationEditor")->isEnabled());QVERIFY(window.findChild<QLabel*>("storedCredentialHint")->isEnabled());
  finish["second-s3"]({});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(window.findChild<QPushButton*>("testDestination")->isEnabled());QCOMPARE(starts,2);
  // Afterwards each control returns to its provider's state.
  QVERIFY(window.findChild<QLineEdit*>("s3Bucket")->isEnabled());QVERIFY(window.findChild<QComboBox*>("s3AclMode")->isEnabled());QVERIFY(!window.findChild<QComboBox*>("b2LinkMode")->isEnabled());
 }
 void storedCredentialsShowOneStatusRowAboveSpacedActions(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));secrets.setSession("development-s3",{"AKIA-synthetic-full-access","synthetic-full-secret",{}});
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("destinations");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* hint=window.findChild<QLabel*>("storedCredentialHint");PUTGD_TRY_VERIFY(hint->isVisible()&&hint->text().contains("AKIA"));
  // The identification appears once: in the status row, under the heading and above the fields.
  auto* editor=window.findChild<QWidget*>("credentialsEditor");int mentions=0;for(auto* label:editor->findChildren<QLabel*>())if(label->isVisible()&&label->text().contains("AKIA"))++mentions;QCOMPARE(mentions,1);
  for(auto* field:editor->findChildren<QLineEdit*>())QVERIFY2(!field->placeholderText().contains("AKIA")&&!field->placeholderText().contains("Stored"),qPrintable(field->objectName()));
  auto* access=window.findChild<QLineEdit*>("credentialAccessKey");QVERIFY(hint->mapTo(editor,QPoint()).y()<access->mapTo(editor,QPoint()).y());
  // Session credentials need only the primary store action; nothing is locked.
  auto* store=window.findChild<QPushButton*>("storeCredentials");auto* unlock=window.findChild<QPushButton*>("unlockCredentials");QVERIFY(store&&unlock);QVERIFY(unlock->isHidden());QCOMPARE(store->property("visualRole").toString(),QString("primary"));QCOMPARE(store->width(),store->sizeHint().width());QCOMPARE(store->mapTo(editor,QPoint(store->width(),0)).x(),access->mapTo(editor,QPoint(access->width(),0)).x());
 }
 void bucketHintAndCredentialGuidanceFollowTheEditors(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* folder=window.findChild<QLineEdit*>("s3ObjectPrefix");auto* hint=window.findChild<QLabel*>("s3BucketHint");QVERIFY(bucket&&folder&&hint);bucket->setText("synthetic-bucket");folder->setText("captures/");QCOMPARE(hint->text(),QString("Uploads go to s3://synthetic-bucket/captures/"));bucket->clear();QVERIFY(hint->text().startsWith("Required"));
  auto* mode=window.findChild<QComboBox*>("credentialMode");auto* button=window.findChild<QPushButton*>("storeCredentials");for(const auto& value:{"keychain","session"}){mode->setCurrentIndex(mode->findData(value));QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->toolTip().contains(button->text()));QVERIFY(mode->accessibleDescription().contains(button->text()));auto* access=window.findChild<QLineEdit*>("credentialAccessKey");access->setText("synthetic-access");QVERIFY(window.settings()->autosaveText().contains(button->text()));access->clear();}
 }
 void missingCredentialsOfferFocusedNavigation_data(){QTest::addColumn<int>("pixels");QTest::newRow("text100")<<14;QTest::newRow("text150")<<21;}
 void missingCredentialsOfferFocusedNavigation(){
  Fonts::load();Ui::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("destinations");window.show();window.activateWindow();auto* action=window.findChild<QPushButton*>("addDestinationCredentials");auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");QVERIFY(action&&scroll&&access&&secret);PUTGD_TRY_VERIFY(action->isVisible());QVERIFY(window.findChild<QLabel*>("destinationProviderState")->text().contains("Needs credentials"));const auto revision=service.revision();int mutations=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="settings.save"||method=="setup.storeCredentials")++mutations;};
  for(auto* target:{access,secret}){if(target==secret)access->setText("synthetic-navigation-id");scroll->verticalScrollBar()->setValue(0);action->setFocus();QTest::keyClick(action,Qt::Key_Space);PUTGD_TRY_VERIFY(target->hasFocus());PUTGD_TRY_VERIFY2(scroll->viewport()->rect().contains(QRect(target->mapTo(scroll->viewport(),QPoint()),target->size())),qPrintable(QString("field %1 at %2,%3 %4x%5, viewport %6x%7, scroll %8/%9").arg(target->objectName()).arg(target->mapTo(scroll->viewport(),QPoint()).x()).arg(target->mapTo(scroll->viewport(),QPoint()).y()).arg(target->width()).arg(target->height()).arg(scroll->viewport()->width()).arg(scroll->viewport()->height()).arg(scroll->verticalScrollBar()->value()).arg(scroll->verticalScrollBar()->maximum())));QVERIFY(scroll->verticalScrollBar()->value()>0);QCOMPARE(service.revision(),revision);QCOMPARE(mutations,0);}
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(1);PUTGD_TRY_VERIFY(action->isHidden());
 }
 void credentialStorageNoticeKeepsItsMeasuredHeight_data(){QTest::addColumn<int>("pixels");QTest::newRow("text100")<<14;QTest::newRow("text150")<<21;}
 void credentialStorageNoticeKeepsItsMeasuredHeight(){
  Fonts::load();Ui::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* mode=window.findChild<QComboBox*>("credentialMode");auto* notice=window.findChild<QLabel*>("credentialStorageNotice");QVERIFY(mode&&notice);PUTGD_TRY_VERIFY(fixture.session.ready());QVERIFY(notice->isHidden());
  for(const auto size:{QSize(1440,1000),QSize(1060,680),QSize(1440,1000)}){window.resize(size);mode->setCurrentIndex(mode->currentData()=="session"?mode->findData("keychain"):mode->findData("session"));PUTGD_TRY_VERIFY(notice->isVisible());putgd::test::settle(30);const QRect control(mode->mapTo(&window,QPoint()),mode->size()),message(notice->mapTo(&window,QPoint()),notice->size());QVERIFY(!control.intersects(message));QVERIFY(message.top()>control.bottom());QVERIFY(notice->height()>=notice->heightForWidth(notice->width()));auto* value=notice->parentWidget();QVERIFY(value->rect().contains(notice->geometry()));QVERIFY(value->rect().contains(QRect(mode->mapTo(value,QPoint()),mode->size())));}
 }

};
QTEST_MAIN(SettingsGui)
#include "test_settings_credentials.moc"
