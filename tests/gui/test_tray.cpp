// The tray icon follows each stage of the work, and a failure leaves it without staying on it.
#include "fixtures/U2Fixtures.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include "ui/TrayController.h"
#include "ui/TrayMenuSettle.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "ui/Ui.h"
#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTimer>
#include "fixtures/Waits.h"

using namespace putgd;
using namespace putgd::test;

class ResidentTray final : public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){
  // This suite posts synthetic critical failures. On a desktop's real bus they land in its notifications (#1307),
  // so it runs only where nothing can show them: no bus, or a private one with no notification server to reach.
  const auto bus=QDBusConnection::sessionBus();
  if(bus.isConnected()&&bus.interface()){
   const QString server("org.freedesktop.Notifications");
   if(bus.interface()->isServiceRegistered(server).value()||bus.interface()->activatableServiceNames().value().contains(server))
    QFAIL("Refusing a session bus with a notification server: run through ctest (gui_tray), which uses dbus-run-session with tests/fixtures/dbus-session-isolated.conf");
  }
  // Outside any seasonal artwork, so the icons compare with the plain tray glyph.
  qputenv("PUTGD_TRAY_CALENDAR_NOW","2026-06-15T12:00:00");
 }
 // A failure badges the icon, then lets it go back to normal; the menu row stays until it is reviewed.
 void failuresClearFromTheIconButStayInTheMenu() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  auto* failureTimer=controller.findChild<QTimer*>("trayFailureTimer");QVERIFY(failureTimer);failureTimer->setInterval(600);
  const auto idle=tray->icon().pixmap(24,24).toImage();const auto failed=Ui::icon("tray",Theme::trayTints(false).error,-2).pixmap(24,24).toImage();
  controller.notifyFailure({"fixture_failed","Synthetic error",{}});QCOMPARE(tray->icon().pixmap(24,24).toImage(),failed);
  QCOMPARE(controller.trayIconState()["activity"].toString(),QString("failed"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  auto* entry=tray->contextMenu()->findChild<QAction*>("trayFailedUploads");QVERIFY(entry->isVisible());entry->trigger();QCOMPARE(views,QStringList{"history"});
  QStringList seen;for(const auto& value:controller.trayIconState()["history"].toArray())seen<<value.toObject()["activity"].toString();
  QCOMPARE(seen,(QStringList{"failed","idle"}));
 }
 // An upload the provider finishes at once ends before the batched observations show its
 // processing or upload, and inside the capture stage's dwell: the icon still walks every
 // stage the outcome proves ran and ends idle, moved only by its own timers.
 void aFastUploadStepsThroughEveryStageAndEndsIdle() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});
  PUTGD_TRY_COMPARE(controller.trayIconState()["activity"].toString(),QString("complete"));
  PUTGD_TRY_COMPARE(controller.trayIconState()["activity"].toString(),QString("idle"));
  QStringList seen;for(const auto& value:controller.trayIconState()["history"].toArray())seen<<value.toObject()["activity"].toString();
  QCOMPARE(seen,(QStringList{"capturing","processing","uploading","complete","idle"}));
 }
 // New work always shows; a failure or completion shows only once the work has ended.
 // A wrong row here shows an old error over a new upload, or a finished glyph for work still running.
 void activityFollowsTheInputs_data() {
  using A=TrayController::Activity;
  QTest::addColumn<int>("working");QTest::addColumn<bool>("failure");QTest::addColumn<bool>("complete");QTest::addColumn<int>("expected");
  const auto row=[](const char* name,A working,bool failure,bool complete,A expected){QTest::newRow(name)<<int(working)<<failure<<complete<<int(expected);};
  row("idle",A::Idle,false,false,A::Idle);
  row("capturing",A::Capturing,false,false,A::Capturing);
  row("processing",A::Processing,false,false,A::Processing);
  row("uploading",A::Uploading,false,false,A::Uploading);
  row("complete",A::Idle,false,true,A::Complete);
  row("failed",A::Idle,true,false,A::Failed);
  row("failure beats an older completion",A::Idle,true,true,A::Failed);
  row("new work hides a failure",A::Capturing,true,false,A::Capturing);
  row("new work hides a completion",A::Processing,false,true,A::Processing);
 }
 void activityFollowsTheInputs() {
  QFETCH(int,working);QFETCH(bool,failure);QFETCH(bool,complete);QFETCH(int,expected);
  QCOMPARE(int(TrayController::activityFor({TrayController::Activity(working),failure,complete})),expected);
 }
 // Which stage an operation's own state shows.
 void stageFollowsTheOperation_data() {
  using A=TrayController::Activity;
  QTest::addColumn<QJsonObject>("operation");QTest::addColumn<int>("expected");
  const QJsonObject upload{{"providerId","s3"}};
  const auto op=[](const QString& action,const QString& state,const QString& phase,const QJsonObject& destination={},const QJsonArray& items={}){
   return QJsonObject{{"action",action},{"state",state},{"phase",phase},{"destination",destination},{"items",items}};};
  QTest::newRow("accepted")<<op("region","accepted","accepted")<<int(A::Capturing);
  QTest::newRow("selecting a region")<<op("region","acquiring","acquiring")<<int(A::Capturing);
  QTest::newRow("editing")<<op("edit","editing","editing")<<int(A::Processing);
  QTest::newRow("reading text")<<op("text","recognizing","recognizing")<<int(A::Processing);
  QTest::newRow("saving a local copy only")<<op("region","running","saving_local")<<int(A::Processing);
  QTest::newRow("captured, its artifact being prepared")<<op("region","acquiring","preparing")<<int(A::Processing);
  QTest::newRow("captured, queued for import")<<op("region","acquiring","queued")<<int(A::Processing);
  QTest::newRow("stored, still preparing its artifact")<<op("region","stored","preparing",upload)<<int(A::Processing);
  QTest::newRow("checking the destination")<<op("region","running","preflight",upload)<<int(A::Processing);
  QTest::newRow("upload phase")<<op("region","running","uploading",upload)<<int(A::Uploading);
  QTest::newRow("an item uploading")<<op("region","running","saving_local",upload,QJsonArray{QJsonObject{{"uploadStatus","uploading"}}})<<int(A::Uploading);
  QTest::newRow("recording keeps its badge")<<op("record","recording","recording",upload)<<int(A::Idle);
  QTest::newRow("recording review keeps its badge")<<op("record","recording","review",upload)<<int(A::Idle);
  QTest::newRow("recording encoding after Upload")<<op("record","recording","encoding",upload)<<int(A::Processing);
 }
 void stageFollowsTheOperation() {
  QFETCH(QJsonObject,operation);QFETCH(int,expected);
  QCOMPARE(int(TrayController::stageOf(operation)),expected);
 }
 // A file that is on the storage and only lacks its link is never called a failed upload: a user told
 // so uploads it again or gives up on a file that is already stored (#1399). Every other case keeps its text.
 void uploadedFilesWithoutTheirLinkAreNotFailedUploads_data() {
  QTest::addColumn<QJsonArray>("items");QTest::addColumn<QString>("expected");
  const auto item=[](const QString& upload,const QString& share,const QString& errorCode={},const QString& intent={}){
   return QJsonObject{{"uploadStatus",upload},{"shareStatus",share},{"shareIntent",intent},{"error",errorCode.isEmpty()?QJsonObject{}:QJsonObject{{"code",errorCode},{"message","Synthetic"}}}};};
  // A link still wanted names History's one-click Finish share link, or Retry upload while a file still needs its upload (#1496).
  QTest::newRow("wanted link unconfirmed")<<QJsonArray{item("uploaded","unknown",{},"create")}<<"Uploaded, but the share link couldn't be confirmed. Select Finish share link in History.";
  QTest::newRow("wanted link not made after inspection")<<QJsonArray{item("uploaded","private",{},"create")}<<"Uploaded, but the share link couldn't be created. Select Finish share link in History.";
  {auto deleted=item("uploaded","unknown",{},"create");deleted["remoteStatus"]="removed";QTest::newRow("wanted link of a deleted file")<<QJsonArray{deleted}<<"Uploaded, but the share link couldn't be confirmed. Select Inspect link in History.";}
  QTest::newRow("wanted link with a failed upload")<<QJsonArray{item("failed","not_requested","permission_denied"),item("uploaded","unknown",{},"create")}<<"1 of 2 files uploaded. 1 failed: access denied by the destination. 1 share link couldn't be confirmed. Select Retry upload in History.";
  QTest::newRow("link unknown")<<QJsonArray{item("uploaded","unknown")}<<"Uploaded, but the share link couldn't be confirmed. Select Inspect link in History.";
  QTest::newRow("link changed")<<QJsonArray{item("uploaded","changed")}<<"Uploaded, but the share link couldn't be confirmed. Select Inspect link in History.";
  QTest::newRow("link failed")<<QJsonArray{item("uploaded","failed","permission_denied")}<<"Uploaded, but the share link couldn't be created: access denied by the destination. Select Create share link in History.";
  QTest::newRow("batch, one link")<<QJsonArray{item("uploaded","shared"),item("uploaded","unknown")}<<"2 files uploaded, but 1 share link couldn't be confirmed. Select Inspect link in History.";
  QTest::newRow("batch, upload and link")<<QJsonArray{item("failed","not_requested","permission_denied"),item("uploaded","failed")}<<"1 of 2 files uploaded. 1 failed: access denied by the destination. 1 share link couldn't be created. Select Create share link in History.";
  QTest::newRow("failed upload, unchanged")<<QJsonArray{item("failed","not_requested","permission_denied")}<<"Upload failed: access denied by the destination.";
 }
 void uploadedFilesWithoutTheirLinkAreNotFailedUploads() {
  QFETCH(QJsonArray,items);QFETCH(QString,expected);
  const QJsonObject operation{{"state","partial"},{"copyStatus","no_result"},{"items",items}};
  QCOMPARE(TrayController::completionMessage(operation,false),expected);
 }
 // #1820: an upload the sensitive text check covered says what it covered, never after a check that couldn't finish.
 void aCoveredUploadSaysWhatWasCovered() {
  QJsonObject item;item["uploadStatus"]="uploaded";item["shareStatus"]="shared";
  QJsonObject covered;covered["key"]=2;covered["card"]=1;
  QJsonObject sensitive;sensitive["found"]=QJsonObject();sensitive["complete"]=true;sensitive["covered"]=covered;
  QJsonObject operation;operation["state"]="completed";operation["copyStatus"]="copied";operation["items"]=QJsonArray{item};operation["sensitive"]=sensitive;
  QCOMPARE(TrayController::completionMessage(operation,false),QString("Uploaded. Covered 1 card number and 2 keys or tokens. Link copied to clipboard."));
  sensitive["complete"]=false;sensitive["reason"]="truncated";operation["sensitive"]=sensitive;
  QCOMPARE(TrayController::completionMessage(operation,false),QString("Uploaded. Link copied to clipboard."));
  // A failed upload still says the original wasn't kept.
  QJsonObject original;original["status"]="failed";sensitive["original"]=original;operation["sensitive"]=sensitive;operation["state"]="failed";
  item["uploadStatus"]="failed";operation["items"]=QJsonArray{item};
  QVERIFY(TrayController::completionMessage(operation,false).contains("Couldn't save the unredacted original locally."));
 }
 // #2081: how long each desktop keeps a closed menu on the screen, from its own settings.
 void aClosedMenuFadesForTheDesktopsTime_data() {
  QTest::addColumn<QString>("desktop");QTest::addColumn<QString>("fadingPopups");QTest::addColumn<QString>("factor");
  QTest::addColumn<int>("animations");QTest::addColumn<int>("ms");QTest::addColumn<QString>("source");
  // KWin: 600 ms OutQuart, invisible after 70 % of it, plus a frame.
  QTest::newRow("plasma")<<"KDE"<<""<<""<<-1<<456<<"kwin-fadingpopups";
  QTest::newRow("plasma half speed")<<"KDE"<<"true"<<"2"<<-1<<877<<"kwin-fadingpopups";
  QTest::newRow("plasma instant")<<"KDE"<<""<<"0"<<-1<<34<<"kwin-animations-instant";
  QTest::newRow("plasma no popup fade")<<"KDE"<<"false"<<"1"<<-1<<34<<"kwin-fadingpopups-off";
  QTest::newRow("plasma bad factor")<<"KDE"<<""<<"fast"<<-1<<456<<"kwin-fadingpopups";
  QTest::newRow("gnome")<<"GNOME"<<""<<""<<-1<<184<<"gnome-popup-animation";
  QTest::newRow("ubuntu gnome off")<<"ubuntu:GNOME"<<""<<""<<0<<34<<"gnome-animations-off";
  QTest::newRow("cinnamon")<<"X-Cinnamon"<<""<<""<<1<<184<<"cinnamon-menu-effects";
  QTest::newRow("cinnamon off")<<"X-Cinnamon"<<""<<""<<0<<34<<"cinnamon-menu-effects-off";
  QTest::newRow("unmeasured")<<"Hyprland"<<""<<""<<-1<<traysettle::unknownFadeMs<<"unmeasured-desktop";
 }
 void aClosedMenuFadesForTheDesktopsTime() {
  QFETCH(QString,desktop);QFETCH(QString,fadingPopups);QFETCH(QString,factor);QFETCH(int,animations);QFETCH(int,ms);QFETCH(QString,source);
  traysettle::FadeInputs in;in.desktop=desktop;in.kwinFadingPopups=fadingPopups;in.kdeAnimationFactor=factor;
  if(animations>=0){in.gnomeAnimations=animations==1;in.cinnamonMenuEffects=animations==1;}
  const auto fade=traysettle::fade(in);
  QCOMPARE(fade.ms,ms);QCOMPARE(fade.source,source);
  QCOMPARE(traysettle::remainingMs(fade,ms+500),0);QCOMPARE(traysettle::remainingMs(fade,-5),ms);QCOMPARE(traysettle::remainingMs(fade,10),qMax(0,ms-10));
 }
 // Through the controller: a capture chosen in the tray menu waits for the fade, a hotkey starts at once.
 void aMenuCaptureWaitsAndAHotkeyDoesNot() {
  QTemporaryDir directory;
  // KDE at a quarter of the animation speed: a 1.7 s fade, far from a hotkey's start.
  QDir(directory.path()).mkpath("xdg");
  {QFile file(directory.filePath("xdg/kdeglobals"));QVERIFY(file.open(QIODevice::WriteOnly));file.write("[KDE]\nAnimationDurationFactor=4\n");}
  const auto oldConfig=qgetenv("XDG_CONFIG_HOME"),oldDesktop=qgetenv("XDG_CURRENT_DESKTOP");
  const auto restore=qScopeGuard([&]{qputenv("XDG_CONFIG_HOME",oldConfig);qputenv("XDG_CURRENT_DESKTOP",oldDesktop);});
  qputenv("XDG_CONFIG_HOME",directory.filePath("xdg").toUtf8());qputenv("XDG_CURRENT_DESKTOP","KDE");
  ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();QVERIFY(tray);
  QElapsedTimer clock;qint64 started=-1;
  connect(&operations,&OperationService::changed,&controller,[&]{if(started<0)started=clock.elapsed();});
  auto* row=tray->contextMenu()->findChild<QAction*>("region");QVERIFY(row);
  clock.start();row->trigger();
  PUTGD_COMPARE_STEADY(started,qint64(-1),1000);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(started>=0,5000);QVERIFY2(started>=1600,qPrintable(QString::number(started)));
  QCOMPARE(controller.trayMenuState()["settle"].toObject()["command"].toString(),QString("region"));
  QCOMPARE(controller.trayMenuState()["settle"].toObject()["fadeSource"].toString(),QString("kwin-fadingpopups"));
  PUTGD_TRY_VERIFY(operations.activeObservations().isEmpty());
  started=-1;clock.start();controller.invokeShortcut("region");
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(started>=0,1500);QVERIFY2(started<800,qPrintable(QString::number(started)));
 }
 // Only rows that read the screen wait; the others answer the click at once.
 void onlyScreenReadingRowsWait() {
  for(const auto* row:{"region","repeat","window","monitor","desktop","scroll","edit","record","text"})QVERIFY2(traysettle::readsScreen(row),row);
  for(const auto* row:{"clipboard","edit-clipboard","edit-file","settings","history","delay-3","show-recording","quit"})QVERIFY2(!traysettle::readsScreen(row),row);
 }
 // The capture starts only after the host's close and the fade after it, in each order hosts send them.
 void aCaptureWaitsUntilTheMenuIsGone() {
  constexpr int fadeMs=120;
  const auto started=[](TrayMenuSettle& settle,const QString& command,QElapsedTimer& clock,qint64& at){clock.start();at=-1;settle.afterMenu(command,[&clock,&at]{at=clock.elapsed();});};
  QElapsedTimer clock;qint64 at=-1;
  {
   // Plasma: "closed" arrives just before "clicked"; the fade runs from the close.
   TrayMenuSettle settle;settle.setFade({fadeMs,"test"});
   settle.menuShown();settle.menuClosed();started(settle,"repeat",clock,at);
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(at>=0,3000);QVERIFY2(at>=fadeMs-5,qPrintable(QString::number(at)));
   QCOMPARE(settle.state()["awaitedClose"].toBool(),false);QCOMPARE(settle.state()["fadeMs"].toInt(),fadeMs);QCOMPARE(settle.state()["command"].toString(),QString("repeat"));
  }
  {
   // GNOME: "clicked" first, "closed" after it. A host that has reported a close before is waited for,
   // even past the fade, and the fade counts from its close.
   TrayMenuSettle settle;settle.setFade({fadeMs,"test"});
   settle.menuShown();settle.menuClosed();settle.menuShown();started(settle,"desktop",clock,at);
   PUTGD_COMPARE_STEADY(at,qint64(-1),fadeMs+60);
   const auto closedAt=clock.elapsed();settle.menuClosed();
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(at>=0,3000);QVERIFY2(at>=closedAt+fadeMs-5,qPrintable(QString("%1 %2").arg(at).arg(closedAt)));
   QCOMPARE(settle.state()["awaitedClose"].toBool(),true);
  }
  {
   // GNOME's first menu: no close reported before, so the fade starts at the click; the
   // close that follows late starts it again, so the capture still waits a whole fade after it.
   TrayMenuSettle settle;settle.setFade({fadeMs,"test"});
   settle.menuShown();started(settle,"repeat",clock,at);
   PUTGD_COMPARE_STEADY(at,qint64(-1),60);
   const auto closedAt=clock.elapsed();settle.menuClosed();
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(at>=0,3000);QVERIFY2(at>=closedAt+fadeMs-5,qPrintable(QString("%1 %2").arg(at).arg(closedAt)));
   QCOMPARE(settle.state()["rebasedOnClose"].toBool(),true);
  }
  {
   // Cinnamon and Budgie report the menu showing but never its close: the fade runs from the click.
   TrayMenuSettle settle;settle.setFade({fadeMs,"test"});
   settle.menuShown();started(settle,"monitor",clock,at);
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(at>=0,3000);QVERIFY2(at>=fadeMs-5&&at<traysettle::closeWaitMs+fadeMs,qPrintable(QString::number(at)));
   QCOMPARE(settle.state()["awaitedClose"].toBool(),false);
  }
  {
   // A close that never comes holds the capture for closeWaitMs at most; the fade then counts from the click.
   for(const int fade:{fadeMs,traysettle::closeWaitMs+150}){
    TrayMenuSettle settle;settle.setFade({fade,"test"});
    settle.menuShown();settle.menuClosed();settle.menuShown();started(settle,"region",clock,at);
    PUTGD_TRY_VERIFY_WITH_TIMEOUT(at>=0,3000);
    QVERIFY2(at>=qMax(fade,traysettle::closeWaitMs)-5,qPrintable(QString("%1 %2").arg(at).arg(fade)));
   }
  }
 }
};

QTEST_MAIN(ResidentTray)
#include "test_tray.moc"
