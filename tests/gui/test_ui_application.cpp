// Settings edits survive quit and close, and a capture started from Settings
// never photographs a credential the user is typing.
#include "ui/UiApplicationController.h"
#include "ui/MainWindow.h"
#include "ui/AboutWindow.h"
#include "fixtures/ControlDevice.h"
#include "fixtures/UiSessionFixture.h"
#include <QAction>
#include <QApplication>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QtTest>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
static MainWindow* mainWindow(){MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;return window;}
static QToolButton* railButton(MainWindow* window,const QString& action){for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")==action)return candidate;return nullptr;}
class UiApplicationTest:public QObject {
 Q_OBJECT
private slots:
 // A capture started from the rail saves the open edits first and hides every
 // put.gd window for a screen action, so none of them lands in the image.
 void railActionsFlushSettingsAndHideScreenWindowsBeforeDispatch_data(){QTest::addColumn<QString>("action");for(const auto& name:Settings::actionNames())QTest::newRow(qPrintable(name))<<name;}
 void railActionsFlushSettingsAndHideScreenWindowsBeforeDispatch(){
  QFETCH(QString,action);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);fixture.client.replyDelayMs["settings.save"]=300;
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});MainWindow* window=mainWindow();AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;QVERIFY(window&&about);
  const auto output=dir.filePath("saved-before-rail");window->findChild<QLineEdit*>("archiveRoot")->setText(output);int invoked=0;bool mainVisible=false,aboutVisible=false;QString savedAtInvocation;
  // The synthetic rejection keeps the fixture from capturing anything.
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method!="action.invoke")return;++invoked;mainVisible=window->isVisible();aboutVisible=about->isVisible();savedAtInvocation=settings.document()["archive"].toObject()["root"].toString();fixture.client.failOnce[method]={"fixture_capture_rejected","Synthetic rejection",{}};};
  auto* button=railButton(window,action);QVERIFY(button);button->click();PUTGD_TRY_COMPARE(invoked,1);QCOMPARE(savedAtInvocation,output);QCOMPARE(mainVisible,action=="clipboard");QCOMPARE(aboutVisible,action=="clipboard");
 }
 // A credential typed but not stored yet stays in its window: the screen
 // capture does not start while it could show it.
 void screenRailActionKeepsUnstoredCredentialsInTheirWindow_data(){QTest::addColumn<bool>("cached");QTest::newRow("active-destination")<<false;QTest::newRow("other-destination")<<true;}
 void screenRailActionKeepsUnstoredCredentialsInTheirWindow(){
  QFETCH(bool,cached);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto other=profiles.first().toObject();other["id"]="synthetic-alternate";other["label"]="Synthetic alternate";profiles.append(other);document["profiles"]=profiles;QVERIFY(settings.save(document,{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","destinations"}});MainWindow* window=mainWindow();QVERIFY(window);
  auto* secret=window->findChild<QLineEdit*>("credentialSecretKey");secret->setText("synthetic-unfinished-input");if(cached)window->findChild<QListWidget*>("destinationList")->setCurrentRow(1);int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke")++invoked;};
  auto* button=railButton(window,"region");QVERIFY(button);button->click();PUTGD_COMPARE_STEADY(invoked,0,300);if(cached)window->findChild<QListWidget*>("destinationList")->setCurrentRow(0);QCOMPARE(secret->text(),QString("synthetic-unfinished-input"));
 }
 // Quit saves the finished edits before the resident stops, also while a
 // destination test holds the save back.
 void explicitQuitFlushesFinishedEditsBeforeStoppingTheResident_data(){QTest::addColumn<bool>("setupPending");QTest::newRow("debounce")<<false;QTest::newRow("accepted-setup")<<true;}
 void explicitQuitFlushesFinishedEditsBeforeStoppingTheResident(){
  QFETCH(bool,setupPending);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;std::function<void(Error)> complete;UiSessionFixture fixture(settings,secrets,[&](auto,auto,auto reply){complete=reply;});ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=mainWindow();QVERIFY(window);
  if(setupPending){window->findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(complete));}const auto output=dir.filePath("saved-before-quit");fixture.client.replyDelayMs["settings.save"]=350;bool quit=false;QString savedAtQuit;fixture.server.setUiQuit([&]{quit=true;savedAtQuit=settings.document()["archive"].toObject()["root"].toString();});window->findChild<QLineEdit*>("archiveRoot")->setText(output);window->findChild<QAction*>("quitApplication")->trigger();
  if(setupPending){PUTGD_VERIFY_STEADY(!quit,500);complete({});}PUTGD_TRY_VERIFY(quit);QCOMPARE(savedAtQuit,output);
 }
 // Closing the last window during a destination test waits for its save.
 void finalCloseWaitsForPendingAutosaveDuringAcceptedSetup(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;std::function<void(Error)> complete;UiSessionFixture fixture(settings,secrets,[&](auto,auto,auto reply){complete=reply;});ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=mainWindow();QVERIFY(window);
  window->findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(complete));const auto output=dir.filePath("saved-before-close");window->findChild<QLineEdit*>("archiveRoot")->setText(output);window->close();
  PUTGD_VERIFY_STEADY(device.last("closeRequested").isEmpty(),500);complete({});PUTGD_TRY_COMPARE(settings.document()["archive"].toObject()["root"].toString(),output);
 }
 // #1704: the sequence an agent drives through the resident. A move sent while a label save is
 // held reports saving, quitting waits for the whole queue, and the resident's document has both.
 void agentMoveAndQuitSaveTheWholeQueue(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();
  for(const auto* id:{"second","third"}){auto profile=profiles.first().toObject();profile["id"]=id;profile["label"]=QString(id)+" destination";profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference",QString("putgd/")+id}};profiles.append(profile);}
  document["profiles"]=profiles;QVERIFY(settings.save(document,{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=mainWindow();QVERIFY(window);window->navigate("destinations");
  auto* label=window->findChild<QLineEdit*>("destinationLabel");QVERIFY(label);label->setText("Renamed first");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  device.send("command",{{"version",1},{"command","moveDestination"},{"params",QJsonObject{{"direction","up"},{"profileId","third"}}}});
  PUTGD_TRY_VERIFY(!device.last("commandResult").isEmpty());
  const auto moved=device.last("commandResult")["payload"].toObject();
  QVERIFY2(moved["error"].toObject().isEmpty()||moved["error"].toObject()["code"].toString().isEmpty(),qPrintable(QJsonDocument(moved).toJson()));
  QCOMPARE(moved["result"].toObject()["saving"].toBool(),true);
  bool quit=false;QJsonObject atQuit;fixture.server.setUiQuit([&]{quit=true;atQuit=settings.document();});
  emit window->quitRequested();
  PUTGD_VERIFY_STEADY(!quit,500);
  PUTGD_TRY_VERIFY((fixture.client.release(),quit));
  QStringList ids;for(const auto& value:atQuit["profiles"].toArray())ids.append(value.toObject()["id"].toString());
  QCOMPARE(ids,QStringList({"development-s3","third","second"}));
  QCOMPARE(atQuit["profiles"].toArray()[0].toObject()["label"].toString(),QString("Renamed first"));
 }
 // A refused close keeps the window and its draft, sends no unsaved secret
 // to the resident, and a second close can still succeed.
 void closeRejectionRetainsTheDraftAndAllowsRetry(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});device.send("activate",{{"view","settings"}});
  MainWindow* window=mainWindow();QVERIFY(window);window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/retained-draft");
  window->findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-secret-never-checkpointed");window->close();
  PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());const auto request=device.last("closeRequested");QVERIFY(!device.output.contains("synthetic-secret"));
  device.send("closeRejected",{{"replyTo",request["sequence"]},{"error",QJsonObject{{"code","test_reject"},{"message","Try again"}}}});
  QVERIFY(window->isVisible());QCOMPARE(exits,0);
  QCOMPARE(window->findChild<QLineEdit*>("archiveRoot")->text(),QString("/tmp/retained-draft"));
  for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->close();
  QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  window->close();QVERIFY(device.last("closeRequested")["sequence"]!=request["sequence"]);
  device.send("closeAccepted",{{"replyTo",device.last("closeRequested")["sequence"]}});QCOMPARE(exits,1);
 }
};
QTEST_MAIN(UiApplicationTest)
#include "test_ui_application.moc"
