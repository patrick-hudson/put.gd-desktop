#include "SettingsGuiFixture.h"
// Destination edits, credentials and routes stay with the destination they belong to.
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 // Moving a destination keeps its unstored credential input and every action's route.
 void reorderingDestinationsKeepsSelectionEditsAndRouting(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  auto routing=[&]{auto doc=service.document();doc.remove("profiles");return doc;};const auto routes=routing();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");auto* down=window.findChild<QAction*>("moveDestinationDown");QVERIFY(list&&down);
  list->setCurrentRow(1);auto* access=window.findChild<QLineEdit*>("credentialAccessKey");access->setText("SYNTHETICACCESSKEY");
  down->trigger();
  QCOMPARE(list->currentRow(),2);QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("synthetic-alternate"));QCOMPARE(access->text(),QString("SYNTHETICACCESSKEY"));
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-third","synthetic-alternate"}));
  QCOMPARE(routing(),routes);QCOMPARE(service.snapshot().actions["clipboard"].profileId,QString("synthetic-third"));
 }
 void removingDestinationBeforeDebounceNeverAppliesItsEditToNextDestination(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles[0].toObject();second["id"]="second-destination";second["label"]="Second destination";auto fields=second["settings"].toObject();fields["bucket"]="second-bucket";second["settings"]=fields;profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();window.findChild<QLineEdit*>("s3Bucket")->setText("edited-first-bucket");QMetaObject::invokeMethod(window.findChild<QLineEdit*>("s3Bucket"),"editingFinished");QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();PUTGD_TRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray()[0].toObject(),second);
 }
 void customUploaderImportKeepsItsTokenOutOfSettingsAndCheckpoints(){
  const QByteArray token="PUTGD-SYNTHETIC-GUI-TOKEN-0123456789abcdef";
  QTemporaryDir dir;CredentialStore secrets;ProviderRegistry registry;
  registry.add("s3",std::make_shared<S3Provider>(secrets));registry.add("custom-http",std::make_shared<HttpUploaderProvider>(secrets));
  SettingsService service(dir.filePath("config.json"),[&](auto id,auto values){return registry.validate(id,values);},nullptr,[&](auto id,auto reference){return registry.validateCredentials(id,reference);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.setup.setNamedCredentials(http::namedCredentials);
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* provider=window.findChild<QComboBox*>("storageProvider");
  QVERIFY(provider->findData("custom-http")>=0);
  provider->setCurrentIndex(provider->findData("custom-http"));
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  PUTGD_TRY_COMPARE(saved()["provider"].toString(),QString("custom-http"));
  // A new one is saved with its defaults and says what it still needs.
  QCOMPARE(saved()["settings"].toObject(),http::defaults());
  auto* panel=window.findChild<CustomUploaderPanel*>("customUploaderEditor");QVERIFY(panel&&panel->isVisible());
  QVERIFY(!window.findChild<QLineEdit*>("credentialAccessKey")->isVisible());
  auto* mode=window.findChild<QComboBox*>("credentialMode");mode->setCurrentIndex(mode->findData("session"));emit mode->activated(mode->currentIndex());
  PUTGD_TRY_COMPARE(saved()["credentials"].toObject()["mode"].toString(),QString("session"));
  // The file repeats its token in the deletion URL, as XBackBone's does.
  const QByteArray file="{\"Version\":\"16.1.0\",\"Name\":\"Lab media\",\"DestinationType\":\"ImageUploader, FileUploader\",\"RequestMethod\":\"POST\",\"RequestURL\":\"https://media.example.test/upload\",\"Body\":\"MultipartFormData\",\"Arguments\":{\"file\":\"{filename}\",\"token\":\""+token+"\"},\"FileFormName\":\"upload\",\"URL\":\"{json:url}\",\"DeletionURL\":\"{json:url}/delete/"+token+"\"}";
  QVERIFY(!panel->importBytes(file));
  PUTGD_TRY_COMPARE(saved()["settings"].toObject()["requestUrl"].toString(),QString("https://media.example.test/upload"));
  const auto settings=saved()["settings"].toObject();
  QCOMPARE(settings["arguments"].toArray(),QJsonArray({QJsonObject{{"name","file"},{"secret",false},{"value","{filename}"}},QJsonObject{{"name","token"},{"secret",true}}}));
  QCOMPARE(settings["deletionUrl"].toString(),QString("{json:url}/delete/{secret:arguments/token}"));
  QCOMPARE(settings["accepts"].toArray(),QJsonArray({"image","file"}));
  // Held in the window, masked, and nowhere else.
  QCOMPARE(panel->typedSecrets().value("arguments/token"),QString::fromLatin1(token));
  auto* value=window.findChild<QLineEdit*>("uploaderFieldValue1");QVERIFY(value);
  QCOMPARE(value->echoMode(),QLineEdit::Password);
  QVERIFY(window.settings()->hasCredentialInputs());
  QFile config(dir.filePath("config.json"));QVERIFY(config.open(QIODevice::ReadOnly));QVERIFY(!config.readAll().contains(token));config.close();
  const auto checkpoint=window.settings()->saveViewState();
  QVERIFY2(!checkpoint.contains("stateError"),qPrintable(checkpoint["stateError"].toString()));
  QVERIFY(!QJsonDocument(checkpoint).toJson().contains(token));
  QVERIFY(!QJsonDocument(panel->state()).toJson().contains(token));
  QCOMPARE(panel->missingSecrets(),QStringList());
  // Stored for the session: the field empties and shows the value as stored.
  auto* store=window.findChild<QPushButton*>("storeCredentials");PUTGD_TRY_VERIFY(store->isEnabled());store->click();
  PUTGD_TRY_VERIFY(panel->typedSecrets().isEmpty());
  QVERIFY(value->text().isEmpty());
  const auto cached=secrets.cached("development-s3",saved()["credentials"].toObject());
  QVERIFY(cached);QCOMPARE(cached->values.value("arguments/token"),QString::fromLatin1(token));
  QVERIFY(!window.settings()->hasCredentialInputs());
  QVERIFY(config.open(QIODevice::ReadOnly));QVERIFY(!config.readAll().contains(token));config.close();
  QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains(token));
  // Plain HTTP beyond loopback is refused beside the address until the destination allows it.
  auto* url=window.findChild<QLineEdit*>("uploaderUrl");url->setText("http://media.example.test/upload");emit url->editingFinished();
  auto* problem=window.findChild<QLabel*>("uploaderUrlError");QVERIFY(problem);
  PUTGD_TRY_VERIFY(problem->isVisible()&&problem->text().contains("plain HTTP"));
  QCOMPARE(saved()["settings"].toObject()["requestUrl"].toString(),QString("https://media.example.test/upload"));
  auto* allow=window.findChild<QCheckBox*>("uploaderAllowHttp");QVERIFY(allow->isVisible());allow->click();
  PUTGD_TRY_COMPARE(saved()["settings"].toObject()["requestUrl"].toString(),QString("http://media.example.test/upload"));
  QCOMPARE(saved()["settings"].toObject()["allowInsecureHttp"].toBool(),true);
  // The deletion URL names this value, so it cannot stop being a secret: the change is
  // refused beside the form and the saved row stays as it was.
  auto* secret=window.findChild<QCheckBox*>("uploaderFieldSecret1");secret->click();
  auto* refused=window.findChild<QLabel*>("uploaderSecretsError");QVERIFY(refused);
  PUTGD_TRY_VERIFY(refused->isVisible()&&refused->text().contains("does not store"));
  QCOMPARE(saved()["settings"].toObject()["arguments"].toArray()[1].toObject(),QJsonObject({{"name","token"},{"secret",true}}));
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_destinations.moc"
