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
 // Global hotkeys are suspended while one is recorded; every way a recording
 // ends or fails gives them back.
 void shortcutRecordingCancellationAndFailuresReleaseSuspension(){
  QList<bool> transitions;std::function<void(Error)> ready;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){transitions.append(active);if(active)ready=reply;else reply({});});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();putgd::test::settle(1);
  auto* input=editor.editors().value("region");auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");
  record->click();QTest::keyClick(input,Qt::Key_Escape);QCOMPARE(transitions,QList<bool>({true,false}));ready({});
  record->click();ready({"shortcut_unavailable","Synthetic suspension failure",{}});QCOMPARE(transitions,QList<bool>({true,false,true,false}));
  record->click();ready({});QTest::keyPress(input,Qt::Key_Control);QTest::mouseClick(record,Qt::LeftButton);QCOMPARE(transitions,QList<bool>({true,false,true,false,true,false}));
  record->click();ready({});editor.editors().value("window")->setFocus();QCOMPARE(transitions.size(),8);QCOMPARE(transitions.last(),false);
  record->click();ready({});QEvent deactivate(QEvent::WindowDeactivate);QApplication::sendEvent(&editor,&deactivate);QCOMPARE(transitions.size(),10);QCOMPARE(transitions.last(),false);
  record->click();ready({});QKeyEvent unsupported(QEvent::KeyPress,Qt::Key_unknown,Qt::NoModifier);QApplication::sendEvent(input,&unsupported);QCOMPARE(transitions.size(),12);QCOMPARE(transitions.last(),false);
  record->click();ready({});editor.hide();QCOMPARE(transitions.size(),14);QCOMPARE(transitions.last(),false);
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_capture.moc"
