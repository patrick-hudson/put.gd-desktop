#include "ui/SettingsWindow.h"
#include "ui/ShortcutEditor.h"
#include "fixtures/UiSessionFixture.h"
#include "ui/ViewState.h"
#include "ui/VisualStyle.h"
#include <QAbstractItemView>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtTest/qtestspontaneevent.h>
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
#include <QKeyEvent>
#include "storage/s3/S3Provider.h"
#include "storage/proton/ProtonProvider.h"
#include <QDoubleSpinBox>
#include <QCheckBox>
using namespace putgd;using namespace putgd::test;
class SettingsPaintObserver final:public QObject {
public:int paints=0;QRegion region;
 bool eventFilter(QObject*,QEvent* event) override {if(event->type()==QEvent::Paint){++paints;region|=static_cast<QPaintEvent*>(event)->region();}return false;}
};
class SettingsGui:public QObject {
 Q_OBJECT

private slots:
 void externalAcceptancePreservesInactiveProviderDraftAfterSave() {
  QTemporaryDir directory;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;
  SettingsService service(directory.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->setValue(432.123);
  provider->setCurrentIndex(provider->findData("s3"));window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("Settings saved"));
  auto external=service.document();auto preferences=external["preferences"].toObject();preferences["notifications"]=!preferences["notifications"].toBool();external["preferences"]=preferences;QVERIFY(service.save(external,service.revision()));
  QTRY_VERIFY(window.saveViewState()["conflict"].toBool());QVERIFY(!window.findChild<QPushButton*>("saveSettings")->isEnabled());provider->setCurrentIndex(provider->findData("proton-drive"));QCOMPARE(window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),432.123);
  window.findChild<QPushButton*>("reloadSettings")->click();QTRY_VERIFY(window.isEnabled());provider->setCurrentIndex(provider->findData("proton-drive"));QCOMPARE(window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),120.0);
 }
 void visibleProviderSwitchingRetainsTypedValuesAndSaves() {
  QTemporaryDir directory;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;
  SettingsService service(directory.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto profile=profiles[0].toObject();auto values=profile["settings"].toObject();values["bucket"]="synthetic-saved";profile["settings"]=values;profiles[0]=profile;document["profiles"]=profiles;
  QVERIFY(service.save(document,{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);window.show();window.findChild<QToolButton*>("advancedSettings")->setChecked(true);window.findChild<QListWidget*>("settingsNavigation")->setCurrentRow(1);
  const auto witnessRoot=qEnvironmentVariable("PUTGD_SETTINGS_WITNESS");
  auto witness=[&](const QString& step){
   if(witnessRoot.isEmpty())return true;
   QFile ready(witnessRoot+"/"+step+".ready");if(!ready.open(QIODevice::WriteOnly))return false;ready.write(step.toUtf8());ready.close();
   QElapsedTimer timer;timer.start();while(!QFileInfo::exists(witnessRoot+"/"+step+".seen")&&timer.elapsed()<10000)QTest::qWait(25);
   return QFileInfo::exists(witnessRoot+"/"+step+".seen");
  };
  auto* provider=window.findChild<QComboBox*>("storageProvider");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QTRY_VERIFY(bucket->isVisible());QVERIFY(witness("01-saved-s3"));
  bucket->setFocus();bucket->selectAll();QTest::keyClicks(bucket,"synthetic-edited-s3");
  provider->setFocus();QTest::keyClick(provider,Qt::Key_Down);QCOMPARE(provider->currentData().toString(),QString("proton-drive"));QVERIFY(witness("02-proton"));
  auto* timeout=window.findChild<QDoubleSpinBox*>("protonRequestTimeout");timeout->setValue(321.123);
  provider->setFocus();QTest::keyClick(provider,Qt::Key_Up);QCOMPARE(bucket->text(),QString("synthetic-edited-s3"));QVERIFY(witness("03-restored-s3"));
  provider->setFocus();QTest::keyClick(provider,Qt::Key_Down);QCOMPARE(timeout->value(),321.123);
  provider->setFocus();QTest::keyClick(provider,Qt::Key_Up);bucket->setFocus();bucket->selectAll();QTest::keyClicks(bucket,"synthetic-last-visible");
  auto* save=window.findChild<QPushButton*>("saveSettings");QTest::mouseClick(save,Qt::LeftButton);QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("Settings saved"));QVERIFY(witness("04-saved-visible"));
  SettingsWindow reopened(fixture.session);window.hide();reopened.show();QCOMPARE(reopened.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-last-visible"));QVERIFY(witness("05-reopened-saved"));
 }
 void providerDraftsSurviveCheckpointAndSaveDisplayedEdits() {
  QTemporaryDir directory;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;
  SettingsService service(directory.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);
  SettingsWindow window(fixture.session);const auto savedDocument=service.document();const auto revision=service.revision();
  auto* provider=window.findChild<QComboBox*>("storageProvider");
  window.findChild<QLineEdit*>("destinationLabel")->setText("Synthetic named destination");
  window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-s3-draft");
  window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("000bad");
  window.findChild<QLineEdit*>("credentialReference")->setText("synthetic-s3-reference");
  provider->setCurrentIndex(provider->findData("proton-drive"));
  window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->setValue(321.123);
  auto* folders=window.findChild<QComboBox*>("protonFolder");
  folders->addItem("Synthetic folder A","AAAAAAAAAAAAAAAAAAAAAA~BBBBBBBBBBBBBBBBBBBBBB");
  folders->addItem("Synthetic folder B","AAAAAAAAAAAAAAAAAAAAAA~CCCCCCCCCCCCCCCCCCCCCC");
  folders->setCurrentIndex(1);
  window.findChild<QCheckBox*>("protonShareDefault")->setChecked(false);
  const auto checkpoint=window.saveViewState();QVERIFY(!checkpoint.contains("stateError"));
  QCOMPARE(service.document(),savedDocument);QCOMPARE(service.revision(),revision);
  SettingsWindow restored(fixture.session);QVERIFY(restored.restoreViewState(checkpoint));
  auto* restoredProvider=restored.findChild<QComboBox*>("storageProvider");
  QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->count(),3);
  QCOMPARE(restored.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),321.123);
  restoredProvider->setCurrentIndex(restoredProvider->findData("s3"));
  QCOMPARE(restored.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-s3-draft"));
  QCOMPARE(restored.findChild<QLineEdit*>("s3ConnectTimeoutMs")->text(),QString("000bad"));
  QCOMPARE(restored.findChild<QLineEdit*>("credentialReference")->text(),QString("synthetic-s3-reference"));
  QCOMPARE(restored.findChild<QLineEdit*>("destinationLabel")->text(),QString("Synthetic named destination"));
  restored.findChild<QToolButton*>("advancedSettings")->setChecked(true);restored.show();
  auto* bucket=restored.findChild<QLineEdit*>("s3Bucket");bucket->setFocus();bucket->selectAll();QTest::keyClicks(bucket,"synthetic-last-visible");
  restored.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("5001");
  restored.findChild<QLineEdit*>("s3PublicUrlBase")->clear();
  QTest::mouseClick(restored.findChild<QPushButton*>("saveSettings"),Qt::LeftButton);
  QTRY_VERIFY(restored.findChild<QLabel*>("settingsStatus")->text().contains("Settings saved"));
  const auto profile=service.document()["profiles"].toArray().first().toObject();
  QCOMPARE(profile["provider"].toString(),QString("s3"));
  QCOMPARE(profile["settings"].toObject()["bucket"].toString(),QString("synthetic-last-visible"));
  QVERIFY(profile["settings"].toObject()["publicUrlBase"].isNull());
  QVERIFY(!profile["settings"].toObject().contains("folderUid"));
  restoredProvider->setCurrentIndex(restoredProvider->findData("proton-drive"));
  QCOMPARE(restored.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),321.123);
  QVERIFY(!restored.findChild<QCheckBox*>("protonShareDefault")->isChecked());
  QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->count(),3);
  restoredProvider->setCurrentIndex(restoredProvider->findData("s3"));
  SettingsWindow reopened(fixture.session);
  QCOMPARE(reopened.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-last-visible"));
 }
 void credentialEntryStaysWithItsDestinationAndServiceUntilClose() {
  QTemporaryDir directory;SettingsService service(directory.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  SettingsWindow window(fixture.session);auto* key=window.findChild<QLineEdit*>("credentialSecretKey");auto* password=window.findChild<QLineEdit*>("protonSharePassword");
  key->setText("synthetic-entry-first");window.findChild<QPushButton*>("addS3Destination")->click();QVERIFY(key->text().isEmpty());
  key->setText("synthetic-entry-second");auto* profiles=window.findChild<QComboBox*>("destinationProfile");profiles->setCurrentIndex(0);
  QVERIFY(key->text()=="synthetic-entry-first");
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));
  QVERIFY(key->text().isEmpty());password->setText("synthetic-entry-proton");
  provider->setCurrentIndex(provider->findData("s3"));QVERIFY(key->text()=="synthetic-entry-first");QVERIFY(password->text().isEmpty());
  provider->setCurrentIndex(provider->findData("proton-drive"));QVERIFY(password->text()=="synthetic-entry-proton");
  const auto checkpoint=window.saveViewState();QVERIFY(!checkpoint.contains("stateError"));QVERIFY(!QJsonDocument(checkpoint).toJson().contains("synthetic-entry-"));
  SettingsWindow recreated(fixture.session);QVERIFY(recreated.restoreViewState(checkpoint));QVERIFY(recreated.findChild<QLineEdit*>("protonSharePassword")->text().isEmpty());
  window.close();profiles->setCurrentIndex(1);QVERIFY(key->text().isEmpty());profiles->setCurrentIndex(0);QVERIFY(password->text().isEmpty());
  provider->setCurrentIndex(provider->findData("s3"));QVERIFY(key->text().isEmpty());
 }
 void provisionalDestinationSurvivesLocalSaveAndCheckpointUntilDiscard() {
  QTemporaryDir directory;SettingsService service(directory.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  SettingsWindow window(fixture.session);window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-provisional");
  window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-entry-provisional");
  window.findChild<QPushButton*>("setupLocalSimple")->click();
  QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty());
  window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(!service.revision().isEmpty());
  QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("Settings saved"));QVERIFY(service.document()["profiles"].toArray().isEmpty());
  auto checkpoint=window.saveViewState();QVERIFY(!checkpoint.contains("stateError"));QVERIFY(!QJsonDocument(checkpoint).toJson().contains("synthetic-entry-"));
  SettingsWindow recreated(fixture.session);QVERIFY(recreated.restoreViewState(checkpoint));recreated.findChild<QPushButton*>("addS3Destination")->click();
  QCOMPARE(recreated.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-provisional"));QVERIFY(recreated.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty());
  window.findChild<QPushButton*>("addS3Destination")->click();QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->text()=="synthetic-entry-provisional");
  recreated.findChild<QPushButton*>("reloadSettings")->click();QTRY_VERIFY(recreated.isEnabled());recreated.findChild<QPushButton*>("addS3Destination")->click();
  QVERIFY(recreated.findChild<QLineEdit*>("s3Bucket")->text().isEmpty());
 }
 void draftIdentitySurvivesInvalidRenamesAndRemoval() {
  QTemporaryDir directory;SettingsService service(directory.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  SettingsWindow window(fixture.session);auto* id=window.findChild<QLineEdit*>("destinationId");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");
  id->clear();bucket->setText("synthetic-first");window.findChild<QPushButton*>("addS3Destination")->click();bucket->setText("synthetic-second");
  auto* profiles=window.findChild<QComboBox*>("destinationProfile");const auto secondId=id->text();profiles->setCurrentIndex(0);QVERIFY(id->text().isEmpty());QCOMPARE(bucket->text(),QString("synthetic-first"));
  id->setText(secondId);profiles->setCurrentIndex(1);QCOMPARE(bucket->text(),QString("synthetic-second"));
  profiles->setCurrentIndex(0);auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));provider->setCurrentIndex(provider->findData("s3"));QCOMPARE(bucket->text(),QString("synthetic-first"));
  profiles->setCurrentIndex(1);window.findChild<QPushButton*>("removeDestination")->click();QCOMPARE(bucket->text(),QString("synthetic-first"));
  const auto checkpoint=window.saveViewState();SettingsWindow recreated(fixture.session);QVERIFY(recreated.restoreViewState(checkpoint));QCOMPARE(recreated.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-first"));
 }
 void checkpointRejectsSecretsInsideInactiveProviderAndProvisionalDrafts() {
  QTemporaryDir directory;SettingsService service(directory.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  SettingsWindow window(fixture.session);auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));
  const auto original=window.saveViewState();QVERIFY(!original.contains("stateError"));
  auto state=original;auto editors=state["profileEditors"].toArray();auto editor=editors[0].toObject();auto providers=editor["providers"].toObject();auto s3=providers["s3"].toObject();auto values=s3["settings"].toObject();values["secretKey"]="synthetic-entry-injected";s3["settings"]=values;providers["s3"]=s3;editor["providers"]=providers;editors[0]=editor;state["profileEditors"]=editors;
  const auto error=validateUiState({{"schemaVersion",1},{"settings",state}});QVERIFY(error);QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-entry-"));SettingsWindow recreated(fixture.session);QVERIFY(!recreated.restoreViewState(state));
  window.findChild<QPushButton*>("setupLocalSimple")->click();state=window.saveViewState();QVERIFY(!state.contains("stateError"));auto provisional=state["provisionalProfile"].toObject();auto profile=provisional["profile"].toObject();auto credentials=profile["credentials"].toObject();credentials["secretKey"]="synthetic-entry-injected";profile["credentials"]=credentials;provisional["profile"]=profile;state["provisionalProfile"]=provisional;QVERIFY(validateUiState({{"schemaVersion",1},{"settings",state}}));
 }
 void selectingSavedDestinationIsNavigation() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto values){return s3.validateSettings(values);});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles[0].toObject();second["id"]="synthetic-second";profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  QVERIFY(!window.saveViewState()["dirty"].toBool());window.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(1);QVERIFY2(!window.saveViewState()["dirty"].toBool(),"Selecting a saved destination should not make unchanged configuration dirty");
 }
 void importReplacesTargetWithoutStaleEditor() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto values){return s3.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  const auto importedBucket=service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString();window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-old-draft");window.findChild<QPushButton*>("addS3Destination")->click();window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-unrelated");window.findChild<QPushButton*>("importDevelopmentS3")->click();QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),importedBucket);
  window.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(1);QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-unrelated"));
 }
 void serviceRoundTripRetainsCredentialReference() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* reference=window.findChild<QLineEdit*>("credentialReference");reference->setText("synthetic-edited-reference");auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));provider->setCurrentIndex(provider->findData("s3"));QVERIFY2(reference->text()=="synthetic-edited-reference","Provider switching must retain each provider's credential reference");
  window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("Settings saved"));
  QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["credentials"].toObject()["reference"].toString(),QString("synthetic-edited-reference"));
  SettingsWindow reopened(fixture.session);QCOMPARE(reopened.findChild<QLineEdit*>("credentialReference")->text(),QString("synthetic-edited-reference"));
 }
 void storingCredentialsClearsOnlyCurrentDestinationInputs() {
  QTemporaryDir directory;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(directory.filePath("config.json"),[&](auto,auto values){return s3.validateSettings(values);});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto first=profiles[0].toObject();first["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=first;auto second=first;second["id"]="synthetic-second";second["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles.append(second);document["profiles"]=profiles;QVERIFY(service.save(document,{}));
  UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);auto* destinations=window.findChild<QComboBox*>("destinationProfile");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");
  access->setText("synthetic-first-access");secret->setText("synthetic-first-secret");destinations->setCurrentIndex(1);secret->setText("synthetic-second-secret");destinations->setCurrentIndex(0);
  window.findChild<QPushButton*>("storeCredentials")->click();QTRY_VERIFY(secrets.cached(first["id"].toString(),first["credentials"].toObject()));QVERIFY(access->text().isEmpty());QVERIFY(secret->text().isEmpty());
  destinations->setCurrentIndex(1);QVERIFY(secret->text()=="synthetic-second-secret");destinations->setCurrentIndex(0);QVERIFY(secret->text().isEmpty());
 }
 void protonDestinationDrafts() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));auto* timeout=window.findChild<QDoubleSpinBox*>("protonRequestTimeout");timeout->setValue(321.123);auto* share=window.findChild<QCheckBox*>("protonShareDefault");share->setChecked(false);auto* folders=window.findChild<QComboBox*>("protonFolder");folders->addItem("Synthetic folder","AAAAAAAAAAAAAAAAAAAAAA~CCCCCCCCCCCCCCCCCCCCCC");folders->setCurrentIndex(folders->count()-1);
  window.findChild<QPushButton*>("addS3Destination")->click();provider->setCurrentIndex(provider->findData("proton-drive"));timeout->setValue(654.321);window.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(0);
  QCOMPARE(timeout->value(),321.123);QVERIFY(!share->isChecked());QCOMPARE(folders->currentData().toString(),QString("AAAAAAAAAAAAAAAAAAAAAA~CCCCCCCCCCCCCCCCCCCCCC"));
  window.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(1);QCOMPARE(timeout->value(),654.321);
 }
 void otherSelectorsPreserveRawFields() {
  QTemporaryDir d;CredentialStore secrets;SettingsService service(d.filePath("config.json"));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QLineEdit*>("s3Endpoint")->setText("https://synthetic.example.invalid");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("000bad");window.findChild<QLineEdit*>("fileTemplate")->setText("Synthetic-{uuid}.{ext}");
  QHash<QLineEdit*,QString> before;for(auto* field:window.findChildren<QLineEdit*>())if(!field->objectName().isEmpty())before[field]=field->text();
  for(const auto& name:QStringList{"uploadClipboardBehavior","namingMode","collisionPolicy","s3RegionChoices","s3AclMode","s3Addressing","credentialMode","publicationRegion","publicationWindow","publicationDesktop","publicationClipboard"}){
   auto* combo=window.findChild<QComboBox*>(name);QVERIFY(combo);const auto index=combo->currentIndex();const auto original=combo->currentText();combo->setCurrentIndex((index+1)%combo->count());combo->setCurrentIndex(index);if(combo->isEditable())combo->setEditText(original);
  }
  for(auto* field:window.findChildren<QLineEdit*>())if(before.contains(field))QCOMPARE(field->text(),before[field]);
 }
 void reloadRestoresSavedProviderValues() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  const auto savedBucket=window.findChild<QLineEdit*>("s3Bucket")->text();window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-unsaved");auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));provider->setCurrentIndex(provider->findData("s3"));window.findChild<QPushButton*>("reloadSettings")->click();
  QTRY_VERIFY(window.isEnabled());QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),savedBucket);QVERIFY(!window.saveViewState()["dirty"].toBool());
 }
 void serviceSavedValues() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});auto doc=Settings::developmentTemplate();
  auto profiles=doc["profiles"].toArray();auto p=profiles[0].toObject();p["label"]="Synthetic saved S3";auto values=p["settings"].toObject();values["bucket"]="synthetic-saved-bucket";values["objectPrefix"]="synthetic-prefix/";p["settings"]=values;profiles[0]=p;doc["profiles"]=profiles;
  QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);const auto revision=service.revision();
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));provider->setCurrentIndex(provider->findData("s3"));
  QCOMPARE(service.document(),doc);QCOMPARE(service.revision(),revision);
  QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-saved-bucket"));
  QCOMPARE(window.findChild<QLineEdit*>("s3ObjectPrefix")->text(),QString("synthetic-prefix/"));
  QCOMPARE(window.findChild<QLineEdit*>("destinationLabel")->text(),QString("Synthetic saved S3"));
 }
 void serviceEditedValues() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("synthetic-edited-bucket");auto* provider=window.findChild<QComboBox*>("storageProvider");
  provider->setCurrentIndex(provider->findData("proton-drive"));window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->setValue(321.123);provider->setCurrentIndex(provider->findData("s3"));
  QCOMPARE(bucket->text(),QString("synthetic-edited-bucket"));provider->setCurrentIndex(provider->findData("proton-drive"));QCOMPARE(window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),321.123);
 }
 void protonServiceRoundTrip() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});auto doc=Settings::developmentTemplate();auto p=doc["profiles"].toArray()[0].toObject();p["provider"]="proton-drive";auto values=ProtonProvider::defaults();values["accountRootUid"]="AAAAAAAAAAAAAAAAAAAAAA~BBBBBBBBBBBBBBBBBBBBBB";values["folderUid"]="AAAAAAAAAAAAAAAAAAAAAA~CCCCCCCCCCCCCCCCCCCCCC";values["folderName"]="Synthetic folder";values["requestTimeoutMs"]=321123;p["settings"]=values;doc["profiles"]=QJsonArray{p};const auto setup=service.save(doc,{});QVERIFY2(setup,qPrintable(setup.error.code+": "+setup.error.message));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("s3"));provider->setCurrentIndex(provider->findData("proton-drive"));
  QCOMPARE(window.findChild<QComboBox*>("protonFolder")->currentData().toString(),QString("AAAAAAAAAAAAAAAAAAAAAA~CCCCCCCCCCCCCCCCCCCCCC"));QCOMPARE(window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),321.123);
 }
 void independentDestinationDrafts() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("synthetic-first");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("000bad");window.findChild<QPushButton*>("addS3Destination")->click();bucket->setText("synthetic-second");auto* profiles=window.findChild<QComboBox*>("destinationProfile");
  for(int i=0;i<3;++i){profiles->setCurrentIndex(0);QCOMPARE(bucket->text(),QString("synthetic-first"));QCOMPARE(window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->text(),QString("000bad"));profiles->setCurrentIndex(1);QCOMPARE(bucket->text(),QString("synthetic-second"));}
  window.findChild<QPushButton*>("removeDestination")->click();QCOMPARE(bucket->text(),QString("synthetic-first"));QCOMPARE(service.document()["profiles"].toArray().size(),1);
 }
 void credentialModeAcrossDestination() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* reference=window.findChild<QLineEdit*>("credentialReference");auto* mode=window.findChild<QComboBox*>("credentialMode");reference->setText("synthetic-edited-reference");mode->setCurrentText("session");mode->setCurrentText("keychain");QCOMPARE(reference->text(),QString("synthetic-edited-reference"));
  mode->setCurrentText("session");window.findChild<QPushButton*>("addS3Destination")->click();reference->setText("synthetic-second-reference");window.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(0);QCOMPARE(mode->currentText(),QString("session"));mode->setCurrentText("keychain");QCOMPARE(reference->text(),QString("synthetic-edited-reference"));
 }
 void navigationAndConditionalControls() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QToolButton*>("advancedSettings")->setChecked(true);window.findChild<QLineEdit*>("fileTemplate")->setText("Synthetic-{uuid}.{ext}");window.findChild<QLineEdit*>("folderImages")->setText(d.filePath("edited"));window.findChild<QLineEdit*>("shortcutRegion")->setText("Ctrl+1");
  for(const auto& name:Settings::actionNames()){auto title=name.left(1).toUpper()+name.mid(1);auto* destination=window.findChild<QComboBox*>("destination"+title);const auto id=window.findChild<QComboBox*>("profile"+title)->currentData();destination->setCurrentText("local");destination->setCurrentText("upload");QCOMPARE(window.findChild<QComboBox*>("profile"+title)->currentData(),id);}
  auto* naming=window.findChild<QComboBox*>("namingMode");for(const auto& mode:QStringList{"template","generated","original","template"})naming->setCurrentIndex(naming->findData(mode));
  for(int i=0;i<4;++i)window.findChild<QListWidget*>("settingsNavigation")->setCurrentRow(i);
  window.findChild<QToolButton*>("advancedSettings")->setChecked(false);window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  QCOMPARE(window.findChild<QLineEdit*>("fileTemplate")->text(),QString("Synthetic-{uuid}.{ext}"));QCOMPARE(window.findChild<QLineEdit*>("folderImages")->text(),d.filePath("edited"));QCOMPARE(window.findChild<QLineEdit*>("shortcutRegion")->text(),QString("Ctrl+1"));
 }
 void saveLatestDisplayedValues() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("synthetic-first");window.findChild<QPushButton*>("addS3Destination")->click();bucket->setText("synthetic-second");window.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(0);bucket->setText("synthetic-last-visible");window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_VERIFY(window.findChild<QPushButton*>("saveSettings")->isEnabled());QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-last-visible"));QCOMPARE(service.document()["profiles"].toArray()[1].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-second"));
  SettingsWindow reopened(fixture.session);QCOMPARE(reopened.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-last-visible"));
 }
 void saveIntentionalClearing() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QLineEdit*>("s3PublicUrlBase")->setText("https://synthetic.example.invalid");window.findChild<QPushButton*>("addS3Destination")->click();window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-second");window.findChild<QComboBox*>("destinationProfile")->setCurrentIndex(0);window.findChild<QLineEdit*>("s3PublicUrlBase")->clear();window.findChild<QPushButton*>("saveSettings")->click();
  QTRY_VERIFY(window.findChild<QPushButton*>("saveSettings")->isEnabled());QVERIFY(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["publicUrlBase"].isNull());SettingsWindow reopened(fixture.session);QVERIFY(reopened.findChild<QLineEdit*>("s3PublicUrlBase")->text().isEmpty());
 }
 void credentialInputsStayWithDestination() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-input-only");window.findChild<QPushButton*>("addS3Destination")->click();
  QVERIFY2(window.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty(),"Credential input from destination A must not appear as input for destination B");
 }
 void firstLaunchLocalPreservesDestination() {
  QTemporaryDir d;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="s3"?s3.validateSettings(values):proton.validateSettings(values);});UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QLineEdit*>("s3Bucket")->setText("synthetic-first-launch");window.findChild<QPushButton*>("setupLocalSimple")->click();window.findChild<QPushButton*>("addS3Destination")->click();
  QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),QString("synthetic-first-launch"));
 }
 void uploadClipboardChoiceSavesAndSurvivesViewState() {
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;
  UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  auto* choice=window.findChild<QComboBox*>("uploadClipboardBehavior");QVERIFY(choice);
  QCOMPARE(choice->currentData().toString(),QString("each_link"));QCOMPARE(choice->count(),2);
  choice->setCurrentIndex(choice->findData("list"));const auto state=window.saveViewState();QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",state}}));
  SettingsWindow restored(fixture.session);QVERIFY(restored.restoreViewState(state));
  QCOMPARE(restored.findChild<QComboBox*>("uploadClipboardBehavior")->currentData().toString(),QString("list"));
  restored.findChild<QPushButton*>("saveSettings")->click();
  QTRY_COMPARE(service.document()["preferences"].toObject()["uploadClipboardBehavior"].toString(),QString("list"));
  SettingsWindow reopened(fixture.session);QCOMPARE(reopened.findChild<QComboBox*>("uploadClipboardBehavior")->currentData().toString(),QString("list"));
 }
 void closedDropdownWheelPreservesValueAndScrollsPane_data() {
  QTest::addColumn<QString>("name");
  for(const auto& action:QStringList{"Region","Window","Desktop","Clipboard"})
   for(const auto& prefix:QStringList{"destination","profile","publication"}){
    const auto name=prefix+action;QTest::newRow(qPrintable(name))<<name;
   }
  for(const auto& name:QStringList{"uploadClipboardBehavior","namingMode","collisionPolicy","destinationProfile","storageProvider","s3RegionChoices","s3AclMode","s3Addressing","credentialMode","protonFolder"})
   QTest::newRow(qPrintable(name))<<name;
 }
 void closedDropdownWheelPreservesValueAndScrollsPane() {
  QFETCH(QString,name);
  QTemporaryDir directory;SettingsService service(directory.filePath("config.json"));CredentialStore secrets;
  UiSessionFixture fixture(service,secrets);SettingsWindow window(fixture.session);
  window.findChild<QPushButton*>("addS3Destination")->click();
  window.findChild<QPushButton*>("addS3Destination")->click();
  window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  if(name=="protonFolder"){
   auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));
   auto* folders=window.findChild<QComboBox*>("protonFolder");folders->addItem("Synthetic folder A","folder-a");folders->addItem("Synthetic folder B","folder-b");
  }
  auto* combo=window.findChild<QComboBox*>(name);QVERIFY(combo);QVERIFY(combo->count()>1);
  auto* pages=window.findChild<QStackedWidget*>("settingsPages");
  QScrollArea* scroll=nullptr;
  for(int i=0;i<pages->count();++i)if(pages->widget(i)->isAncestorOf(combo)){
   window.findChild<QListWidget*>("settingsNavigation")->setCurrentRow(i);scroll=qobject_cast<QScrollArea*>(pages->widget(i));break;
  }
  QVERIFY(scroll);window.resize(1000,700);window.show();QTRY_VERIFY(combo->isVisible());
  scroll->ensureWidgetVisible(combo);QCoreApplication::processEvents();
  auto* bar=scroll->verticalScrollBar();QVERIFY(bar->maximum()>0);
  for(bool focused:{false,true}){
   if(focused)combo->setFocus();else window.findChild<QPushButton*>("saveSettings")->setFocus();
   QTRY_COMPARE(combo->hasFocus(),focused);
   const auto index=combo->currentIndex();const auto text=combo->currentText();
   QSignalSpy changes(combo,&QComboBox::currentIndexChanged);
   // Deliver to the embedded editor too: it is the wheel target over editable text.
   QWidget* target=combo->isEditable()?static_cast<QWidget*>(combo->lineEdit()):combo;
   for(int delta:{-120,120}){
    scroll->ensureWidgetVisible(combo);const auto before=bar->value();
    const bool canScroll=delta<0?before<bar->maximum():before>bar->minimum();
    const QPoint point=target->rect().center();
    QWheelEvent wheel(point,target->mapToGlobal(point),{},QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    // Qt propagates only spontaneous wheel input; use the same marker as QtTest mouse helpers.
    QSpontaneKeyEvent::setSpontaneous(&wheel);qApp->notify(target,&wheel);
    QCOMPARE(combo->currentIndex(),index);QCOMPARE(combo->currentText(),text);QCOMPARE(changes.count(),0);
    if(canScroll)QVERIFY2(delta<0?bar->value()>before:bar->value()<before,"Wheel over a closed dropdown must scroll its containing pane");
    else QCOMPARE(bar->value(),before);
   }
  }
 }
 void dropdownMouseAndKeyboardSelection_data() {
  QTest::addColumn<bool>("editable");QTest::newRow("plain")<<false;QTest::newRow("editable")<<true;
 }
 void dropdownMouseAndKeyboardSelection() {
  QFETCH(bool,editable);
  QWidget window;auto* combo=VisualStyle::choiceBox(&window);combo->setEditable(editable);
  combo->addItems({"First","Second","Third"});combo->resize(200,35);window.resize(250,100);window.show();
  combo->setFocus();QTRY_VERIFY(combo->hasFocus());
  QTest::keyClick(combo,Qt::Key_Down);QCOMPARE(combo->currentIndex(),1);
  QTest::keyClick(combo,Qt::Key_Up);QCOMPARE(combo->currentIndex(),0);
  QTest::mouseClick(combo,Qt::LeftButton,Qt::NoModifier,QPoint(combo->width()-12,combo->height()/2));
  auto* view=combo->view();QTRY_VERIFY(view->isVisible());
  const auto item=combo->model()->index(2,0);view->scrollTo(item);
  QTest::qWait(QApplication::doubleClickInterval()+1);
  QTest::mouseMove(view->viewport(),view->visualRect(item).center());
  QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(item).center());
  QTRY_COMPARE(combo->currentIndex(),2);QTRY_VERIFY(!view->isVisible());
  if(editable){combo->lineEdit()->selectAll();QTest::keyClicks(combo->lineEdit(),"custom-region");QCOMPARE(combo->currentText(),QString("custom-region"));}
 }

 void createdProtonFolderNameSurvivesSaveRefreshAndReopen() {
  QTemporaryDir directory;CredentialStore secrets;ProtonProvider provider;
  SettingsService service(directory.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);},nullptr,[&](auto,auto reference){return provider.validateCredentials(reference);});
  const QString root=QString(22,'a')+"~"+QString(22,'b'),created=QString(22,'a')+"~"+QString(22,'c');
  auto document=configuration(directory.path());auto profile=document["profiles"].toArray().first().toObject();profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/proton/synthetic-folder-test"}};document["profiles"]=QJsonArray{profile};QVERIFY(service.save(document,{}));
  UiSessionFixture fixture(service,secrets);int creates=0,inspections=0;QString createdName;const QString refreshedName=QString::fromUtf8("Synthetic folder – 📁");
  fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject& input,JsonReply reply,auto){
   if(kind=="createFolder"){++creates;createdName=input["name"].toString();reply({},{{"accountRootUid",root},{"uid",created},{"name",input["name"]}});}
   else if(kind=="inspectAccount"){++inspections;reply({},{{"accountRootUid",root},{"folders",QJsonArray{QJsonObject{{"uid",created},{"name",refreshedName}}}}});}
   else reply({"unexpected_setup","Unexpected synthetic action",{}},{});
  });
  QJsonObject retained;
  {SettingsWindow window(fixture.session);auto* folders=window.findChild<QComboBox*>("protonFolder");auto* save=window.findChild<QPushButton*>("saveSettings");
   window.findChild<QPushButton*>("createProtonFolder")->click();QTRY_COMPARE(folders->currentData().toString(),created);QTRY_VERIFY(window.saveViewState()["setupJobs"].toArray().isEmpty());QCOMPARE(creates,1);QCOMPARE(folders->currentText(),createdName);
   save->click();QTRY_COMPARE(service.document()["profiles"].toArray().first().toObject()["settings"].toObject()["folderUid"].toString(),created);QTRY_VERIFY(!window.saveViewState().contains("stateError"));QCOMPARE(folders->currentData().toString(),created);QCOMPARE(folders->currentText(),createdName);
   window.findChild<QPushButton*>("inspectProtonAccount")->click();QTRY_COMPARE(inspections,1);QTRY_VERIFY(window.saveViewState()["setupJobs"].toArray().isEmpty());QCOMPARE(folders->currentData().toString(),created);QCOMPARE(folders->currentText(),refreshedName);
   save->click();QTRY_VERIFY(!window.saveViewState().contains("stateError"));QVERIFY(!window.saveViewState()["dirty"].toBool());QCOMPARE(folders->currentText(),refreshedName);retained=window.saveViewState();
  }
  SettingsWindow reopened(fixture.session);QCOMPARE(reopened.findChild<QComboBox*>("protonFolder")->currentText(),refreshedName);QVERIFY(reopened.restoreViewState(retained));QCOMPARE(reopened.findChild<QComboBox*>("protonFolder")->currentText(),refreshedName);QCOMPARE(reopened.findChild<QComboBox*>("protonFolder")->currentData().toString(),created);QCOMPARE(creates,1);
  SettingsService reloaded(service.path(),[&](auto,auto settings){return provider.validateSettings(settings);});QVERIFY(reloaded.metadata()["validation"].toObject()["ok"].toBool());QCOMPARE(reloaded.document()["profiles"].toArray().first().toObject()["settings"].toObject()["folderUid"].toString(),created);QCOMPARE(reloaded.document()["profiles"].toArray().first().toObject()["settings"].toObject()["folderName"].toString(),refreshedName);
 }
 void s3SessionCredentialsRemainSupported() {
  QTemporaryDir directory;CredentialStore secrets;ProviderRegistry registry;registry.add("s3",std::make_shared<S3Provider>(secrets));
  SettingsService service(directory.filePath("config.json"),[&](auto id,auto settings){return registry.validate(id,settings);},nullptr,[&](auto id,auto reference){return registry.validateCredentials(id,reference);});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto profile=profiles.first().toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;document["profiles"]=profiles;QVERIFY(service.save(document,{}));
 }
 void protonDeadlineIsAdvancedAndPreservesMillisecondsOnSaveReopen(){
  QTemporaryDir d;CredentialStore secrets;ProtonProvider proton;SettingsService service(d.filePath("config.json"),[&](auto,auto values){return proton.validateSettings(values);});
  auto doc=configuration(d.path());auto profile=doc["profiles"].toArray().first().toObject();profile["provider"]="proton-drive";auto values=ProtonProvider::defaults();values["requestTimeoutMs"]=120001;profile["settings"]=values;profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/proton/synthetic-timeout"}};doc["profiles"]=QJsonArray{profile};QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);QJsonObject retained;
  {SettingsWindow window(fixture.session);window.show();auto* timeout=window.findChild<QDoubleSpinBox*>("protonRequestTimeout");QVERIFY(timeout);QCOMPARE(timeout->value(),120.001);QVERIFY(!timeout->isVisible());window.findChild<QToolButton*>("advancedSettings")->setChecked(true);QVERIFY(timeout->isVisible());QCOMPARE(timeout->minimum(),1.0);QCOMPARE(timeout->maximum(),86400.0);QVERIFY(timeout->toolTip().contains("deadline"));window.findChild<QPushButton*>("saveSettings")->click();QTRY_VERIFY(!window.saveViewState().contains("stateError")&&!window.saveViewState()["dirty"].toBool());QCOMPARE(service.document()["profiles"].toArray().first().toObject()["settings"].toObject()["requestTimeoutMs"].toInt(),120001);{SettingsWindow fractional(fixture.session);QCOMPARE(fractional.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),120.001);}timeout->setValue(3600);retained=window.saveViewState();QVERIFY(!retained.contains("stateError"));window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().first().toObject()["settings"].toObject()["requestTimeoutMs"].toInt(),3600000);}
  SettingsWindow reopened(fixture.session);QCOMPARE(reopened.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),3600.0);QVERIFY(reopened.restoreViewState(retained));QCOMPARE(reopened.findChild<QDoubleSpinBox*>("protonRequestTimeout")->value(),3600.0);
 }
 void protonDraftKeepsDefaultsAndNeverExportsPassword() {
  QTemporaryDir d;CredentialStore secrets;ProtonProvider proton;S3Provider s3(secrets);
  SettingsService service(d.filePath("config.json"),[&](auto id,auto values){return id=="proton-drive"?proton.validateSettings(values):s3.validateSettings(values);});
  UiSessionFixture fixture(service,secrets);QTRY_VERIFY(fixture.session.ready());SettingsWindow window(fixture.session);window.show();
  auto* provider=window.findChild<QComboBox*>("storageProvider");QVERIFY(provider);provider->setCurrentIndex(provider->findData("proton-drive"));
  QVERIFY(window.findChild<QCheckBox*>("protonShareDefault")->isChecked());QVERIFY(window.findChild<QWidget*>("protonDestinationEditor")->isVisible());QVERIFY(!window.findChild<QWidget*>("credentialsEditor")->isVisible());QVERIFY(!window.findChild<QComboBox*>("s3RegionChoices")->isVisible());
  auto state=window.saveViewState();QVERIFY(!state.contains("stateError"));const auto profile=state["draft"].toObject()["profiles"].toArray().first().toObject();QCOMPARE(profile["provider"].toString(),QString("proton-drive"));QCOMPARE(profile["settings"].toObject(),ProtonProvider::defaults());
  window.findChild<QToolButton*>("advancedSettings")->setChecked(true);auto* password=window.findChild<QLineEdit*>("protonSharePassword");password->setText("synthetic-secret-not-exported");auto snapshot=window.saveViewState();QVERIFY(!QJsonDocument(snapshot).toJson().contains("synthetic-secret-not-exported"));
  SettingsWindow restored(fixture.session);QVERIFY(restored.restoreViewState(snapshot));QVERIFY(restored.findChild<QLineEdit*>("protonSharePassword")->text().isEmpty());
  window.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["profiles"].toArray().first().toObject()["provider"].toString(),QString("proton-drive"));
 }
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
 void shortcutRecordsOnReleaseAndEscapePreservesDraft(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");QVERIFY(record);
  auto* input=editor.editors().value("region");const auto previous=input->text();
  record->click();QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_1,Qt::ControlModifier);
  QCOMPARE(input->text(),previous);
  QKeyEvent releaseOne(QEvent::KeyRelease,Qt::Key_1,Qt::ControlModifier);QApplication::sendEvent(input,&releaseOne);QCOMPARE(input->text(),previous);
  QTest::keyRelease(input,Qt::Key_Control);QCOMPARE(input->text(),QString("Ctrl+1"));
  record->click();QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_2,Qt::ControlModifier);
  QTest::keyClick(input,Qt::Key_Escape,Qt::ControlModifier);QTest::keyRelease(input,Qt::Key_2,Qt::ControlModifier);QTest::keyRelease(input,Qt::Key_Control);
  QCOMPARE(input->text(),QString("Ctrl+1"));
 }
 void shortcutRecordingNormalizesEveryActionAndIgnoresRepeat(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  for(const auto& action:Settings::actionNames()){
   const auto title=action.left(1).toUpper()+action.mid(1);auto* input=editor.editors().value(action);const auto old=input->text();
   editor.findChild<QPushButton*>("record"+title+"Shortcut")->click();
   QTest::keyPress(input,Qt::Key_Control);QTest::keyRelease(input,Qt::Key_Control);QCOMPARE(input->text(),old);
   QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_Shift,Qt::ControlModifier);QTest::keyPress(input,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier);
   QKeyEvent repeatedPress(QEvent::KeyPress,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier,{},true);QApplication::sendEvent(input,&repeatedPress);
   QKeyEvent repeatedRelease(QEvent::KeyRelease,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier,{},true);QApplication::sendEvent(input,&repeatedRelease);QCOMPARE(input->text(),old);
   QTest::keyRelease(input,Qt::Key_Control,Qt::ShiftModifier);QTest::keyRelease(input,Qt::Key_Shift);QCOMPARE(input->text(),old);
   QTest::keyRelease(input,Qt::Key_F8);QCOMPARE(input->text(),QString("Ctrl+Shift+F8"));
   QCOMPARE(editor.actions({})[action].toObject()["shortcut"].toString(),QString("Ctrl+Shift+F8"));
   QTest::keyClick(input,Qt::Key_Tab);QVERIFY(QApplication::focusWidget()!=input);
   editor.findChild<QPushButton*>("clear"+title+"Shortcut")->click();QVERIFY(input->text().isEmpty());
  }
 }
 void shortcutRecordingTracksPhysicalKeysAcrossModifierRelease(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");const auto old=input->text();record->click();
  const auto send=[&](QEvent::Type type,int key,Qt::KeyboardModifiers modifiers,quint32 scan){QKeyEvent event(type,key,modifiers,scan,0,0);QApplication::sendEvent(input,&event);};
  send(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier,50);send(QEvent::KeyPress,Qt::Key_Exclam,Qt::ShiftModifier,10);
  send(QEvent::KeyRelease,Qt::Key_Shift,Qt::NoModifier,50);QCOMPARE(input->text(),old);
  // X11 can report a repeated key with a new symbol when modifiers change.
  send(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(record->text(),QString("Cancel"));
  send(QEvent::KeyRelease,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(input->text(),QString("Shift+!"));QCOMPARE(record->text(),QString("Record shortcut"));
  record->click();send(QEvent::KeyPress,Qt::Key_1,Qt::KeypadModifier,87);send(QEvent::KeyRelease,Qt::Key_1,Qt::KeypadModifier,87);
  QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::KeypadModifier,Qt::Key_1)).toString(QKeySequence::PortableText));QCOMPARE(record->text(),QString("Record shortcut"));
  for(const auto modifierFirst:{false,true}){
   record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);
   if(modifierFirst){send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);send(QEvent::KeyRelease,Qt::Key_2,Qt::KeypadModifier,88);}
   else {send(QEvent::KeyRelease,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);}
   QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::ControlModifier|Qt::KeypadModifier,Qt::Key_2)).toString(QKeySequence::PortableText));QCOMPARE(record->text(),QString("Record shortcut"));
  }
 }
 void shortcutRecordingCancellationAndFailuresReleaseSuspension(){
  QList<bool> transitions;std::function<void(Error)> ready;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){transitions.append(active);if(active)ready=reply;else reply({});});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();QTest::qWait(1);
  auto* input=editor.editors().value("region");auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");const auto old=input->text();
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);QCOMPARE(input->text(),old);
  QTest::keyClick(input,Qt::Key_Escape);QCOMPARE(transitions,QList<bool>({true,false}));ready({});QCOMPARE(record->text(),QString("Record shortcut"));
  record->click();ready({"shortcut_unavailable","Synthetic suspension failure",{}});QCOMPARE(transitions,QList<bool>({true,false,true,false}));QCOMPARE(input->text(),old);
  record->click();ready({});QTest::keyPress(input,Qt::Key_Control);QTest::mouseClick(record,Qt::LeftButton);QCOMPARE(record->text(),QString("Record shortcut"));QCOMPARE(transitions.last(),false);
  record->click();ready({});editor.editors().value("window")->setFocus();QCOMPARE(transitions.last(),false);QCOMPARE(input->text(),old);
  record->click();ready({});QEvent deactivate(QEvent::WindowDeactivate);QApplication::sendEvent(&editor,&deactivate);QCOMPARE(transitions.last(),false);
  record->click();ready({});QKeyEvent unsupported(QEvent::KeyPress,Qt::Key_unknown,Qt::NoModifier);QApplication::sendEvent(input,&unsupported);QCOMPARE(transitions.last(),false);QVERIFY(editor.findChild<QLabel*>("shortcutRecordingStatus")->text().contains("Unsupported"));
  record->click();ready({});editor.hide();QCOMPARE(transitions.last(),false);QCOMPARE(input->text(),old);
 }
 void shortcutRecordingDistinguishesSupersededRestoreFromFailure(){
  std::function<void(Error)> restored;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){if(active)reply({});else restored=std::move(reply);});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();QTest::qWait(1);
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");auto* status=editor.findChild<QLabel*>("shortcutRecordingStatus");
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);const auto recorded=status->text();
  restored({"shortcut_recording_interrupted","Global shortcut configuration changed.",{}});QCOMPARE(status->text(),recorded);QCOMPARE(input->text(),QString("Ctrl+1"));
  record->click();QTest::keyClick(input,Qt::Key_Escape);restored({"shortcut_restore_failed","Synthetic restore failure",{}});QVERIFY(status->text().contains("Could not restore shortcuts"));QCOMPARE(input->text(),QString("Ctrl+1"));
 }
 void shortcutRecordingPreservesDirtySettingsAndSaveCancelContract(){
  QTemporaryDir d;SettingsService service(d.filePath("config.json"));QVERIFY(service.save(configuration(d.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QTRY_VERIFY(fixture.session.ready());
  fixture.session.setShortcutRecordingHandler([](bool,auto reply){reply({});});
  SettingsWindow window(fixture.session);window.show();window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  auto* navigation=window.findChild<QListWidget*>("settingsNavigation");for(int i=0;i<navigation->count();++i)if(navigation->item(i)->text()=="Shortcuts")navigation->setCurrentRow(i);
  auto* input=window.findChild<QLineEdit*>("shortcutRegion");auto* record=window.findChild<QPushButton*>("recordRegionShortcut");
  const auto saved=service.document();window.findChild<QLineEdit*>("archiveRoot")->setText(d.filePath("unsaved"));
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);QCOMPARE(input->text(),QString("Ctrl+1"));QCOMPARE(service.document(),saved);
  QVERIFY(service.patch({{"preferences",QJsonObject{{"notifications",false}}}},service.revision()));QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("changed"));QCOMPARE(input->text(),QString("Ctrl+1"));QCOMPARE(window.findChild<QLineEdit*>("archiveRoot")->text(),d.filePath("unsaved"));
  window.findChild<QPushButton*>("cancelSettings")->click();QTRY_VERIFY(!window.isVisible());QCOMPARE(service.document()["actions"],saved["actions"]);
  SettingsWindow second(fixture.session);second.show();second.findChild<QToolButton*>("advancedSettings")->setChecked(true);navigation=second.findChild<QListWidget*>("settingsNavigation");for(int i=0;i<navigation->count();++i)if(navigation->item(i)->text()=="Shortcuts")navigation->setCurrentRow(i);
  input=second.findChild<QLineEdit*>("shortcutRegion");second.findChild<QPushButton*>("recordRegionShortcut")->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);
  second.findChild<QPushButton*>("saveSettings")->click();QTRY_COMPARE(service.document()["actions"].toObject()["region"].toObject()["shortcut"].toString(),QString("Ctrl+1"));
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
  const QStringList names{"fileTemplate","captureTemplate","imageTemplate","textTemplate","pendingImageBytes","archiveRoot","keepUploadCopies","folderImages","folderVideos","folderText","folderOther","uploadClipboardBehavior","namingMode","collisionPolicy","destinationProfile","destinationId","destinationLabel","s3Bucket","s3Region","s3Endpoint","s3ObjectPrefix","s3StorageClass","s3PublicUrlBase","s3ConnectTimeoutMs","s3RequestTimeoutMs","s3LowSpeedBytesPerSecond","s3LowSpeedTimeSeconds","s3AclMode","s3Addressing","providerSettingsJson","credentialMode","credentialReference","credentialAccessKey","credentialSecretKey","credentialSessionToken","notifications","launchAtLogin","maxFiles","pendingOperations","concurrentUploads","maxFileBytes","maxBatchBytes","pendingArtifactBytes","clipboardTextBytes","imagePixels","imageBytes","failedArtifactDays","recentActivityLimit","previewCacheBytes","historyRetentionDays","shortcutRegion","shortcutWindow","shortcutDesktop","shortcutClipboard","destinationRegion","destinationWindow","destinationDesktop","destinationClipboard","profileRegion","profileWindow","profileDesktop","profileClipboard"};
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

 void shortcutLayoutErrorsDoNotCallValidDraftInvalid() {
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());const auto draft=editor.actions({});
  for(const auto& code:QStringList{"shortcut_unsupported","shortcut_unmapped","shortcut_ambiguous","shortcut_unavailable"}){
   const auto state=code=="shortcut_ambiguous"?QString("shortcut_unmapped"):code;
   const QJsonObject diagnostics{{"available",true},{"bindings",QJsonObject{{"region",QJsonObject{{"requested","Ctrl+Shift+1"},{"bound",false},{"state",state},{"error",code}}}}}};
   editor.setStatus(diagnostics);auto* label=editor.findChild<QLabel*>("shortcutRegionStatus");QVERIFY(label);QVERIFY(!label->text().contains("Invalid shortcut"));QVERIFY(!label->text().contains("Enter a single"));QVERIFY(!label->text().contains("Try one key"));
   QVERIFY(label->text().contains(code=="shortcut_unsupported"?"Unsupported":"Unavailable"));QCOMPARE(editor.actions({}),draft);
   auto* details=editor.findChild<QPlainTextEdit*>("shortcutDetails");QVERIFY(details);QCOMPARE(QJsonDocument::fromJson(details->toPlainText().toUtf8()).object(),diagnostics);
  }
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
