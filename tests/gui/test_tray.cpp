// A failed upload stays marked on the tray icon until the user opens it.
#include "fixtures/U2Fixtures.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include "ui/TrayController.h"
#include "ui/Ui.h"
#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
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
 void failuresStayBadgedUntilAcknowledged() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  const auto idle=tray->icon().pixmap(24,24).toImage();const auto failed=Ui::icon("tray",Theme::trayTints(false).error,-2).pixmap(24,24).toImage();
  controller.notifyFailure({"fixture_failed","Synthetic error",{}});QCOMPARE(tray->icon().pixmap(24,24).toImage(),failed);
  PUTGD_COMPARE_STEADY(tray->icon().pixmap(24,24).toImage(),failed,3100);
  auto* entry=tray->contextMenu()->findChild<QAction*>("trayFailedUploads");QVERIFY(entry->isVisible());entry->trigger();QCOMPARE(views,QStringList{"history"});QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
 }
 // The tooltip title and text, icon colour and ring each switch on this one value, so a wrong
 // row here shows a wrong tray (green for work still running, a ring over an error) with nothing failing.
 void activityFollowsTheFlags_data() {
  using A=TrayController::Activity;
  QTest::addColumn<bool>("error");QTest::addColumn<bool>("completed");QTest::addColumn<bool>("visible");
  QTest::addColumn<bool>("storage");QTest::addColumn<bool>("tracked");QTest::addColumn<int>("expected");
  const auto row=[](const char* name,bool error,bool completed,bool visible,bool storage,bool tracked,A expected){
   QTest::newRow(name)<<error<<completed<<visible<<storage<<tracked<<int(expected);};
  row("idle",false,false,false,false,false,A::Idle);
  row("preparing keeps the idle value",false,false,false,false,true,A::Idle);
  row("storage not yet shown",false,false,false,true,true,A::Starting);
  row("uploading",false,false,true,true,true,A::Uploading);
  row("uploading between storage requests",false,false,true,false,true,A::Uploading);
  row("finishing",false,false,true,false,false,A::Finishing);
  row("complete",false,true,true,false,false,A::Complete);
  row("error alone",true,false,false,false,false,A::NeedsAttention);
  row("error beats everything",true,true,true,true,true,A::NeedsAttention);
 }
 void activityFollowsTheFlags() {
  QFETCH(bool,error);QFETCH(bool,completed);QFETCH(bool,visible);QFETCH(bool,storage);QFETCH(bool,tracked);QFETCH(int,expected);
  QCOMPARE(int(TrayController::activityFor({error,completed,visible,storage,tracked})),expected);
 }
 // A file that is on the storage and only lacks its link is never called a failed upload: a user told
 // so uploads it again or gives up on a file that is already stored (#1399). Every other case keeps its text.
 void uploadedFilesWithoutTheirLinkAreNotFailedUploads_data() {
  QTest::addColumn<QJsonArray>("items");QTest::addColumn<QString>("expected");
  const auto item=[](const QString& upload,const QString& share,const QString& errorCode={},const QString& intent={}){
   return QJsonObject{{"uploadStatus",upload},{"shareStatus",share},{"shareIntent",intent},{"error",errorCode.isEmpty()?QJsonObject{}:QJsonObject{{"code",errorCode},{"message","Synthetic"}}}};};
  // A link still wanted names History's one-click Finish share link, or Retry upload while a file still needs its upload (#1496).
  QTest::newRow("wanted link unconfirmed")<<QJsonArray{item("uploaded","unknown",{},"create")}<<"Uploaded, but the share link could not be confirmed. Use Finish share link in History.";
  QTest::newRow("wanted link not made after inspection")<<QJsonArray{item("uploaded","private",{},"create")}<<"Uploaded, but the share link was not made. Use Finish share link in History.";
  {auto deleted=item("uploaded","unknown",{},"create");deleted["remoteStatus"]="removed";QTest::newRow("wanted link of a deleted file")<<QJsonArray{deleted}<<"Uploaded, but the share link could not be confirmed. Use Inspect link in History.";}
  QTest::newRow("wanted link with a failed upload")<<QJsonArray{item("failed","not_requested","permission_denied"),item("uploaded","unknown",{},"create")}<<"1 of 2 files uploaded. 1 failed: access denied by the destination. 1 share link could not be confirmed. Use Retry upload in History.";
  QTest::newRow("link unknown")<<QJsonArray{item("uploaded","unknown")}<<"Uploaded, but the share link could not be confirmed. Use Inspect link in History.";
  QTest::newRow("link changed")<<QJsonArray{item("uploaded","changed")}<<"Uploaded, but the share link could not be confirmed. Use Inspect link in History.";
  QTest::newRow("link failed")<<QJsonArray{item("uploaded","failed","permission_denied")}<<"Uploaded, but the share link was not made: access denied by the destination. Use Create share link in History.";
  QTest::newRow("batch, one link")<<QJsonArray{item("uploaded","shared"),item("uploaded","unknown")}<<"2 files uploaded, but 1 share link could not be confirmed. Use Inspect link in History.";
  QTest::newRow("batch, upload and link")<<QJsonArray{item("failed","not_requested","permission_denied"),item("uploaded","failed")}<<"1 of 2 files uploaded. 1 failed: access denied by the destination. 1 share link was not made. Use Create share link in History.";
  QTest::newRow("failed upload, unchanged")<<QJsonArray{item("failed","not_requested","permission_denied")}<<"Upload failed: access denied by the destination.";
 }
 void uploadedFilesWithoutTheirLinkAreNotFailedUploads() {
  QFETCH(QJsonArray,items);QFETCH(QString,expected);
  const QJsonObject operation{{"state","partial"},{"copyStatus","no_result"},{"items",items}};
  QCOMPARE(TrayController::completionMessage(operation,false),expected);
 }
};

QTEST_MAIN(ResidentTray)
#include "test_tray.moc"
