// The tray icon follows each stage of the work, and a failure leaves it without staying on it.
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
 // An upload that finishes inside the capture stage's dwell still walks every stage it
 // reached and ends idle: nothing else happens after it, so only the dwell timer moves it.
 void aFastUploadStepsThroughEveryStageAndEndsIdle() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});
  PUTGD_TRY_COMPARE(provider->requests.size(),1);provider->completions.takeFirst()();
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
