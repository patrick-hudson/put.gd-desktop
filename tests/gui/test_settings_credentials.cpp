#include <QJsonArray>
#include "SettingsGuiFixture.h"
#include "ui/EncryptedLinksPanel.h"
#include "app/DestinationStatus.h"
#include "core/DestinationCheck.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
// Secrets typed, stored or revealed in Settings never reach the configuration,
// saved view state, a credential hint reply or another destination.
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 void destinationsUseFriendlyEnumsAndKeepCredentialsOutOfConfigAndCheckpoint(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-never-export-me");window.findChild<QLineEdit*>("credentialAccessKey")->setText("synthetic-access");
  const auto encoded=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!encoded.contains("synthetic-never-export-me"));QVERIFY(!encoded.contains("synthetic-access"));QVERIFY(!QJsonDocument(window.saveViewState()).toJson().contains("synthetic-never-export-me"));
  PUTGD_VERIFY_STEADY(!QJsonDocument(service.document()).toJson().contains("synthetic-never-export-me"),50);
 }
 void storedCredentialHintsNeverExportFullKeysOrOverwriteInputs(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));secrets.setSession("development-s3",{"AKIA-synthetic-full-access","synthetic-full-secret","synthetic-token"});UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* hint=window.findChild<QLabel*>("storedCredentialHint");PUTGD_TRY_VERIFY(hint->text().contains("AKIA"));
  QVERIFY(!hint->text().contains("synthetic-full-access"));QVERIFY(window.findChild<QLineEdit*>("credentialAccessKey")->text().isEmpty());QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty());
  const auto state=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!state.contains("AKIA"));QVERIFY(!state.contains("synthetic-full-secret"));
  bool done=false;QJsonObject reply;fixture.session.request("setup.credentialHint",{{"profileId","development-s3"},{"expectedRevision",service.revision()}},this,[&](Error error,QJsonObject value){QVERIFY(!error);reply=value;done=true;});PUTGD_TRY_VERIFY(done);const auto encoded=QJsonDocument(reply).toJson();QVERIFY(!encoded.contains("synthetic-full-access"));QVERIFY(!encoded.contains("synthetic-full-secret"));QVERIFY(!encoded.contains("synthetic-token"));
 }
 // A revealed secret stays out of saved state and is never left showing for another destination.
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
  PUTGD_TRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));
  QVERIFY(!window.settings()->hasCredentialInputs());
  QVERIFY(!QJsonDocument(window.saveViewState()).toJson().contains("synthetic-saved-secret"));
  QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains("synthetic-saved-secret"));
  QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-saved-secret"));
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(1);QVERIFY(secret->text().isEmpty());list->setCurrentRow(0);QVERIFY(secret->text().isEmpty());
 }
 void uiStateStrictlyRejectsUnexpectedAndSecretFields(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto state=QJsonObject{{"schemaVersion",1},{"settings",window.settings()->saveViewState()},{"main",window.saveViewState()}};QVERIFY(!validateUiState(state));
  // A Test still running when Settings closes carries the fingerprint it started with (#1692).
  {auto settings=state["settings"].toObject();settings["setupJobs"]=QJsonArray{QJsonObject{{"setupId","job"},{"kind","testDestination"},{"profileId","p"},{"revision","1"},{"fingerprint",QString(64,'a')}}};auto withJob=state;withJob["settings"]=settings;QVERIFY(!validateUiState(withJob));
   settings["setupJobs"]=QJsonArray{QJsonObject{{"setupId","job"},{"kind","testDestination"},{"profileId","p"},{"fingerprint","https://example.invalid/bucket"}}};withJob["settings"]=settings;QVERIFY(validateUiState(withJob));
   settings["setupJobs"]=QJsonArray{QJsonObject{{"setupId","job"},{"kind","testDestination"},{"profileId","p"},{"fingerprint",QString(64,'a')+"\n"}}};withJob["settings"]=settings;QVERIFY(validateUiState(withJob));}
  // #2079: a destination's Encrypted links choice is part of its profile in the checkpoint; any other shape isn't.
  {auto settings=state["settings"].toObject();auto draft=settings["draft"].toObject();
   const auto profileWith=[](const QJsonValue& encrypt){return QJsonObject{{"id","p"},{"label","P"},{"provider","s3"},{"settings",QJsonObject{}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}},{"encrypt",encrypt}};};
   auto withEncrypt=state;draft["profiles"]=QJsonArray{profileWith(QJsonObject{{"mode","passphrase"},{"passphraseCredentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}})};settings["draft"]=draft;withEncrypt["settings"]=settings;
   QVERIFY2(!validateUiState(withEncrypt),qPrintable(validateUiState(withEncrypt).message));
   draft["profiles"]=QJsonArray{profileWith(QJsonObject{{"mode","on"},{"passphrase","not here"}})};settings["draft"]=draft;withEncrypt["settings"]=settings;QVERIFY(validateUiState(withEncrypt));
   draft["profiles"]=QJsonArray{profileWith(QJsonObject{{"mode","always"}})};settings["draft"]=draft;withEncrypt["settings"]=settings;QVERIFY(validateUiState(withEncrypt));}
  auto main=state["main"].toObject();main["secretKey"]="synthetic-secret";state["main"]=main;auto error=validateUiState(state);QVERIFY(error);QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-secret"));
 }
 // #2079: a profile whose Encrypted links mode isn't edited comes back exactly as saved, fields
 // the settings allow (a kept passphrase reference) included, so it never reads as an edit; a
 // typed passphrase goes when Settings clears its typed secrets.
 void encryptedLinksKeepAnUneditedProfile(){
  QWidget host;auto* form=new QFormLayout(&host);EncryptedLinksPanel panel(&host);
  panel.build(form,[](QFormLayout* rows,const QString& label,QWidget* control,const QString&){auto* row=new QWidget;auto* line=new QHBoxLayout(row);line->addWidget(control);rows->addRow(label,row);return row;});
  const QJsonObject reference{{"mode","session"},{"reference",QJsonValue::Null}};
  for(const auto* mode:{"off","on","passphrase"}){
   const QJsonObject profile{{"id","p"},{"provider","s3"},{"credentials",reference},{"encrypt",QJsonObject{{"mode",mode},{"passphraseCredentials",reference}}}};
   panel.load(profile,{{"encrypt",QJsonObject{{"state","passed"}}}},false,false);QCOMPARE(panel.apply(profile),profile);
  }
  auto* typed=host.findChild<QLineEdit*>("encryptedLinksPassphrase");QVERIFY(typed);typed->setText("typed but never saved");panel.clearPassphrase();QVERIFY(typed->text().isEmpty());
  // Closing Settings clears a passphrase typed and never saved, with the other typed secrets.
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* field=window.findChild<QLineEdit*>("encryptedLinksPassphrase");QVERIFY(field);field->setText("typed but never saved");
  window.settings()->close();QVERIFY(field->text().isEmpty());
 }
 // #2103: ui.encryptedLinks answers a mode change only once the file has it, so an agent's next
 // call (setup.storeEncryptPassphrase reads the saved mode) never races the save. Two Capture
 // changes made from outside, as a second client makes them, come first; a conflict answers
 // settings_not_saved, never ok.
 void encryptedLinksModeAnswersOnlyOnceSaved(){
  QFETCH(bool,settle);QFETCH(bool,conflict);
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);DestinationStatus checks(dir.filePath("destination-status.json"),service);fixture.server.setDestinationStatus(&checks);
  const auto saved=[&]{return service.document()["profiles"].toArray().first().toObject();};
  checks.encryptChecked(saved()["id"].toString(),destinationcheck::fingerprint(saved()),true,{});fixture.session.invalidate();
  MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* settings=window.settings();
  // The reply may come up to 8 s later; the shared state outlives a wait that times out.
  const auto drive=[&](const QJsonObject& command,Error* error,QJsonObject* state){struct Answer{bool done=false;Error error;QJsonObject state;};auto answer=std::make_shared<Answer>();settings->encryptedLinksCommand(command,[answer](Error e,QJsonObject s){answer->error=e;answer->state=s;answer->done=true;});if(!waitFor([&]{return answer->done;},10000))return false;*error=answer->error;*state=answer->state;return true;};
  const auto fileMode=[&]{return saved()["encrypt"].toObject()["mode"].toString("off");};
  const auto patch=[&](const QString& publication){bool done=false;Error error;fixture.server.dispatch({{"schemaVersion",1},{"requestId","outside-"+publication},{"session",fixture.server.socketPath()},{"method","settings.patch"},{"params",QJsonObject{{"patch",QJsonObject{{"captureDefaults",QJsonObject{{"publicationMode",publication}}}}},{"expectedRevision",service.revision()}}}},[&](Error e,QJsonObject){error=e;done=true;});return waitFor([&]{return done;})&&!error;};
  Error error;QJsonObject state;
  QVERIFY(waitFor([&]{return drive({{"action","inspect"}},&error,&state)&&state["available"].toBool();}));
  QVERIFY(drive({{"action","mode"},{"mode","on"}},&error,&state));QVERIFY2(!error,qPrintable(error.message));QCOMPARE(fileMode(),QStringLiteral("on"));QVERIFY(state["saved"].toBool());
  QVERIFY(patch("private"));QVERIFY(patch("default"));
  if(settle)QVERIFY(waitFor([&]{return settings->saveViewState()["baseRevision"].toString()==service.revision();}));
  // The resident refuses the next save as a conflict, as it does when the file changed under it.
  if(conflict)fixture.client.failOnce.insert("settings.save",{"config_conflict","Settings changed elsewhere.",{}});
  QVERIFY(drive({{"action","mode"},{"mode","passphrase"}},&error,&state));
  if(conflict){QCOMPARE(error.code,QStringLiteral("settings_not_saved"));QCOMPARE(fileMode(),QStringLiteral("on"));}
  else{QVERIFY2(!error,qPrintable(error.message));QCOMPARE(fileMode(),QStringLiteral("passphrase"));QVERIFY(state["saved"].toBool());}
  QCOMPARE(service.document()["captureDefaults"].toObject()["publicationMode"].toString(),QStringLiteral("default"));
 }
 // #2103: a window that hasn't yet read a change made elsewhere never answers from its stale copy:
 // with Off saved by another client, choosing the passphrase mode the window still shows isn't ok
 // until the resident confirms it.
 void encryptedLinksModeIsConfirmedByTheResident(){
  QFETCH(bool,late);
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);DestinationStatus checks(dir.filePath("destination-status.json"),service);fixture.server.setDestinationStatus(&checks);
  const auto saved=[&]{return service.document()["profiles"].toArray().first().toObject();};
  checks.encryptChecked(saved()["id"].toString(),destinationcheck::fingerprint(saved()),true,{});fixture.session.invalidate();
  MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* settings=window.settings();
  struct Answer{bool done=false;Error error;};
  const auto drive=[&](const QString& mode){auto answer=std::make_shared<Answer>();settings->encryptedLinksCommand({{"action","mode"},{"mode",mode}},[answer](Error e,QJsonObject){answer->error=e;answer->done=true;});return answer;};
  const auto fileMode=[&]{return saved()["encrypt"].toObject()["mode"].toString("off");};
  QVERIFY(waitFor([&]{auto inspect=std::make_shared<QJsonObject>();settings->encryptedLinksCommand({{"action","inspect"}},[inspect](Error,QJsonObject s){*inspect=s;});return waitFor([&]{return !inspect->isEmpty();})&&(*inspect)["available"].toBool();}));
  {auto answer=drive("passphrase");QVERIFY(waitFor([&]{return answer->done;},10000));QVERIFY2(!answer->error,qPrintable(answer->error.message));QCOMPARE(fileMode(),QStringLiteral("passphrase"));}
  // Another client turns it off; the window doesn't read that until the held snapshots go, or
  // (late) a snapshot the resident read before the change answers after it. The resident tells a
  // window about a change with refreshSnapshot (UiApplicationController), which joins one in flight.
  QObject::disconnect(&service,&SettingsService::statusChanged,&fixture.session,&UiSession::invalidate);
  QObject::connect(&service,&SettingsService::statusChanged,&fixture.session,[&]{fixture.session.refreshSnapshot();});
  if(late){bool read=false;fixture.client.replyDelayMs.insert("ui.snapshot",300);fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="ui.snapshot")read=true;};fixture.session.refreshSnapshot();const bool dispatched=waitFor([&]{return read;});fixture.client.beforeDispatch={};QVERIFY(dispatched);}
  else fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="ui.snapshot";};
  auto off=service.document();auto profiles=off["profiles"].toArray();auto profile=profiles.first().toObject();profile["encrypt"]=QJsonObject{{"mode","off"}};profiles[0]=profile;off["profiles"]=profiles;
  bool done=false;fixture.server.dispatch({{"schemaVersion",1},{"requestId","outside-off"},{"session",fixture.server.socketPath()},{"method","settings.save"},{"params",QJsonObject{{"document",off},{"expectedRevision",service.revision()}}}},[&](Error e,QJsonObject){QVERIFY(!e);done=true;});QVERIFY(waitFor([&]{return done;}));QCOMPARE(fileMode(),QStringLiteral("off"));
  auto answer=drive("passphrase");
  PUTGD_VERIFY_STEADY(!answer->done||answer->error,100);
  fixture.client.hold={};while(fixture.client.release()){}
  QVERIFY(waitFor([&]{return answer->done;},10000));
  if(!answer->error)QCOMPARE(fileMode(),QStringLiteral("passphrase"));else QCOMPARE(answer->error.code,QStringLiteral("settings_not_saved"));
 }
 void encryptedLinksModeIsConfirmedByTheResident_data(){QTest::addColumn<bool>("late");QTest::newRow("snapshot held")<<false;QTest::newRow("snapshot read before the change, answered after")<<true;}
 void encryptedLinksModeAnswersOnlyOnceSaved_data(){
  QTest::addColumn<bool>("settle");QTest::addColumn<bool>("conflict");
  QTest::newRow("right after the outside changes")<<false<<false;QTest::newRow("once the window has them")<<true<<false;QTest::newRow("refused as a conflict")<<true<<true;
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_credentials.moc"
