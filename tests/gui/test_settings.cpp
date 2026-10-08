#include "ui/SettingsWindow.h"
#include "ui/ShortcutEditor.h"
#include "fixtures/U2Fixtures.h"
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
#include "storage/s3/S3Provider.h"
using namespace putgd;using namespace putgd::test;
class SettingsGui:public QObject {
 Q_OBJECT

private slots:
 void simpleSetupStartsWithoutDevelopmentAndSavesCompleteDestination() {
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);int tests=0;
  SettingsService service(d.filePath("config.json"),[&](auto id,auto value){return id=="s3"?provider.validateSettings(value):Error{"provider_unavailable","Unknown provider",{}};});
  SettingsWindow window(service,secrets,[&](auto,auto,auto){++tests;});window.show();
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");QVERIFY2(advanced,"Setup needs an explicit Advanced settings expansion");QVERIFY(!advanced->isChecked());
  QVERIFY(!window.findChild<QListWidget*>("settingsNavigation")->isVisible());QVERIFY(service.revision().isEmpty());QVERIFY(!QFile::exists(service.path()));QVERIFY(service.document()["profiles"].toArray().isEmpty());for(const auto& action:Settings::actionNames())QVERIFY(service.snapshot().actions[action].profileId.isEmpty());QCOMPARE(tests,0);
  window.findChild<QPushButton*>("addS3Destination")->click();
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QVERIFY(bucket->isVisible());bucket->setText("synthetic-bucket");
  window.findChild<QLineEdit*>("s3Region")->setText("eu-west-1");QVERIFY(!window.findChild<QComboBox*>("credentialMode")->isVisible());QVERIFY(!window.findChild<QLineEdit*>("credentialSessionToken")->isVisible());QVERIFY(!window.findChild<QComboBox*>("destinationProfile")->isVisible());
  auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");auto* token=window.findChild<QLineEdit*>("credentialSessionToken");for(auto* field:{access,secret,token})QCOMPARE(field->echoMode(),QLineEdit::Password);access->setText("synthetic-access");secret->setText("synthetic-secret");
  QVERIFY(!window.findChild<QLineEdit*>("s3Endpoint")->isVisible());window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(!service.revision().isEmpty());
  const auto profile=service.document()["profiles"].toArray().first().toObject();const auto settings=profile["settings"].toObject();
  QCOMPARE(settings["endpoint"].toString(),QString("https://s3.eu-west-1.amazonaws.com"));QCOMPARE(settings["aclMode"].toString(),QString("omit"));QCOMPARE(settings["storageClass"].toString(),QString("STANDARD"));QVERIFY(settings["publicUrlBase"].isNull());
  for(const auto& action:Settings::actionNames())QCOMPARE(service.snapshot().actions[action].profileId,profile["id"].toString());
  QVERIFY(profile["id"].toString()!="development-s3");QCOMPARE(tests,0);const auto encoded=QJsonDocument(service.document()).toJson();QVERIFY(!encoded.contains("synthetic-access"));QVERIFY(!encoded.contains("synthetic-secret"));QVERIFY(!encoded.contains("synthetic-token"));QVERIFY(!secrets.cached(profile["id"].toString(),profile["credentials"].toObject()));
  advanced->setChecked(true);QVERIFY(window.findChild<QComboBox*>("credentialMode")->isVisible());QVERIFY(token->isVisible());window.findChild<QComboBox*>("credentialMode")->setCurrentText("session");token->setText("synthetic-token");advanced->setChecked(false);window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().first().toObject()["credentials"].toObject()["mode"].toString(),QString("session"));
  const auto sessionProfile=service.document()["profiles"].toArray().first().toObject();QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-token"));QVERIFY(!secrets.cached(sessionProfile["id"].toString(),sessionProfile["credentials"].toObject()));window.findChild<QPushButton*>("storeCredentials")->click();QVERIFY(secrets.cached(sessionProfile["id"].toString(),sessionProfile["credentials"].toObject()));QVERIFY(access->text().isEmpty());QVERIFY(secret->text().isEmpty());QVERIFY(token->text().isEmpty());QCOMPARE(tests,0);
 }
 void advancedExpansionPreservesDraftAndCustomEndpoint() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;SettingsWindow window(service,secrets);window.show();
  auto* toggle=window.findChild<QToolButton*>("advancedSettings");QVERIFY(toggle);window.findChild<QPushButton*>("addS3Destination")->click();
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("draft-bucket");window.findChild<QPushButton*>("addS3Destination")->click();auto* profiles=window.findChild<QComboBox*>("destinationProfile");QVERIFY(profiles->isVisible());profiles->setCurrentIndex(0);QCOMPARE(bucket->text(),QString("draft-bucket"));toggle->setFocus();QTest::keyClick(toggle,Qt::Key_Space);QVERIFY(toggle->isChecked());
  auto* endpoint=window.findChild<QLineEdit*>("s3Endpoint");endpoint->setText("https://objects.example.invalid");toggle->click();QCOMPARE(bucket->text(),QString("draft-bucket"));
  window.findChild<QLineEdit*>("s3Region")->setText("eu-north-1");QCOMPARE(endpoint->text(),QString("https://objects.example.invalid"));toggle->click();QCOMPARE(endpoint->text(),QString("https://objects.example.invalid"));QVERIFY(service.revision().isEmpty());
 }
 void sizeInputsSaveFractionalMiBAndPreserveExactAcceptedBytes() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));auto doc=configuration(d.path(),true);auto limits=doc["limits"].toObject();limits["maxBatchBytes"]=qint64(2147483649LL);doc["limits"]=limits;QVERIFY(service.save(doc,{}));CredentialStore secrets;SettingsWindow window(service,secrets);
  auto* field=window.findChild<QLineEdit*>("maxFileBytes");QVERIFY2(field->accessibleName().contains("MiB"),"Size controls must declare binary units");field->setText("1.5");window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_COMPARE(service.document()["limits"].toObject()["maxFileBytes"].toInteger(),qint64(1572864));QCOMPARE(service.document()["limits"].toObject()["maxBatchBytes"].toInteger(),qint64(2147483649LL));
  window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QPushButton*>("saveSettings")->isEnabled());QCOMPARE(service.document()["limits"].toObject()["maxBatchBytes"].toInteger(),qint64(2147483649LL));
  auto revision=service.revision();for(const auto& invalid:QStringList{"0.0000001","999999999999999999999999","bad"}){field->setText(invalid);window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QPushButton*>("saveSettings")->isEnabled());QCOMPARE(service.revision(),revision);QVERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("config_invalid"));}
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
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;SettingsWindow window(service,secrets);
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
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto id,auto value){return id=="s3"?provider.validateSettings(value):Error{"provider_unavailable","Unknown provider",{}};});QVERIFY(service.save(configuration(d.path(),true),{}));SettingsWindow window(service,secrets);
  auto* add=window.findChild<QPushButton*>("addS3Destination");QVERIFY2(add,"Setup must allow a new S3 destination without importing development settings");add->click();
  window.findChild<QLineEdit*>("destinationId")->setText("personal");window.findChild<QLineEdit*>("destinationLabel")->setText("Personal upload");window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-bucket");window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),1);
  auto first=service.document()["profiles"].toArray().first().toObject();QCOMPARE(first["id"].toString(),QString("personal"));QVERIFY(first["settings"].toObject()["publicUrlBase"].isNull());
  add->click();window.findChild<QLineEdit*>("destinationId")->setText("second");window.findChild<QLineEdit*>("s3Bucket")->setText("second-synthetic-bucket");window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),2);QCOMPARE(service.document()["profiles"].toArray().first().toObject(),first);
  auto* remove=window.findChild<QPushButton*>("removeDestination");QVERIFY(remove);remove->click();window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray().first().toObject(),first);
 }
 void reloadingInvalidFileKeepsValidationVisible() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;SettingsWindow window(service,secrets);auto revision=service.revision();QFile file(service.path());QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("invalid json");file.close();QVERIFY(!service.reload());
  QSignalSpy completed(&service,&SettingsService::statusChanged);window.findChild<QPushButton*>("reloadSettings")->click();QTRY_VERIFY(!completed.isEmpty());QVERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("config_invalid"));QCOMPARE(service.revision(),revision);
 }
 void guiActionsAreAvailableWithoutTray() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;SettingsWindow window(service,secrets);QSignalSpy actions(&window,&SettingsWindow::actionRequested);
  for(auto name:Settings::actionNames()){auto* button=window.findChild<QPushButton*>("invoke"+name.left(1).toUpper()+name.mid(1));QVERIFY(button);button->click();QCOMPARE(actions.takeFirst().first().toString(),name);}QVERIFY(service.revision().isEmpty());
 }
 void s3SetupOptionalUrlAndSessionModeUseSharedValidation() {
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto id,auto value){return id=="s3"?provider.validateSettings(value):Error{"provider_unavailable","Unknown provider",{}};});SettingsWindow window(service,secrets);
  window.findChild<QPushButton*>("importDevelopmentS3")->click();window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-bucket");window.findChild<QLineEdit*>("s3PublicUrlBase")->clear();window.findChild<QComboBox*>("s3AclMode")->setCurrentText("omit");window.findChild<QComboBox*>("credentialMode")->setCurrentText("session");window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_VERIFY(!service.revision().isEmpty());auto profile=service.document()["profiles"].toArray().first().toObject();QCOMPARE(profile["settings"].toObject()["bucket"].toString(),QString("synthetic-bucket"));QVERIFY(profile["settings"].toObject()["publicUrlBase"].isNull());QCOMPARE(profile["settings"].toObject()["aclMode"].toString(),QString("omit"));QVERIFY(profile["credentials"].toObject()["reference"].isNull());QCOMPARE(profile["credentials"].toObject()["mode"].toString(),QString("session"));
  auto revision=service.revision();window.findChild<QLineEdit*>("s3Endpoint")->setText("bad-endpoint");window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("s3_settings_invalid"));QCOMPARE(service.revision(),revision);
 }
 void invalidExternalFilePreservesAcceptedStateAndBlocksStaleSave() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;SettingsWindow window(service,secrets);const auto revision=service.revision();window.findChild<QLineEdit*>("archiveRoot")->setText(d.filePath("draft"));QFile file(service.path());QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("invalid json");file.close();QVERIFY(!service.reload());QCOMPARE(service.revision(),revision);window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("config_conflict"));QFile preserved(service.path());QVERIFY(preserved.open(QIODevice::ReadOnly));QCOMPARE(preserved.readAll(),QByteArray("invalid json"));
 }
 void localSetupAndAtomicValidation() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));CredentialStore secrets;SettingsWindow window(service,secrets);
  window.show();auto* local=window.findChild<QPushButton*>("setupLocalSimple");QVERIFY(local);QVERIFY(local->isVisible());local->click();
  auto* save=window.findChild<QPushButton*>("saveSettings");QVERIFY(save);save->click();QTRY_VERIFY(!service.revision().isEmpty());
  QCOMPARE(service.snapshot().actions["region"].destination,QString("local"));
  auto old=service.document();auto* root=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(root);root->setText("relative/path");save->click();
  QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("absolute"));QCOMPARE(service.document(),old);
 }
 void externalAcceptancePreservesDraftAndRequiresReload() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;SettingsWindow window(service,secrets);
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");QVERIFY(advanced);advanced->setChecked(true);auto* root=window.findChild<QLineEdit*>("archiveRoot");root->setText(d.filePath("draft"));advanced->setChecked(false);auto changed=service.document();auto archive=changed["archive"].toObject();archive["root"]=d.filePath("external");changed["archive"]=archive;QVERIFY(service.save(changed,service.revision()));
  QCOMPARE(root->text(),d.filePath("draft"));QVERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("conflict",Qt::CaseInsensitive));QVERIFY(!window.findChild<QPushButton*>("saveSettings")->isEnabled());
  window.findChild<QPushButton*>("reloadSettings")->click();QTRY_COMPARE(root->text(),d.filePath("external"));
 }
 void actionAndFolderChangesRemainIndependent() {
  QTemporaryDir d;ProviderRegistry providers;providers.add("opaque",std::make_shared<OpaqueProvider>());SettingsService service(d.filePath("config.json"),[&](auto id,auto s){return providers.validate(id,s);});QVERIFY(service.save(configuration(d.path()),{}));CredentialStore secrets;SettingsWindow window(service,secrets);
  auto shownProfile=QJsonDocument::fromJson(window.findChild<QPlainTextEdit*>("providerSettingsJson")->toPlainText().toUtf8()).object();QVERIFY2(shownProfile.contains("workspace"),"Generic provider form must load its actual workspace settings");QCOMPARE(shownProfile.size(),1);
  window.findChild<QComboBox*>("destinationRegion")->setCurrentIndex(1);window.findChild<QLineEdit*>("folderVideos")->setText(d.filePath("video"));window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_VERIFY2(service.snapshot().actions["region"].destination=="local",qPrintable(window.findChild<QLabel*>("settingsStatus")->text()));QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("upload"));QCOMPARE(service.snapshot().archive.folders["videos"],d.filePath("video"));QVERIFY(service.snapshot().archive.folders["images"].isEmpty());QCOMPARE(service.snapshot().collisionPolicy,QString("rename"));
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings.moc"
