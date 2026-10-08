#include "ui/SettingsWindow.h"
#include "fixtures/U2Fixtures.h"
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSignalSpy>
#include <QPlainTextEdit>
#include "storage/s3/S3Provider.h"
using namespace putgd;using namespace putgd::test;
class SettingsGui:public QObject {
 Q_OBJECT

private slots:
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
  auto* local=window.findChild<QPushButton*>("setupLocal");QVERIFY(local);local->click();
  auto* save=window.findChild<QPushButton*>("saveSettings");QVERIFY(save);save->click();QTRY_VERIFY(!service.revision().isEmpty());
  QCOMPARE(service.snapshot().actions["region"].destination,QString("local"));
  auto old=service.document();auto* root=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(root);root->setText("relative/path");save->click();
  QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("absolute"));QCOMPARE(service.document(),old);
 }
 void externalAcceptancePreservesDraftAndRequiresReload() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;SettingsWindow window(service,secrets);
  auto* root=window.findChild<QLineEdit*>("archiveRoot");root->setText(d.filePath("draft"));auto changed=service.document();auto archive=changed["archive"].toObject();archive["root"]=d.filePath("external");changed["archive"]=archive;QVERIFY(service.save(changed,service.revision()));
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
