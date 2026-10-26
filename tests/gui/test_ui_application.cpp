#include "ui/UiApplicationController.h"
#include "ui/SettingsWindow.h"
#include "ui/MainWindow.h"
#include "ui/HistoryWindow.h"
#include "ui/AboutWindow.h"
#include "ui/Theme.h"
#include "ui/VisualStyle.h"
#include "ui/ViewState.h"
#include <QScopeGuard>
#include "fixtures/UiSessionFixture.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QTabBar>
#include <QMenu>
#include <QStackedWidget>
#include <QToolButton>
#include <QPushButton>
#include <QAction>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QStyledItemDelegate>
#include <QtTest>
#include <QTimer>
#include <QStatusBar>
#include <QStyleOptionToolButton>
#include "ui/Graphite.h"
#include "fixtures/Waits.h"
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
 // #558: opening and checkpointing History must not construct the hidden
 // Settings tree; explicit navigation and agent commands still initialize it.
 void historyDefersCleanSettingsAndUsesCurrentConfiguration(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();QVERIFY(settings.save(document,{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);QJsonObject saved;
  {
   MainWindow prior(fixture.session);prior.navigate("settings");
   saved={{"schemaVersion",1},{"settings",prior.settings()->saveViewState()},{"main",prior.saveViewState()}};
   QVERIFY(!saved["settings"].toObject()["dirty"].toBool());
  }
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,saved);
  device.send("activate",{{"view","history"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
  QVERIFY(window);QVERIFY(!window->findChild<SettingsWindow*>());QCOMPARE(window->saveViewState()["page"].toInt(),0);
  PUTGD_TRY_VERIFY(!device.last("checkpoint").isEmpty());
  auto retained=saved["settings"].toObject();retained.remove("captureRouting");QCOMPARE(device.last("checkpoint")["payload"].toObject()["state"].toObject()["settings"].toObject(),retained);QVERIFY(!window->findChild<SettingsWindow*>());
  auto archive=document["archive"].toObject();archive["root"]=dir.filePath("changed-before-settings");document["archive"]=archive;QVERIFY(settings.save(document,settings.revision()));
  PUTGD_TRY_COMPARE(fixture.session.revision(),settings.revision());QVERIFY(!window->findChild<SettingsWindow*>());
  device.send("activate",{{"view","settings"}});QVERIFY(window->findChild<SettingsWindow*>());
  QCOMPARE(window->findChild<QLineEdit*>("archiveRoot")->text(),archive["root"].toString());
  auto* instance=window->findChild<SettingsWindow*>();device.send("activate",{{"view","destinations"}});QCOMPARE(window->findChild<SettingsWindow*>(),instance);QCOMPARE(window->saveViewState()["page"].toInt(),1);
  device.send("activate",{{"view","history"}});QCOMPARE(window->findChild<SettingsWindow*>(),instance);
 }
 void historyCloseDoesNotAllocateSettings(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","history"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
  QVERIFY(window);QVERIFY(!window->findChild<SettingsWindow*>());window->close();
  PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());QVERIFY(!window->findChild<SettingsWindow*>());
  QVERIFY(!device.last("closeRequested")["payload"].toObject()["state"].toObject().contains("settings"));
 }
 void historyRestoresActiveSettingsDraft(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);QJsonObject saved;
  {
   MainWindow prior(fixture.session);auto state=prior.settings()->saveViewState();state["dirty"]=true;state["autosaveBlocked"]=true;auto draft=state["draft"].toObject();auto archive=draft["archive"].toObject();archive["root"]=dir.filePath("retained-draft");draft["archive"]=archive;state["draft"]=draft;saved={{"schemaVersion",1},{"settings",state}};
  }
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,saved);device.send("activate",{{"view","history"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
  QVERIFY(window);QVERIFY(window->findChild<SettingsWindow*>());QVERIFY(window->findChild<SettingsWindow*>()->saveViewState()["dirty"].toBool());QCOMPARE(window->findChild<QLineEdit*>("archiveRoot")->text(),dir.filePath("retained-draft"));
 }
 void historyRestoresProvisionalDestinationAndSetupPolling(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);QJsonObject saved;
  {
   MainWindow prior(fixture.session);auto state=prior.settings()->saveViewState();const auto profile=state["draft"].toObject()["profiles"].toArray().first();const auto editor=state["profileEditors"].toArray().first();
   state["provisionalProfile"]=QJsonObject{{"profile",profile},{"editor",editor}};
   state["setupJobs"]=QJsonArray{QJsonObject{{"setupId","synthetic-restored-setup"},{"kind","testDestination"},{"profileId","development-s3"},{"revision",settings.revision()}}};saved={{"schemaVersion",1},{"settings",state}};QVERIFY(!validateUiState(saved));
  }
  bool inspected=false;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="setup.inspect"&&params["id"]=="synthetic-restored-setup")inspected=true;};
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,saved);device.send("activate",{{"view","history"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);QVERIFY(window->findChild<SettingsWindow*>());
  QCOMPARE(window->settings()->saveViewState()["provisionalProfile"],saved["settings"].toObject()["provisionalProfile"]);PUTGD_TRY_VERIFY(inspected);
 }
 void agentSettingsCommandInitializesFromHistory(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","history"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);QVERIFY(!window->findChild<SettingsWindow*>());
  device.send("command",{{"version",1},{"command","moveDestination"},{"params",QJsonObject{{"direction","down"},{"profileId","development-s3"}}}});
  QVERIFY(window->findChild<SettingsWindow*>());QCOMPARE(device.last("commandResult")["payload"].toObject()["error"].toObject()["code"].toString(),QString("destination_move_invalid"));QCOMPARE(window->saveViewState()["page"].toInt(),0);
 }
 // #60: the appearance setting, saved here or patched by the CLI/MCP, switches
 // the scheme of the running interface without a restart.
 void appearanceSettingAppliesLive(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();QVERIFY(settings.save(document,{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  auto restore=qScopeGuard([]{Theme::setScheme(Theme::Scheme::Dark);});
  QCOMPARE(Theme::scheme(),Theme::Scheme::Dark);
  auto preferences=document["preferences"].toObject();preferences["appearance"]=QJsonObject{{"theme","light"}};document["preferences"]=preferences;QVERIFY(settings.save(document,settings.revision()));
  fixture.session.invalidate();PUTGD_TRY_COMPARE(Theme::scheme(),Theme::Scheme::Light);
  // The switch reaches a checkpoint, so ui.viewState reports it.
  PUTGD_TRY_VERIFY(device.last("checkpoint")["payload"].toObject()["state"].toObject()["appearance"].toObject()["scheme"]==QString("light"));
  preferences["appearance"]=QJsonObject{{"theme","system"}};document["preferences"]=preferences;QVERIFY(settings.save(document,settings.revision()));
  fixture.session.invalidate();PUTGD_TRY_COMPARE(Theme::scheme(),Theme::Scheme::Dark);
 }
 // #261: a window switched into a scheme lays its rail out exactly like a
 // window opened in that scheme (the capture buttons sat 2 px lower).
 void switchedRailMatchesAFreshWindow_data(){QTest::addColumn<int>("from");QTest::addColumn<int>("to");QTest::newRow("dark-to-light")<<int(Theme::Scheme::Dark)<<int(Theme::Scheme::Light);QTest::newRow("light-to-dark")<<int(Theme::Scheme::Light)<<int(Theme::Scheme::Dark);}
 void switchedRailMatchesAFreshWindow(){
  QFETCH(int,from);QFETCH(int,to);
  // As putgd-ui starts: the application style first, then the window's own.
  VisualStyle::apply(*qobject_cast<QApplication*>(QCoreApplication::instance()));
  auto restore=qScopeGuard([]{Theme::setScheme(Theme::Scheme::Dark);});
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  const auto layout=[](MainWindow& window){
   QStringList rows;auto* rail=window.findChild<QWidget*>("commandBand");if(!rail)return rows;
   for(auto* child:rail->findChildren<QWidget*>())if(child->isVisible()){
    const auto r=QRect(child->mapTo(rail,QPoint(0,0)),child->size());
    rows<<QStringLiteral("%1 %2 %3,%4 %5x%6 font %7").arg(QLatin1String(child->metaObject()->className()),child->objectName()).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(child->font().toString());
   }
   return rows;
  };
  // A band button holds the focus, as in putgd-ui: its focus border must not
  // change its height.
  const auto focusHelp=[](MainWindow& window){if(auto* button=window.findChild<QWidget*>("captureActionButton"))button->setFocus();};
  Theme::setScheme(Theme::Scheme(to));
  MainWindow fresh(fixture.session);fresh.resize(1320,880);fresh.show();QVERIFY(putgd::test::waitForWindowExposed(&fresh));focusHelp(fresh);putgd::test::settle(50);
  const auto expected=layout(fresh);fresh.hide();QVERIFY(!expected.isEmpty());
  Theme::setScheme(Theme::Scheme(from));
  MainWindow switched(fixture.session);switched.resize(1320,880);switched.show();QVERIFY(putgd::test::waitForWindowExposed(&switched));focusHelp(switched);putgd::test::settle(50);
  Theme::setScheme(Theme::Scheme(to));putgd::test::settle(50);
  const auto actual=layout(switched);
  for(int i=0;i<qMax(actual.size(),expected.size());++i)if(actual.value(i)!=expected.value(i))qWarning("rail %d: fresh [%s] switched [%s]",i,qPrintable(expected.value(i)),qPrintable(actual.value(i)));
  QCOMPARE(actual,expected);
 }
 // #237: a theme picked in Settings applies at once, before the resident
 // accepts the save; a refused save puts the accepted theme back.
 void pickedThemeAppliesBeforeTheSaveAndRevertsWhenRefused(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  auto restore=qScopeGuard([]{Theme::setScheme(Theme::Scheme::Dark);});
  device.send("activate",{{"view","settings"}});QVERIFY(!device.last("shown").isEmpty());
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* main=qobject_cast<MainWindow*>(widget))window=main;QVERIFY(window);
  auto* theme=window->findChild<QComboBox*>("appearanceTheme");QVERIFY(theme);QCOMPARE(Theme::scheme(),Theme::Scheme::Dark);
  bool residentSawLight=false;QObject::connect(&settings,&SettingsService::accepted,this,[&]{if(settings.snapshot().appearanceTheme=="light")residentSawLight=Theme::scheme()==Theme::Scheme::Light;});
  fixture.client.replyDelayMs["settings.save"]=300;
  // The 400 ms autosave timer, which a pick must not wait for.
  QTimer* autosave=nullptr;for(auto* timer:window->settings()->findChildren<QTimer*>())if(timer->isSingleShot()&&timer->interval()==400)autosave=timer;QVERIFY(autosave);
  // The save must reach the resident while the autosave timer is still
  // pending: Qt fires the dispatch's zero timer before a 400 ms one however
  // late the event loop runs, so this is an ordering, not a time limit.
  int saves=0;bool savedBeforeAutosave=false;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="settings.save"&&saves++==0)savedBeforeAutosave=autosave->isActive();};
  theme->setCurrentIndex(theme->findData("light"));
  // Applied in the same event, before the resident has seen the save.
  QCOMPARE(Theme::scheme(),Theme::Scheme::Light);QCOMPARE(settings.snapshot().appearanceTheme,QString("system"));
  PUTGD_TRY_VERIFY(saves>0);QVERIFY(savedBeforeAutosave);PUTGD_TRY_COMPARE(settings.snapshot().appearanceTheme,QString("light"));QVERIFY(residentSawLight);
  PUTGD_TRY_VERIFY(!window->settings()->saveViewState()["dirty"].toBool());QCOMPARE(Theme::scheme(),Theme::Scheme::Light);
  // Refused: the accepted theme comes back and the autosave error shows.
  fixture.client.replyDelayMs.clear();fixture.client.failOnce["settings.save"]={"config_write_failed","Synthetic refused save",{}};
  theme->setCurrentIndex(theme->findData("dark"));QCOMPARE(Theme::scheme(),Theme::Scheme::Dark);
  PUTGD_TRY_COMPARE(Theme::scheme(),Theme::Scheme::Light);PUTGD_TRY_VERIFY(window->findChild<QLabel*>("settingsStatus")->text().contains("Synthetic refused save"));
  QCOMPARE(settings.snapshot().appearanceTheme,QString("light"));QVERIFY(window->settings()->saveViewState()["autosaveBlocked"].toBool());
  // Retry saving applies the kept pick through the accepted revision.
  window->findChild<QPushButton*>("retryAutosave")->click();PUTGD_TRY_COMPARE(settings.snapshot().appearanceTheme,QString("dark"));PUTGD_TRY_COMPARE(Theme::scheme(),Theme::Scheme::Dark);
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
  PUTGD_TRY_COMPARE(settings.document()["profiles"].toArray().first().toObject()["id"].toString(),QString("synthetic-alternate"));
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
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(completions,1,14000);QCOMPARE(error.code,QString("shortcut_recording_timeout"));
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
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;std::function<void(Error)> complete;UiSessionFixture fixture(settings,secrets,[&](auto,auto,auto reply){complete=reply;});ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);window->findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(complete));const auto output=dir.filePath("saved-before-close");window->findChild<QLineEdit*>("archiveRoot")->setText(output);window->close();putgd::test::settle(500);PUTGD_TRY_VERIFY(device.last("closeRequested").isEmpty());PUTGD_TRY_VERIFY(window->isVisible());complete({});PUTGD_TRY_COMPARE(settings.document()["archive"].toObject()["root"].toString(),output);PUTGD_TRY_VERIFY(!window->settings()->saveViewState().contains("stateError"));
 }
 void explicitQuitFlushesFinishedEditsBeforeStoppingTheResident_data(){QTest::addColumn<bool>("setupPending");QTest::newRow("debounce")<<false;QTest::newRow("accepted-setup")<<true;}
 void explicitQuitFlushesFinishedEditsBeforeStoppingTheResident(){
  QFETCH(bool,setupPending);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;std::function<void(Error)> complete;UiSessionFixture fixture(settings,secrets,[&](auto,auto,auto reply){complete=reply;});ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);
  if(setupPending){window->findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(complete));}const auto output=dir.filePath("saved-before-quit");fixture.client.replyDelayMs["settings.save"]=350;bool quit=false;QString savedAtQuit;fixture.server.setUiQuit([&]{quit=true;savedAtQuit=settings.document()["archive"].toObject()["root"].toString();});window->findChild<QLineEdit*>("archiveRoot")->setText(output);window->findChild<QAction*>("quitApplication")->trigger();
  if(setupPending){putgd::test::settle(500);PUTGD_TRY_VERIFY(!quit);complete({});}PUTGD_TRY_VERIFY(quit);QCOMPARE(savedAtQuit,output);
 }
 void allMainPagesRemainVisibleWithNativeStyleRowMetrics(){
  // Graphite: the pages are one centred segmented control in the header.
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);MainWindow window(fixture.session);auto* navigation=window.findChild<QTabBar*>("mainNavigation");QVERIFY(navigation);window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  for(int row=0;row<navigation->count();++row){navigation->setCurrentIndex(row);PUTGD_TRY_VERIFY(navigation->rect().contains(navigation->tabRect(row)));PUTGD_TRY_COMPARE(window.findChild<QStackedWidget*>("mainPages")->currentIndex(),row);}
  QCOMPARE(QStringList({navigation->tabText(0),navigation->tabText(1),navigation->tabText(2)}),QStringList({"History","Destinations","Settings"}));
  // Centred in the window header.
  const int centre=navigation->mapTo(&window,navigation->rect().center()).x();QVERIFY2(std::abs(centre-window.width()/2)<=4,qPrintable(QString::number(centre)));
 }
 void railActionsFlushSettingsAndHideScreenWindowsBeforeDispatch_data(){QTest::addColumn<QString>("action");for(const auto& name:Settings::actionNames())QTest::newRow(qPrintable(name))<<name;}
 void railActionsFlushSettingsAndHideScreenWindowsBeforeDispatch(){
  QFETCH(QString,action);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);fixture.client.replyDelayMs["settings.save"]=300;
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});device.send("activate",{{"view","about"}});MainWindow* window=nullptr;AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets()){if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;}QVERIFY(window&&about);
  const auto output=dir.filePath("saved-before-rail");window->findChild<QLineEdit*>("archiveRoot")->setText(output);int invoked=0;bool mainVisible=false,aboutVisible=false;QString savedAtInvocation;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method!="action.invoke")return;++invoked;mainVisible=window->isVisible();aboutVisible=about->isVisible();savedAtInvocation=settings.document()["archive"].toObject()["root"].toString();fixture.client.failOnce[method]={"fixture_capture_rejected","Synthetic rejection",{}};};
  QToolButton* button=nullptr;for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")==action)button=candidate;QVERIFY(button);button->click();button->click();PUTGD_TRY_COMPARE(invoked,1);QCOMPARE(savedAtInvocation,output);QCOMPARE(mainVisible,action=="clipboard");QCOMPARE(aboutVisible,action=="clipboard");PUTGD_TRY_VERIFY(window->isVisible()&&about->isVisible());QVERIFY(device.last("closeRequested").isEmpty());
 }
 void screenRailActionWaitsForAnOpenDialogToBeDismissed(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","history"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);QDialog dialog(window);dialog.setWindowModality(Qt::NonModal);dialog.show();int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke"){++invoked;fixture.client.failOnce[method]={"fixture_rejected","Synthetic rejection",{}};}};QToolButton* button=nullptr;for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();putgd::test::settle(300);PUTGD_TRY_COMPARE(invoked,0);PUTGD_TRY_VERIFY(window->isVisible()&&dialog.isVisible());dialog.reject();button->click();PUTGD_TRY_COMPARE(invoked,1);PUTGD_TRY_VERIFY(window->isVisible());
 }
 void acceptedScreenRailActionClosesTheHiddenHelperAfterCheckpoint(){
  QTemporaryDir dir;ProviderRegistry registry;SettingsService settings(dir.filePath("config/config.json"));auto document=Settings::defaults();auto archive=document["archive"].toObject();archive["keepUploads"]=true;archive["root"]=dir.filePath("archive");document["archive"]=archive;QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();OperationService operations(dir.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});QSignalSpy exits(&controller,&UiApplicationController::exitRequested);device.send("activate",{{"view","history"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);QToolButton* button=nullptr;for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);window->move(25,25);PUTGD_TRY_VERIFY(!device.last("checkpoint").isEmpty());const auto checkpoint=device.last("checkpoint");button->click();PUTGD_TRY_COMPARE(source.reads,1);PUTGD_TRY_VERIFY(!window->isEnabled());QVERIFY(!window->isVisible());QVERIFY(device.last("closeRequested").isEmpty());QCOMPARE(exits.size(),0);device.send("checkpointAccepted",{{"replyTo","unknown"}});QVERIFY(device.last("closeRequested").isEmpty());device.send("checkpointAccepted",{{"replyTo",checkpoint["sequence"]}});PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());const auto close=device.last("closeRequested");device.send("closeAccepted",{{"replyTo",close["sequence"]}});QCOMPARE(exits.size(),1);QCOMPARE(device.last("closeConfirmed")["payload"].toObject()["replyTo"],close["sequence"]);
 }
 void screenRailActionKeepsUnstoredCredentialsInTheirWindow_data(){QTest::addColumn<bool>("cached");QTest::newRow("active-destination")<<false;QTest::newRow("other-destination")<<true;}
 void screenRailActionKeepsUnstoredCredentialsInTheirWindow(){
  QFETCH(bool,cached);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto other=profiles.first().toObject();other["id"]="synthetic-alternate";other["label"]="Synthetic alternate";profiles.append(other);document["profiles"]=profiles;QVERIFY(settings.save(document,{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","destinations"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);auto* secret=window->findChild<QLineEdit*>("credentialSecretKey");secret->setText("synthetic-unfinished-input");if(cached)window->findChild<QListWidget*>("destinationList")->setCurrentRow(1);int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke")++invoked;};QToolButton* button=nullptr;for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();putgd::test::settle(300);PUTGD_TRY_COMPARE(invoked,0);PUTGD_TRY_VERIFY(window->isVisible());if(cached)window->findChild<QListWidget*>("destinationList")->setCurrentRow(0);QCOMPARE(secret->text(),QString("synthetic-unfinished-input"));QVERIFY(device.last("closeRequested").isEmpty());
 }
 void clearedCredentialsAfterDestinationRoundTripAllowScreenCapture(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto other=profiles.first().toObject();other["id"]="synthetic-alternate";other["label"]="Synthetic alternate";profiles.append(other);document["profiles"]=profiles;QVERIFY(settings.save(document,{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","destinations"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);auto* secret=window->findChild<QLineEdit*>("credentialSecretKey");secret->setText("synthetic-unfinished-input");auto* destinations=window->findChild<QListWidget*>("destinationList");destinations->setCurrentRow(1);destinations->setCurrentRow(0);QCOMPARE(secret->text(),QString("synthetic-unfinished-input"));secret->clear();int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke"){++invoked;fixture.client.failOnce[method]={"fixture_rejected","Synthetic rejection",{}};}};QToolButton* button=nullptr;for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();PUTGD_TRY_COMPARE(invoked,1);PUTGD_TRY_VERIFY(window->isVisible());
 }
 void failedSaveDoesNotLaunchADeferredRailAction(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);fixture.client.failOnce["settings.save"]={"config_locked","Synthetic save failure",{}};const auto output=dir.filePath("retry-before-capture");window->findChild<QLineEdit*>("archiveRoot")->setText(output);int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke"){++invoked;fixture.client.failOnce[method]={"fixture_rejected","Synthetic rejection",{}};}};QToolButton* button=nullptr;for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();PUTGD_TRY_VERIFY(window->settings()->saveViewState()["autosaveBlocked"].toBool());putgd::test::settle(300);PUTGD_TRY_COMPARE(invoked,0);PUTGD_TRY_VERIFY(window->isVisible());for(auto* widget:QApplication::topLevelWidgets())if(auto* notice=qobject_cast<QMessageBox*>(widget))notice->close();window->findChild<QPushButton*>("retryAutosave")->click();PUTGD_TRY_COMPARE(settings.document()["archive"].toObject()["root"].toString(),output);putgd::test::settle(300);PUTGD_TRY_COMPARE(invoked,0);button->click();PUTGD_TRY_COMPARE(invoked,1);PUTGD_TRY_VERIFY(window->isVisible());
 }
 void closingOrQuittingDuringRailSaveCancelsOnlyThePendingInvocation_data(){QTest::addColumn<bool>("quit");QTest::newRow("close")<<false;QTest::newRow("quit")<<true;}
 void closingOrQuittingDuringRailSaveCancelsOnlyThePendingInvocation(){
  QFETCH(bool,quit);QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);fixture.client.replyDelayMs["settings.save"]=300;ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});device.send("activate",{{"view","settings"}});MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);int invoked=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="action.invoke")++invoked;};const auto output=dir.filePath("saved-without-capture");window->findChild<QLineEdit*>("archiveRoot")->setText(output);QToolButton* button=nullptr;for(auto* candidate:window->findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")=="region")button=candidate;QVERIFY(button);button->click();bool stopped=false;fixture.server.setUiQuit([&]{stopped=true;});if(quit)window->findChild<QAction*>("quitApplication")->trigger();else window->close();PUTGD_TRY_COMPARE(settings.document()["archive"].toObject()["root"].toString(),output);if(quit)PUTGD_TRY_VERIFY(stopped);else PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());putgd::test::settle(350);PUTGD_TRY_COMPARE(invoked,0);
 }
 void finalCloseWaitsForAnOutstandingLargeCheckpoint(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  const QJsonObject activity{{"selectedId",QString(6*1024*1024,'x')},{"selectedItemId",""},{"collapsedIds",QJsonArray{}},{"geometry",QJsonArray{0,0,500,400}}};
  const QJsonObject state{{"schemaVersion",1},{"activity",activity}};
  ControlDevice device;UiControl control(device);QSignalSpy failures(&control,&UiControl::failed);UiApplicationController controller(fixture.session,control,state);int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});
  device.send("activate",{{"view","about"}});AboutWindow* about=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=dynamic_cast<AboutWindow*>(widget))about=candidate;QVERIFY(about);
  device.writeLimit=0;about->move(25,25);putgd::test::settle(350);about->close();QCOMPARE(failures.size(),0);QCOMPARE(exits,0);QVERIFY(!about->isEnabled());
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
  PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());QCOMPARE(exits,0);
  device.send("closeAccepted",{{"replyTo",device.last("closeRequested")["sequence"]}});QCOMPARE(exits,1);
 }
 void closeRejectionRetainsTheDraftAndAllowsRetry(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  int exits=0;connect(&controller,&UiApplicationController::exitRequested,this,[&]{++exits;});device.send("activate",{{"view","settings"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
  QVERIFY(window);window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/retained-draft");
  window->findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-secret-never-checkpointed");window->close();
  PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());const auto request=device.last("closeRequested");QVERIFY(!request.isEmpty());QVERIFY(!device.output.contains("synthetic-secret"));
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
  QVERIFY(window&&about);window->showMinimized();window->close();QVERIFY(window->isHidden());about->accept();PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());
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
  window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/first-draft");window->close();PUTGD_TRY_VERIFY(!device.last("closeRequested").isEmpty());const auto request=device.last("closeRequested");
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(!device.last("closeCancel").isEmpty(),6000);QVERIFY(!window->isEnabled());
  device.send("closeAccepted",{{"replyTo",request["sequence"]}});QCOMPARE(exits,0);QVERIFY(!window->isEnabled());QVERIFY(device.last("closeConfirmed").isEmpty());
  device.send("closeCancelled",{{"replyTo",device.last("closeCancel")["sequence"]}});QVERIFY(window->isEnabled());QCOMPARE(exits,0);
  for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->close();
  QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  window->findChild<QLineEdit*>("archiveRoot")->setText("/tmp/newer-draft");window->close();PUTGD_TRY_VERIFY(device.last("closeRequested")["sequence"]!=request["sequence"]);const auto next=device.last("closeRequested");QVERIFY(next["sequence"]!=request["sequence"]);QVERIFY(QJsonDocument(next).toJson().contains("newer-draft"));
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
 // #348: the first ui.show updates lands on Updates although the fresh window
 // settles smaller than it opened (Aurora's 1280×800 screen) and the update's
 // rows arrive after it is shown. A scroll of the user's own then wins.
 // The tray's "Recent activity" entry is gone; the view name stays an alias for History.
 void activityViewNameOpensHistory(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","settings"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);
  auto* navigation=window->findChild<QTabBar*>("mainNavigation");QVERIFY(navigation);PUTGD_TRY_COMPARE(navigation->currentIndex(),2);
  device.send("activate",{{"view","activity"}});
  PUTGD_TRY_COMPARE(navigation->currentIndex(),0);QCOMPARE(navigation->tabText(0),QString("History"));PUTGD_TRY_VERIFY(window->history()->isVisible());
  const auto windows=QApplication::topLevelWidgets();QCOMPARE(std::count_if(windows.begin(),windows.end(),[](QWidget* widget){return qobject_cast<MainWindow*>(widget)!=nullptr;}),1);
 }
 void showUpdatesLandsOnUpdatesInAFreshWindow(){
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);QJsonObject update;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();if(!update.isEmpty())data["update"]=update;return data;});
  ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
  device.send("activate",{{"view","updates"}});
  MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;QVERIFY(window);
  auto* status=window->findChild<QLabel*>("updateStatus");auto* install=window->findChild<QPushButton*>("installUpdate");auto* tabs=window->findChild<QTabWidget*>("settingsTabs");QVERIFY(status&&install&&tabs);
  auto* scroll=qobject_cast<QScrollArea*>(tabs->widget(0));QVERIFY(scroll);auto* bar=scroll->verticalScrollBar();
  const auto inView=[&](QWidget* widget){return widget->isVisible()&&scroll->viewport()->rect().contains(QRect(widget->mapTo(scroll->viewport(),QPoint()),widget->size()));};
  PUTGD_TRY_VERIFY(window->isVisible());PUTGD_TRY_VERIFY(fixture.session.ready());PUTGD_TRY_VERIFY(inView(status));
  // The window manager fits it to the screen after it was shown.
  window->resize(1280,760);PUTGD_TRY_COMPARE(window->height(),760);PUTGD_TRY_VERIFY(bar->maximum()>0);
  PUTGD_TRY_VERIFY2(inView(status),"Updates stays in view after the window settles smaller");
  // The update arrives: its actions add rows to the group, still in view.
  update={{"supported",true},{"build",QJsonObject{{"version","0.1.1-nightly.886"},{"channel","nightly"},{"supported",true},{"packageTarget","appimage"}}},
   {"check",true},{"channelSetting","build"},{"channel","nightly"},{"checking",false},{"action",QJsonObject{{"kind","download"},{"repository","none"}}},{"error",QJsonObject{}},
   {"checkedAt",QJsonValue()},{"nextCheckAt",QJsonValue()},
   {"result",QJsonObject{{"channel","nightly"},{"current","0.1.1-nightly.886"},{"latest","0.1.1-nightly.887"},{"updateAvailable",true},
    {"downloadUrl","https://github.com/patrick-hudson/put.gd-desktop/releases/download/v0.1.1-nightly.887/putgd-0.1.1-nightly.887-x86_64.AppImage"}}},
   {"install",QJsonObject{{"state","idle"},{"available",true},{"running",false},{"restartRequired",false}}}};
  fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(install->isVisible());PUTGD_TRY_VERIFY(inView(status));PUTGD_TRY_VERIFY(inView(install));
  // Once the user scrolls, a later change of size leaves the page where they put it.
  bar->triggerAction(QAbstractSlider::SliderToMinimum);QCOMPARE(bar->value(),0);
  window->resize(1280,700);PUTGD_TRY_COMPARE(window->height(),700);PUTGD_COMPARE_STEADY(bar->value(),0,300);QVERIFY(!inView(status));
 }
 // #391: the rail at the heights a maximized window gets on short screens,
 // with Update available and a three-line capture output. Nothing in it
 // overlaps or is squeezed below its minimum, and the navigation stays in view.
 // At 1280×800 the whole rail fits; at 768 px it may scroll a little,
 // depending on the fonts, and below the window's minimum height, where a
 // window manager can still hold it, the rest scrolls into view.
 void railFitsShortWindows_data(){
  QTest::addColumn<QSize>("size");QTest::addColumn<int>("scrolls");
  enum{Never,Maybe,Always};
  QTest::newRow("natural")<<QSize(1320,880)<<int(Never);
  QTest::newRow("1280x800-gnome")<<QSize(1280,738)<<int(Never);
  QTest::newRow("1280x800-plasma")<<QSize(1280,722)<<int(Never);
  QTest::newRow("1366x768")<<QSize(1366,690)<<int(Maybe);
  QTest::newRow("1024x768-minimum")<<QSize(1060,680)<<int(Maybe);
  QTest::newRow("below-minimum")<<QSize(1060,560)<<int(Always);
 }
 void railFitsShortWindows(){
  QFETCH(QSize,size);QFETCH(int,scrolls);
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});auto document=Settings::developmentTemplate();
  document["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","default"}};auto actions=document["actions"].toObject();
  for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["destination"]=name=="region"?"local":"default";action["profileId"]=QJsonValue::Null;actions[name]=action;}document["actions"]=actions;QVERIFY(settings.save(document,{}));
  CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=QJsonObject{{"supported",true},{"result",QJsonObject{{"updateAvailable",true},{"latest","26.1022.999-nightly"}}}};return data;});
  MainWindow window(fixture.session);if(size.height()<window.minimumHeight())window.setMinimumSize(size);window.resize(size);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));fixture.session.refreshSnapshot();
  // A nightly links its version to its release; the longest CalVer one.
  QJsonObject snapshot;fixture.server.dispatch({{"schemaVersion",1},{"requestId","rail"},{"session",fixture.server.socketPath()},{"method","ui.snapshot"},{"params",QJsonObject{}}},[&](Error e,QJsonObject data){QVERIFY(!e);snapshot=data;});PUTGD_TRY_VERIFY(!snapshot.isEmpty());
  auto build=snapshot["build"].toObject();build["version"]="26.1231.99999-nightly";build["dirty"]=false;snapshot["build"]=build;fixture.session.applySnapshot(snapshot);
  auto* version=window.findChild<QPushButton*>("appVersionLink");QVERIFY(version);PUTGD_TRY_VERIFY(version->isVisible());
  auto* update=window.findChild<QPushButton*>("updateAvailable");auto* output=window.findChild<QLabel*>("captureOutputSummary");auto* navigation=window.findChild<QTabBar*>("mainNavigation");auto* band=window.findChild<QWidget*>("commandBand");
  QVERIFY(update&&output&&navigation&&band);PUTGD_TRY_VERIFY(update->isVisible());PUTGD_TRY_COMPARE(window.size(),size);QVERIFY2(!output->text().contains('\n')&&output->text().contains("1 action exception"),qPrintable(output->text()));
  Q_UNUSED(scrolls);
  // Every capture action keeps its own visible button at every width (narrow
  // bands show icons, named in tooltips); Capture region has no duplicate menu.
  QStringList reachable;QToolButton* region=nullptr;
  for(auto* button:window.findChildren<QToolButton*>("captureActionButton")){if(button->isVisible())reachable<<button->property("captureAction").toString();if(button->property("captureAction")=="region")region=button;if(button->toolButtonStyle()==Qt::ToolButtonIconOnly)QVERIFY(!button->accessibleName().isEmpty()&&!button->toolTip().isEmpty());}
  QVERIFY(region&&!region->menu());QCOMPARE(region->toolButtonStyle(),Qt::ToolButtonTextBesideIcon);
  for(const auto& name:Settings::invocableActionNames())QVERIFY2(reachable.contains(name),qPrintable(name+" unreachable at "+QString::number(size.width())));
  // The band's controls neither overlap nor leave it; the status band shows the whole version line.
  const auto problem=[&]()->QString{
   QWidgetList items;for(auto* child:band->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(child->isVisible()&&!child->isWindow())items<<child;
   for(int i=0;i<items.size();++i){if(!band->rect().contains(items[i]->geometry()))return items[i]->accessibleName()+" leaves the band";for(int j=i+1;j<items.size();++j)if(items[i]->geometry().intersects(items[j]->geometry()))return items[i]->accessibleName()+" overlaps "+items[j]->accessibleName();}
   return {};
  };
  QString found;QVERIFY2(putgd::test::waitFor([&]{found=problem();return found.isEmpty();}),qPrintable(found));
  QVERIFY2(version->height()>=version->fontMetrics().lineSpacing(),qPrintable(QString("version link %1 px for a %2 px line").arg(version->height()).arg(version->fontMetrics().lineSpacing())));
  QVERIFY(window.rect().contains(QRect(version->mapTo(&window,QPoint()),version->size())));
 }
 // The status band at the minimum width and 150% text, with an update offered and
 // unsaved settings: one bounded "Changes not saved", no "save automatically",
 // and the summary, Change output…, update link and version neither overlap
 // nor leave the band. The split button keeps its 32 px arrow zone in the band.
 void statusBandFitsAtLargeTextWhileChangesWait(){
  auto font=QApplication::font();const auto original=font;auto restore=qScopeGuard([&]{QApplication::setFont(original);});font.setPixelSize(21);QApplication::setFont(font);
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(settings.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=QJsonObject{{"supported",true},{"result",QJsonObject{{"updateAvailable",true},{"latest","26.1231.99999-nightly"}}}};return data;});
  MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));fixture.session.refreshSnapshot();
  auto* update=window.findChild<QPushButton*>("updateAvailable");PUTGD_TRY_VERIFY(update->isVisible());
  auto* explanation=window.findChild<QLabel*>("autosaveExplanation");auto* status=window.findChild<QLabel*>("autosaveStatus");
  // "Settings save automatically" gives way before the summary has to elide.
  auto* elided=window.findChild<Graphite::ElidedLabel*>("captureOutputSummary");PUTGD_TRY_VERIFY(explanation->isHidden()||!elided->elided());
  fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost reply",{}};window.findChild<QLineEdit*>("maxFiles")->setText("251");
  PUTGD_TRY_COMPARE(status->text(),QString("Changes not saved"));QVERIFY(status->isVisible());QVERIFY(explanation->isHidden());QVERIFY(!status->text().contains('\n'));
  auto* band=window.statusBar();auto* summary=window.findChild<QLabel*>("captureOutputSummary");auto* change=window.findChild<QPushButton*>("changeCaptureOutput");
  const auto rectIn=[&](QWidget* w){return QRect(w->mapTo(band,QPoint()),w->size());};
  QString found;QVERIFY2(putgd::test::waitFor([&]{
   found.clear();QWidgetList shown{summary,change,status,update};for(auto* w:band->findChildren<QWidget*>()){if((w->objectName()=="appVersion"||w->objectName()=="appVersionLink")&&w->isVisible())shown<<w;}
   for(int i=0;i<shown.size()&&found.isEmpty();++i){if(!band->rect().contains(rectIn(shown[i])))found=shown[i]->objectName()+" leaves the band";for(int j=i+1;j<shown.size()&&found.isEmpty();++j)if(rectIn(shown[i]).intersects(rectIn(shown[j])))found=shown[i]->objectName()+" overlaps "+shown[j]->objectName();}
   if(found.isEmpty()&&change->width()<change->sizeHint().width())found="Change output… is cut";
   if(found.isEmpty()&&summary->width()<summary->minimumSizeHint().width())found="the summary is under its minimum";
   return found.isEmpty();}),qPrintable(found));
  // The summary is a tab stop whatever its width.
  QCOMPARE(summary->focusPolicy(),Qt::StrongFocus);
  for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->popupMode()==QToolButton::MenuButtonPopup){
   QStyleOptionToolButton option;option.initFrom(button);option.rect=button->rect();option.features=QStyleOptionToolButton::MenuButtonPopup|QStyleOptionToolButton::HasMenu;
   QVERIFY(button->style()->subControlRect(QStyle::CC_ToolButton,&option,QStyle::SC_ToolButtonMenu,button).width()>=32);}
 }
};
QTEST_MAIN(UiApplicationTest)
#include "test_ui_application.moc"
