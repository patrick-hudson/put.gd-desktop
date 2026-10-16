#include "ui/UiApplicationController.h"
#include "ui/SettingsWindow.h"
#include "ui/MainWindow.h"
#include "ui/AboutWindow.h"
#include "fixtures/UiSessionFixture.h"
#include <QApplication>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QtTest>
using namespace putgd;using namespace putgd::test;
class ControlDevice:public QIODevice {
public:
 QByteArray input,output;quint64 sequence=0;qint64 writeLimit=16*1024*1024;
 ControlDevice(){open(ReadWrite|Unbuffered);}
 bool isSequential()const override{return true;}
 qint64 bytesAvailable()const override{return input.size();}
 qint64 readData(char* data,qint64 max)override{const auto n=qMin(max,qint64(input.size()));memcpy(data,input.constData(),n);input.remove(0,n);return n;}
 qint64 writeData(const char* data,qint64 n)override{const auto count=qMin(n,writeLimit);output.append(data,count);return count;}
 void send(const QString& type,QJsonObject payload={}){input+=QJsonDocument(QJsonObject{{"version",1},{"type",type},{"sequence",QString::number(++sequence)},{"payload",payload}}).toJson(QJsonDocument::Compact)+'\n';emit readyRead();}
 QJsonObject last(const QString& type){QJsonObject result;for(const auto& line:output.split('\n')){const auto frame=QJsonDocument::fromJson(line).object();if(frame["type"]==type)result=frame;}return result;}
};
class UiApplicationTest:public QObject {
 Q_OBJECT
private slots:
 void shortcutRecordingRequiresMatchingResidentAcknowledgement(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  bool completed=false;Error error;fixture.session.setShortcutRecording(true,this,[&](Error value){error=value;completed=true;});
  const auto request=device.last("shortcutRecording");QVERIFY(!request.isEmpty());QVERIFY(request["payload"].toObject()["active"].toBool());QVERIFY(!completed);
  device.send("shortcutRecordingChanged",{{"replyTo","unknown"},{"active",true}});QVERIFY(!completed);
  device.send("shortcutRecordingChanged",{{"replyTo",request["sequence"]},{"active",true}});QVERIFY(completed);QVERIFY(!error);
  completed=false;fixture.session.setShortcutRecording(false,this,[&](Error value){error=value;completed=true;});QVERIFY(!completed);
  device.send("shortcutRecordingChanged",{{"replyTo",device.last("shortcutRecording")["sequence"]},{"active",false},{"error",QJsonObject{{"code","restore_failed"},{"message","Synthetic failure"}}}});
  QVERIFY(completed);QCOMPARE(error.code,QString("restore_failed"));
 }
 void shortcutRecordingTimeoutRestoresAndIgnoresLateAcknowledgement(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int completions=0;Error error;fixture.session.setShortcutRecording(true,this,[&](Error value){error=value;++completions;});
  const auto request=device.last("shortcutRecording");QVERIFY(request["payload"].toObject()["active"].toBool());
  QTRY_COMPARE_WITH_TIMEOUT(completions,1,14000);QCOMPARE(error.code,QString("shortcut_recording_timeout"));
  const auto restore=device.last("shortcutRecording");QVERIFY(restore["sequence"]!=request["sequence"]);QCOMPARE(restore["payload"].toObject()["active"],QJsonValue(false));
  device.send("shortcutRecordingChanged",{{"replyTo",request["sequence"]},{"active",true}});QCOMPARE(completions,1);
  device.send("shortcutRecordingChanged",{{"replyTo",restore["sequence"]},{"active",false}});QCOMPARE(completions,1);
 }
 void shortcutRecordingFailsWithoutPrivateControlAndDropsDeadReceiver(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  bool completed=false;Error error;fixture.session.setShortcutRecording(true,this,[&](Error value){completed=true;error=value;});QVERIFY(completed);QCOMPARE(error.code,QString("shortcut_recording_unavailable"));
  std::function<void(Error)> pending;fixture.session.setShortcutRecordingHandler([&](bool,auto reply){pending=std::move(reply);});
  completed=false;auto* receiver=new QObject;fixture.session.setShortcutRecording(true,receiver,[&](Error){completed=true;});delete receiver;pending({});QVERIFY(!completed);
 }
 void finalCloseWaitsForPendingAutosaveDuringAcceptedSetup(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;std::function<void(Error)> complete;UiSessionFixture fixture(settings,secrets,[&](auto,auto,auto reply){complete=reply;});ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);window->findChild<QPushButton*>("testDestination")->click();QTRY_VERIFY(bool(complete));const auto output=dir.filePath("saved-before-close");window->findChild<QLineEdit*>("archiveRoot")->setText(output);window->close();QTest::qWait(500);QVERIFY(device.last("closeRequested").isEmpty());QVERIFY(window->isVisible());complete({});QTRY_COMPARE(settings.document()["archive"].toObject()["root"].toString(),output);QTRY_VERIFY(!window->settings()->saveViewState().contains("stateError"));
 }
 void explicitQuitFlushesFinishedEditsBeforeStoppingTheResident_data(){QTest::addColumn<bool>("setupPending");QTest::newRow("debounce")<<false;QTest::newRow("accepted-setup")<<true;}
 void explicitQuitFlushesFinishedEditsBeforeStoppingTheResident(){
  QFETCH(bool,setupPending);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;std::function<void(Error)> complete;UiSessionFixture fixture(settings,secrets,[&](auto,auto,auto reply){complete=reply;});ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);
  if(setupPending){window->findChild<QPushButton*>("testDestination")->click();QTRY_VERIFY(bool(complete));}const auto output=dir.filePath("saved-before-quit");fixture.client.replyDelayMs["settings.save"]=350;bool quit=false;QString savedAtQuit;fixture.server.setUiQuit([&]{quit=true;savedAtQuit=settings.document()["archive"].toObject()["root"].toString();});window->findChild<QLineEdit*>("archiveRoot")->setText(output);window->findChild<QPushButton*>("quitApplication")->click();
  if(setupPending){QTest::qWait(500);QVERIFY(!quit);complete({});}QTRY_VERIFY(quit);QCOMPARE(savedAtQuit,output);
 }
 void finalCloseWaitsForAnOutstandingLargeCheckpoint(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  const QJsonObject activity{{"selectedId",QString(6*1024*1024,'x')},{"selectedItemId",""},{"collapsedIds",QJsonArray{}},{"geometry",QJsonArray{0,0,500,400}}};
  const QJsonObject state{{"schemaVersion",1},{"activity",activity}};
  ControlDevice device;UiControl control(device);QSignalSpy failures(&control,&UiControl::failed);UiApplicationController controller(fixture.session,control,state);int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});
  device.send("activate",{{"view","about"}});AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;QVERIFY(about);
  device.writeLimit=0;about->move(25,25);QTest::qWait(350);about->close();QCOMPARE(failures.size(),0);QCOMPARE(exits,0);QVERIFY(!about->isEnabled());
  device.writeLimit=16*1024*1024;emit device.bytesWritten(0);const auto checkpoint=device.last("checkpoint");QVERIFY(!checkpoint.isEmpty());QVERIFY(device.last("closeRequested").isEmpty());
  device.send("checkpointAccepted",{{"replyTo",checkpoint["sequence"]}});const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QCOMPARE(failures.size(),0);QCOMPARE(request["payload"].toObject()["state"],state);
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,1);
 }
 void aboutDismissalAlsoClosesTheHelper(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});
  device.send("activate",{{"view","about"}});
  AboutWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=dynamic_cast<AboutWindow*>(widget))window=candidate;
  QVERIFY(window);window->accept();
  QTRY_VERIFY(!device.last("closeRequested").isEmpty());QCOMPARE(exits,0);
  device.send("closeAccepted",{{"replyTo",device.last("closeRequested")["sequence"]}});QCOMPARE(exits,1);
 }
 void closeRejectionRetainsTheDraftAndAllowsRetry(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});device.send("activate",{{"view","settings"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
  QVERIFY(window);window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/retained-draft");
  window->findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-secret-never-checkpointed");window->close();
  QTRY_VERIFY(!device.last("closeRequested").isEmpty());const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QVERIFY(!device.output.contains("synthetic-secret"));
  device.send("closeRejected",{{"replyTo",request["sequence"]},{"error",QJsonObject{{"code","test_reject"},{"message","Try again"}}}});
  QVERIFY(window->isVisible());QVERIFY(window->isEnabled());QCOMPARE(exits,0);
  QCOMPARE(window->findChild<QLineEdit*>("archiveRoot")->text(),QString("/tmp/retained-draft"));
  for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->close();
  QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  window->close();QVERIFY(device.last("closeRequested")["sequence"]!=request["sequence"]);
  device.send("closeAccepted",{{"replyTo",device.last("closeRequested")["sequence"]}});QCOMPARE(exits,1);
 }
 void closingOneOfTwoViewsAndMinimizingDoesNotExit(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});
  MainWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}
  QVERIFY(window&&about);window->close();QVERIFY(window->isHidden());QVERIFY(about->isVisible());QVERIFY(device.last("closeRequested").isEmpty());
  device.send("activate",{{"view","settings"}});window->showMinimized();QCoreApplication::processEvents();QVERIFY(device.last("closeRequested").isEmpty());
 }
 void closedMinimizedSettingsDoesNotKeepTheHelperAlive(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});
  MainWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}
  QVERIFY(window&&about);window->showMinimized();window->close();QVERIFY(window->isHidden());about->accept();QTRY_VERIFY(!device.last("closeRequested").isEmpty());
 }
 void closingAboutPreservesCredentialsInOpenSettings(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});
  MainWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}
  QVERIFY(window&&about);window->findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-input");about->close();QCOMPARE(window->findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-input"));QVERIFY(device.last("closeRequested").isEmpty());
 }
 void timedOutCloseRemainsImmutableUntilCancellationIsAcknowledged(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});device.send("activate",{{"view","settings"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);
  window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/first-draft");window->close();QTRY_VERIFY(!device.last("closeRequested").isEmpty());const auto request=device.last("closeRequested");
  QTRY_VERIFY_WITH_TIMEOUT(!device.last("closeCancel").isEmpty(),6000);QVERIFY(!window->isEnabled());
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,0);QVERIFY(!window->isEnabled());QVERIFY(device.last("closeConfirmed").isEmpty());
  device.send("closeCancelled",{{"replyTo",device.last("closeCancel")["sequence"]}});QVERIFY(window->isEnabled());QCOMPARE(exits,0);
  for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->close();
  QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/newer-draft");window->close();QTRY_VERIFY(device.last("closeRequested")["sequence"]!=request["sequence"]);const auto next=device.last("closeRequested");QVERIFY(next["sequence"]!=request["sequence"]);QVERIFY(QJsonDocument(next).toJson().contains("newer-draft"));
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,0);
  device.send("closeAccepted",{{"replyTo",next["sequence"]}});QCOMPARE(exits,1);QCOMPARE(device.last("closeConfirmed")["payload"].toObject()["replyTo"],next["sequence"]);
 }
 void lastCloseWaitsForAcceptedCheckpoint(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});
  device.send("activate",{{"view","settings"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
  QVERIFY(window);QVERIFY(window->isVisible());window->close();
  QVERIFY2(window->isVisible(),"Keep the last window alive until the resident accepts its draft");
  const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QCOMPARE(exits,0);
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,1);
 }
};
QTEST_MAIN(UiApplicationTest)
#include "test_ui_application.moc"
