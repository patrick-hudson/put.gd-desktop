#include <QJsonArray>
#include "SettingsGuiFixture.h"
#include "ui/EncryptedLinksPanel.h"
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
};
QTEST_MAIN(SettingsGui)
#include "test_settings_credentials.moc"
