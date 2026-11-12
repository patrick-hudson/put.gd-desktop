#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 // Recording routed to its own destination with its own sharing mode: opening the
 // Capture page rewrites nothing, and changing the default keeps that route.
 void recordingOutputPreservesInheritanceAndExplicitRoutes(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto actions=doc["actions"].toObject();auto region=actions["region"].toObject();region["destination"]="upload";region["profileId"]="synthetic-alternate";region["publicationMode"]="private";actions["region"]=region;
  auto record=actions["record"].toObject();record["shortcut"]="Ctrl+Shift+9";record["destination"]="upload";record["profileId"]="synthetic-third";record["publicationMode"]="share";actions["record"]=record;doc["actions"]=actions;QVERIFY(service.save(doc,{}));const auto original=service.document();QFile stored(service.path());QVERIFY(stored.open(QIODevice::ReadOnly));const auto bytes=stored.readAll();stored.close();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(1);window.show();window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);
  PUTGD_COMPARE_STEADY(service.document(),original,500);QVERIFY(stored.open(QIODevice::ReadOnly));QCOMPARE(stored.readAll(),bytes);stored.close();
  auto* base=window.findChild<QComboBox*>("defaultCaptureDestination");base->setCurrentIndex(base->findData("synthetic-alternate"));PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("synthetic-alternate"));QCOMPARE(service.document()["actions"].toObject()["record"].toObject(),record);
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_capture.moc"
