#include <QComboBox>
#include <QAction>
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
 // #1729: the random-name rows show only for Random names, length and characters only for
 // Random characters, and each choice saves to naming.random*.
 void randomNameRowsFollowTheModeAndSave(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();
  auto* mode=window.findChild<QComboBox*>("namingMode");auto* style=window.findChild<QComboBox*>("randomStyle");auto* length=window.findChild<QSpinBox*>("randomLength");auto* capitals=window.findChild<QCheckBox*>("randomUppercase");auto* digits=window.findChild<QCheckBox*>("randomDigits");
  QVERIFY(mode&&style&&length&&capitals&&digits);
  PUTGD_TRY_VERIFY(!style->isVisible()&&!length->isVisible());
  mode->setCurrentIndex(mode->findData("generated"));PUTGD_TRY_VERIFY(style->isVisible());QVERIFY(!length->isVisible());QVERIFY(!digits->isVisible());
  PUTGD_TRY_COMPARE(service.document()["naming"].toObject()["mode"].toString(),QString("generated"));QVERIFY(!service.document()["naming"].toObject().contains("randomStyle"));
  style->setCurrentIndex(style->findData("string"));PUTGD_TRY_VERIFY(length->isVisible()&&capitals->isVisible()&&digits->isVisible());
  PUTGD_TRY_COMPARE(service.document()["naming"].toObject()["randomStyle"].toString(),QString("string"));
  length->setValue(8);PUTGD_TRY_COMPARE(service.document()["naming"].toObject()["randomLength"].toInt(),8);
  capitals->setChecked(false);PUTGD_TRY_COMPARE(service.document()["naming"].toObject()["randomUppercase"].toBool(true),false);
  digits->setChecked(false);PUTGD_TRY_COMPARE(service.document()["naming"].toObject()["randomDigits"].toBool(true),false);
  // The kept view state accepts the new keys, or the dashboard refuses to close or capture.
  QVERIFY(!window.settings()->saveViewState().contains("stateError"));
  const auto random=Settings::fromValidated(service.document()).randomName;QCOMPARE(random.style,QString("string"));QCOMPARE(random.length,8);QVERIFY(!random.uppercase);QVERIFY(!random.digits);
  mode->setCurrentIndex(mode->findData("original"));PUTGD_TRY_VERIFY(!style->isVisible()&&!length->isVisible());
 }
 // #1804: turning the sensitive text check on saves it, and the kept view state still accepts the draft.
 void sensitiveTextCheckSavesAndKeepsTheViewState(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();
  // #1820: one choice per kind, Off, Warn or Redact automatically.
  auto* check=window.findChild<QCheckBox*>("sensitiveTextCheck");auto* email=window.findChild<QComboBox*>("sensitiveTextKind_email");QVERIFY(check&&email);
  check->setChecked(true);PUTGD_TRY_VERIFY(email->isEnabled());email->setCurrentIndex(email->findData("warn"));
  PUTGD_TRY_VERIFY(Settings::fromValidated(service.document()).sensitiveTextCheck);
  PUTGD_TRY_COMPARE(Settings::fromValidated(service.document()).sensitiveTextCategories,(QStringList{"email","card","key","list"}));
  QJsonObject state;PUTGD_TRY_VERIFY((state=window.settings()->saveViewState(),!state.contains("stateError")));
  QVERIFY(state["draft"].toObject()["preferences"].toObject()["sensitiveTextCheck"].toObject()["enabled"].toBool());
  // Apply to all kinds sets all seven, saved as one change.
  auto* all=window.findChild<QAction*>("sensitiveTextApplyAll_redact");QVERIFY(all);all->trigger();
  PUTGD_TRY_COMPARE(Settings::fromValidated(service.document()).sensitiveTextCover,Settings::sensitiveTextCategoryNames());
  PUTGD_TRY_VERIFY((state=window.settings()->saveViewState(),!state.contains("stateError")));
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
