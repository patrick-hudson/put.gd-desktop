#include "ui/UiApplicationController.h"
#include "ui/SettingsWindow.h"
#include "ui/AboutWindow.h"
#include "fixtures/UiSessionFixture.h"
#include <QApplication>
#include <QJsonDocument>
#include <QLineEdit>
#include <QToolButton>
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
  SettingsWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<SettingsWindow*>(widget))window=candidate;
  QVERIFY(window);window->findChild<QToolButton*>("advancedSettings")->setChecked(true);window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/retained-draft");
  window->findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-secret-never-checkpointed");window->close();
  const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QVERIFY(!device.output.contains("synthetic-secret"));
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
  SettingsWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<SettingsWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}
  QVERIFY(window&&about);window->close();QVERIFY(window->isHidden());QVERIFY(about->isVisible());QVERIFY(device.last("closeRequested").isEmpty());
  device.send("activate",{{"view","settings"}});window->showMinimized();QCoreApplication::processEvents();QVERIFY(device.last("closeRequested").isEmpty());
 }
 void closedMinimizedSettingsDoesNotKeepTheHelperAlive(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});
  SettingsWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<SettingsWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}
  QVERIFY(window&&about);window->showMinimized();window->close();QVERIFY(window->isHidden());about->accept();QTRY_VERIFY(!device.last("closeRequested").isEmpty());
 }
 void closingAboutPreservesCredentialsInOpenSettings(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});
  SettingsWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<SettingsWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}
  QVERIFY(window&&about);window->findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-input");about->close();QCOMPARE(window->findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-input"));QVERIFY(device.last("closeRequested").isEmpty());
 }
 void timedOutCloseRemainsImmutableUntilCancellationIsAcknowledged(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});device.send("activate",{{"view","settings"}});
  SettingsWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<SettingsWindow*>(widget))window=candidate;QVERIFY(window);
  window->findChild<QToolButton*>("advancedSettings")->setChecked(true);window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/first-draft");window->close();const auto request=device.last("closeRequested");
  QTRY_VERIFY_WITH_TIMEOUT(!device.last("closeCancel").isEmpty(),6000);QVERIFY(!window->isEnabled());
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,0);QVERIFY(!window->isEnabled());QVERIFY(device.last("closeConfirmed").isEmpty());
  device.send("closeCancelled",{{"replyTo",device.last("closeCancel")["sequence"]}});QVERIFY(window->isEnabled());QCOMPARE(exits,0);
  for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->close();
  QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/newer-draft");window->close();const auto next=device.last("closeRequested");QVERIFY(next["sequence"]!=request["sequence"]);QVERIFY(QJsonDocument(next).toJson().contains("newer-draft"));
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,0);
  device.send("closeAccepted",{{"replyTo",next["sequence"]}});QCOMPARE(exits,1);QCOMPARE(device.last("closeConfirmed")["payload"].toObject()["replyTo"],next["sequence"]);
 }
 void collapsedSettingsCloseExportsCompleteDraftWithoutMaterializingPages(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});
  device.send("activate",{{"view","settings"}});SettingsWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<SettingsWindow*>(widget))window=candidate;
  QVERIFY(window);QVERIFY(!window->findChild<QLineEdit*>("archiveRoot"));window->findChild<QLineEdit*>("s3Bucket")->setText("collapsed-draft");window->findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-private-input");window->close();
  const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QVERIFY(!device.output.contains("synthetic-private-input"));const auto state=request["payload"].toObject()["state"].toObject()["settings"].toObject();QVERIFY(!state["advanced"].toBool());QVERIFY(state["dirty"].toBool());QCOMPARE(state["draft"].toObject()["profiles"].toArray().first().toObject()["settings"].toObject()["bucket"].toString(),QString("collapsed-draft"));QVERIFY(state["numbers"].toObject().contains("pendingImageBytes"));QVERIFY(!window->findChild<QLineEdit*>("archiveRoot"));
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,1);
 }
 void lastCloseWaitsForAcceptedCheckpoint(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});
  device.send("activate",{{"view","settings"}});
  SettingsWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<SettingsWindow*>(widget))window=candidate;
  QVERIFY(window);QVERIFY(window->isVisible());window->close();
  QVERIFY2(window->isVisible(),"Keep the last window alive until the resident accepts its draft");
  const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QCOMPARE(exits,0);
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,1);
 }
};
QTEST_MAIN(UiApplicationTest)
#include "test_ui_application.moc"
