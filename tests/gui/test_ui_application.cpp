#include "ui/UiApplicationController.h"
#include "ui/SettingsWindow.h"
#include "ui/MainWindow.h"
#include "ui/AboutWindow.h"
#include "ui/Theme.h"
#include <QScopeGuard>
#include "fixtures/UiSessionFixture.h"
#include <QApplication>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
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
 // #60: the appearance setting, saved here or patched by the CLI/MCP, switches
 // the scheme of the running interface without a restart.
 void appearanceSettingAppliesLive(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();QVERIFY(settings.save(document,{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  auto restore=qScopeGuard([]{Theme::setScheme(Theme::Scheme::Dark);});
  QCOMPARE(Theme::scheme(),Theme::Scheme::Dark);
  auto preferences=document["preferences"].toObject();preferences["appearance"]=QJsonObject{{"theme","light"}};document["preferences"]=preferences;QVERIFY(settings.save(document,settings.revision()));
  fixture.session.invalidate();QTRY_COMPARE(Theme::scheme(),Theme::Scheme::Light);
  // The switch reaches a checkpoint, so ui.viewState reports it.
  QTRY_VERIFY(device.last("checkpoint")["payload"].toObject()["state"].toObject()["appearance"].toObject()["scheme"]==QString("light"));
  preferences["appearance"]=QJsonObject{{"theme","system"}};document["preferences"]=preferences;QVERIFY(settings.save(document,settings.revision()));
  fixture.session.invalidate();QTRY_COMPARE(Theme::scheme(),Theme::Scheme::Dark);
 }
 void moveDestinationCommandDrivesTheOpenSettingsWindow(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto other=profiles.first().toObject();other["id"]="synthetic-alternate";other["label"]="Synthetic alternate";other["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-alternate"}};profiles.append(other);document["profiles"]=profiles;QVERIFY(settings.save(document,{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  auto result=[&]{return device.last("commandResult")["payload"].toObject();};
  device.send("command",{{"version",1},{"command","moveDestination"},{"params",QJsonObject{{"direction","down"}}}});QCOMPARE(result()["error"].toObject()["code"].toString(),QString("ui_view_closed"));QCOMPARE(result()["replyTo"].toString(),QString::number(device.sequence));
  device.send("activate",{{"view","destinations"}});QVERIFY(!device.last("shown").isEmpty());
  device.send("command",{{"version",1},{"command","explode"},{"params",QJsonObject{}}});QCOMPARE(result()["error"].toObject()["code"].toString(),QString("ui_command_unknown"));
  device.send("command",{{"version",2},{"command","moveDestination"},{"params",QJsonObject{{"direction","up"}}}});QCOMPARE(result()["error"].toObject()["code"].toString(),QString("ui_command_unsupported"));QCOMPARE(settings.document()["profiles"].toArray().first().toObject()["id"].toString(),QString("development-s3"));
  device.send("command",{{"version",1},{"command","moveDestination"},{"params",QJsonObject{{"direction","up"},{"profileId","synthetic-alternate"}}}});
  QCOMPARE(result()["replyTo"].toString(),QString::number(device.sequence));QVERIFY2(result()["error"].toObject()["code"].toString().isEmpty(),qPrintable(result()["error"].toObject()["code"].toString()));
  QCOMPARE(result()["result"].toObject()["order"].toArray(),QJsonArray({"synthetic-alternate","development-s3"}));QCOMPARE(result()["result"].toObject()["selected"].toString(),QString("synthetic-alternate"));
  QTRY_COMPARE(settings.document()["profiles"].toArray().first().toObject()["id"].toString(),QString("synthetic-alternate"));
  device.send("command",{{"version",1},{"command","moveDestination"},{"params",QJsonObject{{"direction","up"}}}});QCOMPARE(result()["error"].toObject()["code"].toString(),QString("destination_move_invalid"));
 }
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
 void allMainPagesRemainVisibleWithNativeStyleRowMetrics(){
  class NativeRows:public QStyledItemDelegate {public:using QStyledItemDelegate::QStyledItemDelegate;QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override{auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(qMax(size.height(),52));return size;}};
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);MainWindow window(fixture.session);auto* navigation=window.findChild<QListWidget*>("mainNavigation");QVERIFY(navigation);navigation->setItemDelegate(new NativeRows(navigation));window.resize(1060,680);window.show();navigation->setCurrentRow(2);QTest::qWait(100);QCOMPARE(navigation->verticalScrollBar()->maximum(),0);for(int row=0;row<navigation->count();++row){const auto rect=navigation->visualItemRect(navigation->item(row));QVERIFY(rect.top()>=0);QVERIFY(rect.bottom()<navigation->viewport()->height());}
 }
 void railActionsFlushSettingsAndHideScreenWindowsBeforeDispatch_data(){QTest::addColumn<QString>("action");for(const auto& name:Settings::actionNames())QTest::newRow(qPrintable(name))<<name;}
 void railActionsFlushSettingsAndHideScreenWindowsBeforeDispatch(){
  QFETCH(QString,action);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);fixture.client.replyDelayMs["settings.save"]=300;
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});MainWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}QVERIFY(window&&about);
  const auto output=dir.filePath("saved-before-rail");window->findChild<QLineEdit*>("archiveRoot")->setText(output);int invoked=0;bool mainVisible=false,aboutVisible=false;QString savedAtInvocation;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method!="action.invoke")return;++invoked;mainVisible=window->isVisible();aboutVisible=about->isVisible();savedAtInvocation=settings.document()["archive"].toObject()["root"].toString();fixture.client.failOnce[method]={"fixture_capture_rejected","Synthetic rejection",{}};};
  QPushButton* button=nullptr;for(auto* candidate:window->findChildren<QPushButton*>())if(candidate->property("captureAction")==action)button=candidate;QVERIFY(button);button->click();button->click();QTRY_COMPARE(invoked,1);QCOMPARE(savedAtInvocation,output);QCOMPARE(mainVisible,action=="clipboard");QCOMPARE(aboutVisible,action=="clipboard");QTRY_VERIFY(window->isVisible()&&about->isVisible());QVERIFY(device.last("closeRequested").isEmpty());
 }
 void screenRailActionWaitsForAnOpenDialogToBeDismissed(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","history"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);QDialog dialog(window);dialog.setWindowModality(Qt::NonModal);dialog.show();int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke"){++invoked;fixture.client.failOnce[method]={"fixture_rejected","Synthetic rejection",{}};}};QPushButton* button=nullptr;for(auto* candidate:window->findChildren<QPushButton*>())if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();QTest::qWait(300);QCOMPARE(invoked,0);QVERIFY(window->isVisible()&&dialog.isVisible());dialog.reject();button->click();QTRY_COMPARE(invoked,1);QTRY_VERIFY(window->isVisible());
 }
 void acceptedScreenRailActionClosesTheHiddenHelperAfterCheckpoint(){
  QTemporaryDir dir;ProviderRegistry registry;SettingsService settings(dir.filePath("config/config.json"));auto document=Settings::defaults();auto archive=document["archive"].toObject();archive["keepUploads"]=true;archive["root"]=dir.filePath("archive");document["archive"]=archive;QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();OperationService operations(dir.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});QSignalSpy exits(&controller,&UiApplicationController::exitRequested);device.send("activate",{{"view","history"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);QPushButton* button=nullptr;for(auto* candidate:window->findChildren<QPushButton*>())if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);window->move(25,25);QTRY_VERIFY(!device.last("checkpoint").isEmpty());const auto checkpoint=device.last("checkpoint");button->click();QTRY_COMPARE(source.reads,1);QTRY_VERIFY(!window->isEnabled());QVERIFY(!window->isVisible());QVERIFY(device.last("closeRequested").isEmpty());QCOMPARE(exits.size(),0);device.send("checkpointAccepted",{{"replyTo","unknown"}});QVERIFY(device.last("closeRequested").isEmpty());device.send("checkpointAccepted",{{"replyTo",checkpoint["sequence"]}});QTRY_VERIFY(!device.last("closeRequested").isEmpty());const auto close=device.last("closeRequested");device.send("closeAccepted",{{"replyTo",close["sequence"]}});QCOMPARE(exits.size(),1);QCOMPARE(device.last("closeConfirmed")["payload"].toObject()["replyTo"],close["sequence"]);
 }
 void screenRailActionKeepsUnstoredCredentialsInTheirWindow_data(){QTest::addColumn<bool>("cached");QTest::newRow("active-destination")<<false;QTest::newRow("other-destination")<<true;}
 void screenRailActionKeepsUnstoredCredentialsInTheirWindow(){
  QFETCH(bool,cached);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto other=profiles.first().toObject();other["id"]="synthetic-alternate";other["label"]="Synthetic alternate";profiles.append(other);document["profiles"]=profiles;QVERIFY(settings.save(document,{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","destinations"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);auto* secret=window->findChild<QLineEdit*>("credentialSecretKey");secret->setText("synthetic-unfinished-input");if(cached)window->findChild<QListWidget*>("destinationList")->setCurrentRow(1);int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke")++invoked;};QPushButton* button=nullptr;for(auto* candidate:window->findChildren<QPushButton*>())if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();QTest::qWait(300);QCOMPARE(invoked,0);QVERIFY(window->isVisible());if(cached)window->findChild<QListWidget*>("destinationList")->setCurrentRow(0);QCOMPARE(secret->text(),QString("synthetic-unfinished-input"));QVERIFY(device.last("closeRequested").isEmpty());
 }
 void clearedCredentialsAfterDestinationRoundTripAllowScreenCapture(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto other=profiles.first().toObject();other["id"]="synthetic-alternate";other["label"]="Synthetic alternate";profiles.append(other);document["profiles"]=profiles;QVERIFY(settings.save(document,{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","destinations"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);auto* secret=window->findChild<QLineEdit*>("credentialSecretKey");secret->setText("synthetic-unfinished-input");auto* destinations=window->findChild<QListWidget*>("destinationList");destinations->setCurrentRow(1);destinations->setCurrentRow(0);QCOMPARE(secret->text(),QString("synthetic-unfinished-input"));secret->clear();int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke"){++invoked;fixture.client.failOnce[method]={"fixture_rejected","Synthetic rejection",{}};}};QPushButton* button=nullptr;for(auto* candidate:window->findChildren<QPushButton*>())if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();QTRY_COMPARE(invoked,1);QTRY_VERIFY(window->isVisible());
 }
 void failedSaveDoesNotLaunchADeferredRailAction(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);fixture.client.failOnce["settings.save"]={"config_locked","Synthetic save failure",{}};const auto output=dir.filePath("retry-before-capture");window->findChild<QLineEdit*>("archiveRoot")->setText(output);int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke"){++invoked;fixture.client.failOnce[method]={"fixture_rejected","Synthetic rejection",{}};}};QPushButton* button=nullptr;for(auto* candidate:window->findChildren<QPushButton*>())if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();QTRY_VERIFY(window->settings()->saveViewState()["autosaveBlocked"].toBool());QTest::qWait(300);QCOMPARE(invoked,0);QVERIFY(window->isVisible());for(auto* widget:QApplication::topLevelWidgets())if(auto* notice=qobject_cast<QMessageBox*>(widget))notice->close();window->findChild<QPushButton*>("retryAutosave")->click();QTRY_COMPARE(settings.document()["archive"].toObject()["root"].toString(),output);QTest::qWait(300);QCOMPARE(invoked,0);button->click();QTRY_COMPARE(invoked,1);QTRY_VERIFY(window->isVisible());
 }
 void closingOrQuittingDuringRailSaveCancelsOnlyThePendingInvocation_data(){QTest::addColumn<bool>("quit");QTest::newRow("close")<<false;QTest::newRow("quit")<<true;}
 void closingOrQuittingDuringRailSaveCancelsOnlyThePendingInvocation(){
  QFETCH(bool,quit);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);fixture.client.replyDelayMs["settings.save"]=300;ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke")++invoked;};const auto output=dir.filePath("saved-without-capture");window->findChild<QLineEdit*>("archiveRoot")->setText(output);QPushButton* button=nullptr;for(auto* candidate:window->findChildren<QPushButton*>())if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();bool stopped=false;fixture.server.setUiQuit([&]{stopped=true;});if(quit)window->findChild<QPushButton*>("quitApplication")->click();else window->close();QTRY_COMPARE(settings.document()["archive"].toObject()["root"].toString(),output);if(quit)QTRY_VERIFY(stopped);else QTRY_VERIFY(!device.last("closeRequested").isEmpty());QTest::qWait(350);QCOMPARE(invoked,0);
 }
 void finalCloseWaitsForAnOutstandingLargeCheckpoint(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  const QJsonObject activity{{"selectedId",QString(6*1024*1024,'x')},{"selectedItemId",""},{"collapsedIds",QJsonArray{}},{"geometry",QJsonArray{0,0,500,400}}};
  const QJsonObject state{{"schemaVersion",1},{"activity",activity}};
  ControlDevice device;UiControl control(device);QSignalSpy failures(&control,&UiControl::failed);UiApplicationController controller(fixture.session,control,state);int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});
  device.send("activate",{{"view","about"}});AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;QVERIFY(about);
  device.writeLimit=0;about->move(25,25);QTest::qWait(350);about->close();QCOMPARE(failures.size(),0);QCOMPARE(exits,0);QVERIFY(!about->isEnabled());
  device.writeLimit=16*1024*1024;emit device.bytesWritten(0);const auto checkpoint=device.last("checkpoint");QVERIFY(!checkpoint.isEmpty());QVERIFY(device.last("closeRequested").isEmpty());
  device.send("checkpointAccepted",{{"replyTo",checkpoint["sequence"]}});const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QCOMPARE(failures.size(),0);
  // The checkpoint also reports the appearance in use (#60); the rest is the saved state.
  auto sent=request["payload"].toObject()["state"].toObject();QCOMPARE(sent.take("appearance").toObject()["theme"].toString(),QString("system"));QCOMPARE(sent,state);
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
