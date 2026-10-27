#include "fixtures/U2Fixtures.h"
#include "fixtures/FakeRecordingBackend.h"
#include "ui/TrayController.h"
#include "ui/ResidentTheme.h"
#include "ui/VisualStyle.h"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSystemTrayIcon>
#include "fixtures/Waits.h"

using namespace putgd;
using namespace putgd::test;

namespace {
QAction* actionNamed(QSystemTrayIcon* tray, const QString& text) {
 if (!tray || !tray->contextMenu()) return nullptr;
 for (auto* action : tray->contextMenu()->actions())
  if (action->text() == text) return action;
 return nullptr;
}

int greenPixels(const QImage& image) {
 int count=0;
 for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)
  if(image.pixelColor(x,y)==QColor("#62d69b"))++count;
 return count;
}

class CaptureSource final : public SourceAdapter {
public:
 QStringList actions;
 int copies = 0;
 quint64 clipboardRevision() const override { return 0; }
 QJsonObject capabilities() const override { return {}; }
 void acquire(const QString& action, const ResourceLimits&, std::function<void(PayloadResult)> done) override {
  actions.append(action);
  done({{}, {"source_cancelled", "Synthetic selection cancelled", {}}});
 }
 void cancelAcquisition() override {}
 void copy(const CopyPayload&, std::function<void(Error)> done) override { ++copies; done({}); }
};
class SupersedingSource final : public SourceAdapter {
public:
 int copies=0;std::function<void(Error)> pendingCopy;
 quint64 clipboardRevision() const override {return 0;}
 QJsonObject capabilities() const override {return {};}
 void acquire(const QString& action,const ResourceLimits&,std::function<void(PayloadResult)> done) override {
  if(action=="clipboard")done({textPayload(),{}});
  else done({{}, {"source_cancelled","Synthetic capture cancelled",{}}});
 }
 void invalidateCopyEligibility() override {
  if(pendingCopy)std::exchange(pendingCopy,{})({"copy_suppressed","A newer action superseded clipboard ownership",{}});
 }
 void copy(const CopyPayload&,std::function<void(Error)> done) override {++copies;pendingCopy=std::move(done);}
 void cancelAcquisition() override {}
};

}

class ResidentTray final : public QObject {
 Q_OBJECT
private slots:
 void residentThemeScopeRestoresTheEnvironmentForChildren() {
  // Views and helpers inherit the environment; only the resident's own
  // QApplication construction may see the generic theme override.
  const bool aware=QGuiApplication::desktopSettingsAware();
  qputenv("QT_QPA_PLATFORMTHEME","qt6ct");
  {GenericResidentTheme theme;QCOMPARE(qgetenv("QT_QPA_PLATFORMTHEME"),QByteArray("generic"));QVERIFY(!QGuiApplication::desktopSettingsAware());}
  QCOMPARE(qgetenv("QT_QPA_PLATFORMTHEME"),QByteArray("qt6ct"));
  qunsetenv("QT_QPA_PLATFORMTHEME");
  {GenericResidentTheme theme;QCOMPARE(qgetenv("QT_QPA_PLATFORMTHEME"),QByteArray("generic"));}
  QVERIFY(!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME"));
  QGuiApplication::setDesktopSettingsAware(aware);
 }
 void failedOperationsWithoutItemsShowTheirError() {
  const QJsonObject tooLarge{{"state","failed"},{"copyStatus","no_result"},{"items",QJsonArray{}},{"error",QJsonObject{{"code","edit_input_too_large"},{"message","This image is too large to edit; use a smaller copy"}}}};
  QCOMPARE(TrayController::completionMessage(tooLarge,false),QString("This image is too large to edit; use a smaller copy"));
  const QJsonObject done{{"state","completed"},{"copyStatus","copied"},{"items",QJsonArray{QJsonObject{{"uploadStatus","uploaded"}}}},{"error",QJsonObject{}}};
  QCOMPARE(TrayController::completionMessage(done,true),QString("Uploaded. Link copied to the clipboard. Click to open in browser."));
  QCOMPARE(TrayController::completionMessage(QJsonObject{{"state","completed"},{"copyStatus","no_result"},{"items",QJsonArray{QJsonObject{{"uploadStatus","uploaded"}},QJsonObject{{"uploadStatus","uploaded"}}}}},false),QString("2 files uploaded."));
  // #473: a refused automatic copy is named, with the way back; the link still opens.
  const QJsonObject suppressed{{"state","completed"},{"copyStatus","suppressed"},{"items",QJsonArray{QJsonObject{{"uploadStatus","uploaded"}}}},{"error",QJsonObject{}}};
  QCOMPARE(TrayController::completionMessage(suppressed,true),QString("Uploaded. Link not copied: the clipboard changed during the upload. Copy it from History. Click to open in browser."));
  const QJsonObject suppressedLocal{{"state","completed"},{"copyStatus","suppressed"},{"items",QJsonArray{QJsonObject{{"uploadStatus","local_only"}}}},{"error",QJsonObject{}}};
  QCOMPARE(TrayController::completionMessage(suppressedLocal,false),QString("Saved locally; not copied because the clipboard changed meanwhile. Copy it from History."));
 }
 // #152: a failed or partial upload names the failure in plain words, never a raw copyStatus code.
 void failedUploadsNameTheFailure() {
  const QJsonObject denied{{"code","s3_permission_denied"},{"message","S3 denied the request (HTTP 403)."}};
  const QJsonObject saved{{"status","saved"}};
  // The m74 case: an editor upload to a destination that rejects it, local copy kept.
  const QJsonObject partial{{"state","partial"},{"copyStatus","no_result"},{"error",QJsonObject{}},
   {"items",QJsonArray{QJsonObject{{"uploadStatus","failed"},{"error",denied},{"local",saved}}}}};
  const auto text=TrayController::completionMessage(partial,false);
  QCOMPARE(text,QString("Upload failed: access denied by the destination. A local copy was saved."));
  QVERIFY(!text.contains("no_result"));QVERIFY(!text.contains("Clipboard:"));
  // Without a local copy, and with a failure the map doesn't know: the provider's own words.
  const QJsonObject failed{{"state","failed"},{"copyStatus","no_result"},{"error",QJsonObject{}},
   {"items",QJsonArray{QJsonObject{{"uploadStatus","failed"},{"error",QJsonObject{{"code","fixture_failed"},{"message","Synthetic upload failure."}}}}}}};
  QCOMPARE(TrayController::completionMessage(failed,false),QString("Upload failed: synthetic upload failure."));
  // A batch: how many made it, why the rest didn't, what was copied.
  const QJsonObject batch{{"state","partial"},{"copyStatus","copied"},{"error",QJsonObject{}},
   {"items",QJsonArray{QJsonObject{{"uploadStatus","uploaded"}},QJsonObject{{"uploadStatus","failed"},{"error",QJsonObject{{"code","s3_transport_unknown"},{"message","x"}}},{"local",saved}},QJsonObject{{"uploadStatus","uploaded"}}}}};
  QCOMPARE(TrayController::completionMessage(batch,false),QString("2 of 3 files uploaded. 1 failed: the destination could not be reached. A local copy was saved. Link copied to the clipboard."));
  // The operation's own error when no item carries one.
  const QJsonObject interrupted{{"state","interrupted"},{"copyStatus","pending"},{"error",QJsonObject{{"code","s3_authentication_failed"},{"message","x"}}},{"items",QJsonArray{QJsonObject{{"uploadStatus","pending"}}}}};
  QCOMPARE(TrayController::completionMessage(interrupted,false),QString("Upload interrupted: the destination did not accept the saved credentials."));
  for(const auto& [code,phrase]:std::initializer_list<std::pair<const char*,const char*>>{{"s3_bucket_not_found","the bucket does not exist"},{"credential_setup_required","the destination needs to be set up again in Settings"},
      {"s3_object_too_large","the file is larger than the destination accepts"},{"proton_request_timeout","the destination could not be reached"},{"s3_region_mismatch","the bucket is in a different region than the destination's settings"}})
   QCOMPARE(TrayController::failureReason(QJsonObject{{"code",code},{"message","Raw"}}),QString(phrase));
  QCOMPARE(TrayController::failureReason({}),QString());
 }
 // #175: several files of one operation transferring at once read like History's active row.
 void queuedEditorUsesPhaseRatherThanEditingState() {
  for(const auto& state:QStringList{"editing","acquiring"})
   QCOMPARE(TrayController::describeActivity({QJsonObject{{"id","submitted-edit"},{"action","edit"},{"state",state},{"phase","queued"}}}),QString("Queued"));
 }
 void tooltipSummarizesConcurrentFiles() {
  auto item=[](const char* status,const char* phase,qint64 sent,qint64 total){
   QJsonObject value{{"uploadStatus",status},{"phase",phase},{"artifact",QJsonObject{{"bytes",double(total)}}}};
   if(sent>=0)value["uploadProgress"]=QJsonObject{{"phase","uploading"},{"bytesTransferred",double(sent)},{"bytesTotal",double(total)}};
   return value;
  };
  constexpr qint64 mib=1024*1024;
  const QJsonObject batch{{"id","a"},{"action","clipboard"},{"state","uploading"},{"phase","uploading"},{"created","2026-10-20T06:00:00.000Z"},
   {"items",QJsonArray{item("uploading","uploading",4*mib,8*mib),item("uploading","uploading",2*mib,8*mib),item("pending","queued",-1,8*mib),
                       item("pending","queued",-1,8*mib),item("pending","queued",-1,8*mib),item("uploaded","completed",8*mib,8*mib)}}};
  QCOMPARE(TrayController::describeActivity({batch}),QString("Uploading: 14 MiB of 24 MiB (58%) · 2 uploading, 3 waiting, 1 done"));
  // One file transferring still names it.
  const QJsonObject single{{"id","b"},{"action","clipboard"},{"state","uploading"},{"phase","uploading"},{"created","2026-10-20T06:00:00.000Z"},
   {"items",QJsonArray{item("uploading","uploading",3*mib,4*mib),item("pending","queued",-1,4*mib)}}};
  QCOMPARE(TrayController::describeActivity({single}),QString("Uploading file 1/2: 3 MiB of 4 MiB (75%)"));
 }

 void fastUploadsDoNotFlashTheIcon() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->prepareUploads=true;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.payload=textPayload();OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Completion must not open a view");},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idle=tray->icon().pixmap(24,24).toImage();
  bool changed=false;QTimer sampler;sampler.setInterval(1);connect(&sampler,&QTimer::timeout,this,[&]{changed=changed||tray->icon().pixmap(24,24).toImage()!=idle;});sampler.start();
  for(int i=0;i<3;++i){
   bool accepted=false;operations.invoke("clipboard",[&](Error error,QJsonObject){QVERIFY(!error);accepted=true;});
   PUTGD_TRY_VERIFY(accepted);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  }
  putgd::test::settle(350);PUTGD_TRY_VERIFY2(!changed,"Brief successful uploads must not flash green or a completion frame");
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idle);
 }

 void visibleUploadFinishesWithoutAnIdleGapAndHoldsItsFrame() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.payload=textPayload();OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idle=tray->icon().pixmap(24,24).toImage();
  auto drain=qScopeGuard([provider]{provider->deferUploads=false;for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error error,QJsonObject){QVERIFY(!error);});PUTGD_TRY_COMPARE(provider->requests.size(),1);
  PUTGD_TRY_VERIFY(tray->icon().pixmap(24,24).toImage()!=idle);QElapsedTimer visible;visible.start();
  bool idleGap=false;QTimer sampler;sampler.setInterval(1);connect(&sampler,&QTimer::timeout,this,[&]{idleGap=idleGap||tray->icon().pixmap(24,24).toImage()==idle;});sampler.start();
  provider->completions.takeFirst()();PUTGD_TRY_VERIFY(tray->toolTip().contains("Upload complete"));
  putgd::test::settle(qMax(0,450-int(visible.elapsed())));PUTGD_TRY_VERIFY2(!idleGap,"Visible progress must hold green through receipt lookup and completion");sampler.stop();
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idle);
 }

 void concurrentTransferFillUsesSourceByteWeights() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->concurrency=8;provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=2;document["limits"]=limits;
  auto naming=document["naming"].toObject();naming["mode"]="generated";document["naming"]=naming;QVERIFY(settings.save(document,{}));
  Source source;source.payload.kind="text";source.payload.bytes=QByteArray(1024*1024,'x');OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  auto drain=qScopeGuard([provider]{provider->deferUploads=false;for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error error,QJsonObject){QVERIFY(!error);});PUTGD_TRY_COMPARE(provider->requests.size(),1);
  source.payload.bytes=QByteArray(9*1024*1024,'x');operations.invoke("clipboard",[](Error error,QJsonObject){QVERIFY(!error);});PUTGD_TRY_COMPARE(provider->requests.size(),2);
  provider->requests[0].progress({"uploading",1024*1024,1024*1024});provider->requests[1].progress({"uploading",0,9*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("(100%)")&&tray->toolTip().contains("(0%)"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",QColor("#62d69b"),10).pixmap(24,24).toImage());
 }

 void uploadProgressAndActiveColorWithoutOpeningViews() {
  QTemporaryDir directory; ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;
  providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.defer=true;source.payload.kind="text";source.payload.bytes=QByteArray(4*1024*1024,'x');
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();QVERIFY(tray);
  const auto idleIcon=tray->icon().pixmap(24,24).toImage();const auto idleTooltip=tray->toolTip();
  QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);operationId=value["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Acquiring"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  source.acquisitions.takeFirst()({source.payload,{}});
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  PUTGD_TRY_COMPARE(provider->requests.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",QColor("#62d69b"),0).pixmap(24,24).toImage());
  provider->requests.first().progress({"uploading",2*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("2 MiB of 4 MiB"));
  const auto half=tray->icon().pixmap(24,24).toImage();
  provider->requests.first().progress({"uploading",0,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("(0%)"));
  const auto empty=tray->icon().pixmap(24,24).toImage();
  QVERIFY2(greenPixels(empty)<greenPixels(half),"The green glyph must fill as transferred bytes increase");
  int previous=greenPixels(empty);
  for(int percent:{25,50,75,100}) {
   provider->requests.first().progress({"uploading",qint64(percent)*4*1024*1024/100,4*1024*1024});
   PUTGD_TRY_VERIFY(tray->toolTip().contains(QString("(%1%)").arg(percent)));
   const auto current=tray->icon().pixmap(24,24).toImage();
   QVERIFY(greenPixels(current)>previous);previous=greenPixels(current);
   if(percent==50) {
    QCOMPARE(current,half);
    QVERIFY(current.pixelColor(12,5)!=QColor("#62d69b"));
    QCOMPARE(current.pixelColor(12,20),QColor("#62d69b"));
   }
  }
  // A provider retry restarts its reported transfer; never retain a stale fill.
  provider->requests.first().progress({"uploading",0,4*1024*1024});
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),empty);
  QVERIFY(views.isEmpty());
  provider->requests.first().progress({"preparing",0,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Preparing"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),empty);
  provider->requests.first().progress({"uploading",8*1024*1024,{}});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("8 MiB transferred"));
  QVERIFY(tray->toolTip().contains("Source size: 4 MiB"));QVERIFY(tray->toolTip().contains("Estimated 99%"));
  provider->requests.first().progress({"uploading",2*1024*1024,{}});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Estimated 50%"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),half);
  provider->requests.first().progress({"finalizing",4*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Finalizing"));
  const auto full=tray->icon().pixmap(24,24).toImage();
  QCOMPARE(full,VisualStyle::icon("tray",QColor("#62d69b")).pixmap(24,24).toImage());
  provider->completions.takeFirst()();
  PUTGD_TRY_COMPARE(tray->toolTip(),idleTooltip);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  // Late provider observations cannot reactivate a finished transfer.
  provider->requests.first().progress({"uploading",1,4*1024*1024});putgd::test::settle(300);
  PUTGD_TRY_COMPARE(tray->toolTip(),idleTooltip);PUTGD_TRY_VERIFY(views.isEmpty());
  // An idle tray does not mean the operation is released: copying it
  // earlier is refused with operation_busy (#346).
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  bool copied=false;operations.copyResult(operationId,[&](Error error,QJsonObject){QVERIFY(!error);copied=true;});
  PUTGD_TRY_VERIFY(copied);putgd::test::settle(50);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  // A transfer without a provider total still reaches full on its receipt.
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);source.acquisitions.takeFirst()({source.payload,{}});
  PUTGD_TRY_COMPARE(provider->requests.size(),2);
  provider->requests.last().progress({"uploading",2*1024*1024,{}});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Estimated 50%"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),half);
  provider->completions.takeFirst()();
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Upload complete"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),full);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
 }

 void queuedWorkAndTemporaryErrorFeedback() {
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=1;document["limits"]=limits;
  QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idleTooltip=tray->toolTip();const auto idleIcon=tray->icon().pixmap(24,24).toImage();
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->requests.size(),1);
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.reads,2);
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Queued"));PUTGD_TRY_VERIFY(tray->icon().pixmap(24,24).toImage()!=idleIcon);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(provider->requests.size(),2);
  QVERIFY(tray->toolTip()!=idleTooltip);
  provider->failUpload=true;provider->completions.takeFirst()();
  const auto red=VisualStyle::icon("tray",QColor("#ffb4ab")).pixmap(24,24).toImage();
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3100);
  PUTGD_TRY_COMPARE(tray->toolTip(),idleTooltip);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
 }
 void preparingAndQueuedOperationsStayBlueBeforeStorageDispatch() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  // One upload at a time: the second action queues behind the first.
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=1;document["limits"]=limits;
  QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Queue status must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  auto release=std::make_shared<std::promise<void>>();const auto gate=release->get_future().share();auto preparing=std::make_shared<std::atomic_bool>(false);
  provider->capabilitiesGate=[gate,preparing]{preparing->store(true);gate.wait();};
  // Release even when an assertion fails, before the service joins its workers.
  auto drain=qScopeGuard([release]{release->set_value();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_VERIFY(preparing->load());
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.reads,2);
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Queued"));QCOMPARE(provider->checks.load(),0);QCOMPARE(provider->uploads.load(),0);
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void uploadPreparationDoesNotEmitAnIntermediateGreenTransition() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  provider->dispatcher=this;provider->deferChecks=true;provider->deferUploads=true;provider->prepareUploads=true;
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  const auto blue=tray->icon().pixmap(24,24).toImage();const auto green=VisualStyle::icon("tray",QColor("#62d69b"),0).pixmap(24,24).toImage();
  auto drain=qScopeGuard([provider]{provider->deferChecks=false;provider->deferUploads=false;for(auto complete:std::exchange(provider->checkCompletions,{}))complete();for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->checkCompletions.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),green);
  QList<bool> transitions;connect(&operations,&OperationService::storageActivityChanged,this,[&]{transitions.append(operations.storageActive());});
  provider->checkCompletions.takeFirst()();PUTGD_TRY_COMPARE(provider->requests.size(),1);
  QVERIFY2(!transitions.contains(true),"An upload still preparing must not produce even a transient green event");
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),green);
  provider->requests.first().progress({"uploading",0,provider->requests.first().artifact.bytes});PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),green);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void deliberateClipboardSupersessionDoesNotFlashAnError() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));SupersedingSource source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);
  operations.invalidateCopyEligibility();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryStore store(operations.databasePath());QCOMPARE(store.operation(id).copyStatus,QString("suppressed"));QCOMPARE(store.operation(id).state,QString("completed"));
  const auto success=VisualStyle::icon("tray",QColor("#62d69b")).pixmap(24,24).toImage();
  QElapsedTimer watch;watch.start();while(watch.elapsed()<1000){const auto current=tray->icon().pixmap(24,24).toImage();QVERIFY(current==blue||(current==success&&tray->toolTip().contains("Upload complete")));putgd::test::settle(20);}
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void cancelledAcquisitionReturnsIdle() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idleTooltip=tray->toolTip();const auto blue=tray->icon().pixmap(24,24).toImage();QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);PUTGD_TRY_VERIFY(tray->toolTip().contains("Acquiring"));
  operations.cancel(id,[](Error e,QJsonObject){QVERIFY(!e);});
  // Clipboard acquisition owns its asynchronous drain after cancellation.
  source.acquisitions.takeFirst()({{}, {"cancelled","Synthetic acquisition drained",{}}});
  PUTGD_TRY_COMPARE(tray->toolTip(),idleTooltip);
  putgd::test::settle(350);PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void multipleProgressUpdatesUseOneBatchAndBoundedRows() {
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->concurrency=8;provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=4;document["limits"]=limits;
  auto naming=document["naming"].toObject();naming["mode"]="generated";document["naming"]=naming;
  QVERIFY(settings.save(document,{}));Source source;source.payload.kind="text";source.payload.bytes=QByteArray(8*1024*1024,'x');
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  for(int index=1;index<=4;++index){operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->requests.size(),index);}
  putgd::test::settle(300);QSignalSpy batches(&operations,&OperationService::observationsChanged);
  for(int index=0;index<4;++index)provider->requests[index].progress({"uploading",(index+1)*1024*1024,8*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("1 more active operations"));PUTGD_TRY_COMPARE(batches.size(),1);
  const auto tooltip=tray->toolTip();
  QVERIFY(tooltip.contains("1 MiB of 8 MiB"));QVERIFY(tooltip.contains("2 MiB of 8 MiB"));QVERIFY(tooltip.contains("3 MiB of 8 MiB"));QVERIFY(!tooltip.contains("4 MiB of 8 MiB"));
  QVERIFY(tooltip.indexOf("1 MiB")<tooltip.indexOf("2 MiB"));QVERIFY(tooltip.indexOf("2 MiB")<tooltip.indexOf("3 MiB"));
  putgd::test::settle(300);PUTGD_TRY_COMPARE(batches.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",QColor("#62d69b"),31).pixmap(24,24).toImage());
  for(auto complete:std::exchange(provider->completions,{}))complete();
  PUTGD_TRY_VERIFY(!tray->toolTip().contains("Working"));
 }

 void localSaveRetryShowsWorkAndClearsOnSuccessOrError() {
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto archive=document["archive"].toObject();archive["keepUploads"]=true;
  const auto blocker=fixture(directory.path(),"blocked","synthetic");archive["root"]=blocker;document["archive"]=archive;
  QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Retry progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idleTooltip=tray->toolTip();const auto idleIcon=tray->icon().pixmap(24,24).toImage();
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryStore store(operations.databasePath());const auto prior=store.operation(id);QCOMPARE(prior.items[0].local.status,QString("failed"));
  const auto red=VisualStyle::icon("tray",QColor("#ffb4ab")).pixmap(24,24).toImage();
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3100);
  bool failedRetryDone=false;operations.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);failedRetryDone=true;});PUTGD_TRY_VERIFY(failedRetryDone);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QVERIFY(QFile::remove(blocker));QVERIFY(QDir().mkpath(blocker));
  putgd::test::settle(3100);
  bool done=false;operations.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);done=true;});
  QVERIFY(tray->toolTip().contains("Saving local copy"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  QCOMPARE(store.operation(id).state,prior.state);
  PUTGD_TRY_VERIFY(done);QCOMPARE(tray->toolTip(),idleTooltip);QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  const auto retried=store.operation(id);QCOMPARE(retried.items[0].local.status,QString("saved"));QCOMPARE(provider->uploads.load(),1);
  QCOMPARE(retried.state,prior.state);QCOMPARE(retried.copyStatus,prior.copyStatus);
  auto before=prior.items[0].toJson(),after=retried.items[0].toJson();before.remove("local");after.remove("local");QCOMPARE(after,before);
  QString code;operations.retryLocalSave("missing-operation",[&](Error e,QJsonObject){code=e.code;});
  QVERIFY(tray->toolTip().contains("Saving local copy"));PUTGD_TRY_VERIFY(!code.isEmpty());
  QCOMPARE(tray->toolTip(),idleTooltip);QVERIFY(operations.activeObservations().isEmpty());
 }

 void acquisitionAndPreparationErrorsFlashWithoutNotifications_data() {
  QTest::addColumn<bool>("preparation");
  QTest::newRow("capture-failure") << false;
  QTest::newRow("preparation-failure") << true;
 }
 void acquisitionAndPreparationErrorsFlashWithoutNotifications() {
  QFETCH(bool,preparation);
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));
  auto doc=configuration(directory.path(),true);auto preferences=doc["preferences"].toObject();preferences["notifications"]=false;doc["preferences"]=preferences;
  QVERIFY(settings.save(doc,{}));Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Feedback must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  controller.invokeShortcut("region");PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
  // An unsupported payload exercises ArtifactStore's preparation error.
  ClipboardPayload payload;payload.kind="unsupported";
  source.acquisitions.takeFirst()({payload,preparation?Error{}:Error{"fixture_capture_failed","Synthetic capture failure",{}}});
  const auto red=VisualStyle::icon("tray",QColor("#ffb4ab")).pixmap(24,24).toImage();
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3100);PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void errorRecoveryResumesAnotherActiveStorageRequest_data() {
  QTest::addColumn<bool>("storageFailure");
  QTest::newRow("capture-failure") << false;
  QTest::newRow("upload-failure") << true;
 }
 void errorRecoveryResumesAnotherActiveStorageRequest() {
  QFETCH(bool,storageFailure);
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->concurrency=8;provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=2;document["limits"]=limits;
  auto naming=document["naming"].toObject();naming["mode"]="generated";document["naming"]=naming;
  QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Feedback must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  const auto green=VisualStyle::icon("tray",QColor("#62d69b"),0).pixmap(24,24).toImage();
  const auto red=VisualStyle::icon("tray",QColor("#ffb4ab")).pixmap(24,24).toImage();
  auto drain=qScopeGuard([provider]{for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->requests.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),green);
  if(storageFailure) {
   controller.invokeShortcut("clipboard");PUTGD_TRY_COMPARE(provider->requests.size(),2);
   provider->failUpload=true;provider->completions.takeLast()();provider->failUpload=false;
  } else {
   source.defer=true;controller.invokeShortcut("region");PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
   source.acquisitions.takeFirst()({{}, {"fixture_capture_failed","Synthetic capture failure",{}}});
  }
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  // Progress from the successful request must not mask the error pulse.
  provider->requests.first().progress({"uploading",1,100});
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3100);PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",QColor("#62d69b"),1).pixmap(24,24).toImage());
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void aNewErrorRestartsTheFeedbackInterval() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  const auto blue=tray->icon().pixmap(24,24).toImage();const auto red=VisualStyle::icon("tray",QColor("#ffb4ab")).pixmap(24,24).toImage();
  controller.notifyFailure({"fixture_failed","Synthetic first error",{}});QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(2100);controller.notifyFailure({"fixture_failed","Synthetic second error",{}});
  putgd::test::settle(1100);PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3100);PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void delegatesEveryViewAndExplicitQuitWithoutConstructingViews() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QStringList views;
  int quits = 0;
  auto controller = std::make_unique<TrayController>(operations,
    [&](const QString& view) { views.append(view); }, [&] { ++quits; });
  QPointer<QSystemTrayIcon> tray = controller->findChild<QSystemTrayIcon*>();
  QVERIFY(tray);
  QVERIFY(!tray->icon().isNull());
  // Visibility intent must survive a desktop tray that appears after startup.
  QVERIFY(tray->isVisible());
  QPointer<QMenu> menu = tray->contextMenu();
  QVERIFY(menu);
  const auto windows = QApplication::topLevelWidgets();
  for (const auto& name : QStringList{"Settings", "History", "About put.gd"}) {
   auto* action = actionNamed(tray, name);
   QVERIFY(action);
   action->trigger();
  }
  QCOMPARE(views, QStringList({"settings", "history", "about"}));
  // "Recent activity" opened the same History page, so it is gone.
  QVERIFY(!actionNamed(tray, "Recent activity"));
  QStringList data; for (auto* action : menu->actions()) data.append(action->data().toString());
  QVERIFY(!data.contains("activity"));
  // A single & would be eaten as a mnemonic marker in the menu.
  QVERIFY(actionNamed(tray, "Capture region && edit"));
  // Capture entries come first in a fixed order, then the editor sources and the clipboard upload.
  QStringList texts; for (auto* action : menu->actions()) { if (action->isSeparator()) break; texts.append(action->text()); }
  QCOMPARE(texts, QStringList({"Capture region", "Capture region && edit", "Capture window", "Capture desktop", "Record screen", "Edit image", "Upload clipboard"}));
  // Other editor sources sit in one submenu; a file opens the main window's picker.
  QMenu* editImage = nullptr; for (auto* action : menu->actions()) if (action->menu() && action->text() == "Edit image") editImage = action->menu();
  QVERIFY(editImage); QCOMPARE(editImage->actions().size(), 2);
  QCOMPARE(editImage->actions().at(1)->text(), QString("Edit an image file…")); editImage->actions().at(1)->trigger();
  QCOMPARE(views.last(), QString("editFile"));
  QCOMPARE(QApplication::topLevelWidgets(), windows);
  QCOMPARE(quits, 0);
  controller->activate();
  QCOMPARE(views.last(), QString("settings"));
  QVERIFY(QMetaObject::invokeMethod(tray, "activated", Qt::DirectConnection,
                                   Q_ARG(QSystemTrayIcon::ActivationReason, QSystemTrayIcon::Trigger)));
  QCOMPARE(views.last(), QString("history"));
  auto* quit = actionNamed(tray, "Quit put.gd");
  QVERIFY(quit);
  quit->trigger();
  QCOMPARE(quits, 1);
  QVERIFY(tray);
  QCOMPARE(source.actions.size(), 0);
  QCOMPARE(source.copies, 0);
  controller.reset();
  QVERIFY(tray.isNull());
  QVERIFY(menu.isNull());
 }

 void updateEntryOpensUpdatesWithTheDownloadGlyph() {
  // The tray's Update available lands on Settings → Updates (not the first
  // settings page), and its icon is the download glyph, not the generic file.
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QStringList views;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  auto* tray = controller.findChild<QSystemTrayIcon*>();
  QVERIFY(tray);
  controller.setUpdateStatus({{"channel", "stable"}, {"result", QJsonObject{{"updateAvailable", true}, {"latest", "0.1.2"}}}});
  auto* update = actionNamed(tray, "Update available: 0.1.2");
  QVERIFY(update);
  QVERIFY(update->isVisible());
  const auto image = [](const QIcon& icon) { return icon.pixmap(QSize(18, 18), 1.0).toImage().convertToFormat(QImage::Format_ARGB32); };
  const auto tint = controller.trayTint()["color"].toString();
  QCOMPARE(image(update->icon()), image(VisualStyle::icon("download", QColor(tint))));
  QVERIFY(image(update->icon()) != image(VisualStyle::icon("update", QColor(tint))));
  update->trigger();
  QCOMPARE(views, QStringList({"updates"}));
 }

 void everyCaptureActionWorksWithNoViewsOpen() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(), true), {}));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  auto* tray = controller.findChild<QSystemTrayIcon*>();
  QVERIFY(tray);
  QSet<QString> terminalActions;
  connect(&operations, &OperationService::changed, this, [&](const QString& id) {
   operations.inspect(id, [&](Error error, QJsonObject operation) {
    QVERIFY(!error);
    if (operation["state"] == "cancelled") terminalActions.insert(operation["action"].toString());
   });
  });
  for (const auto& name : QStringList{"region", "window", "desktop", "clipboard"}) {
   auto* action = actionNamed(tray, name == "clipboard" ? QString("Upload clipboard") : "Capture " + name);
   QVERIFY(action);
   action->trigger();
   PUTGD_TRY_VERIFY(source.actions.contains(name));
   // The acquisition callback is processed asynchronously by the resident.
   // Wait for its terminal event before beginning another screen selection.
   PUTGD_TRY_VERIFY(terminalActions.contains(name));
  }
  QCOMPARE(source.actions, QStringList({"region", "window", "desktop", "clipboard"}));
  QVERIFY(views.isEmpty());
  QCOMPARE(source.copies, 0);
 }

 void nativeShortcutDuringRecoveryDoesNotOpenViews() {
  QTemporaryDir directory;ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));CaptureSource source;
  OperationService operations(directory.filePath("data"),settings,providers,source);
  QVERIFY(!operations.ready());QStringList views;
  TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  controller.invokeShortcut("region");
  QVERIFY(views.isEmpty());QVERIFY(source.actions.isEmpty());
  PUTGD_TRY_VERIFY(operations.ready());
 }
 void repeatedNativeCaptureDoesNotOpenViewsOverSelection() {
  QTemporaryDir directory;ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  CaptureSource source;OperationService operations(directory.filePath("data"),settings,providers,source);
  PUTGD_TRY_VERIFY(operations.ready());QStringList views;
  TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  controller.invokeShortcut("region");
  controller.invokeShortcut("region");
  QVERIFY2(views.isEmpty(),"A repeated native shortcut must not open History over an accepted selection");
  PUTGD_TRY_COMPARE(source.actions.size(),1);
 }
 void nativeFullQueueDoesNotOpenViewsOverSelection() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));
  auto document=configuration(directory.path(),true);auto limits=document["limits"].toObject();
  limits["pendingOperations"]=1;limits["concurrentUploads"]=1;document["limits"]=limits;
  QVERIFY(settings.save(document,{}));CaptureSource source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  controller.invokeShortcut("clipboard");controller.invokeShortcut("region");
  QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),2);
  controller.invokeShortcut("region");
  QVERIFY2(views.isEmpty(),"queue_full precedes busy_selecting and must not map History over selection");
  PUTGD_TRY_COMPARE(source.actions.size(),2);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
 }
 void nativeClipboardRejectionDoesNotOpenViewsOverSelection() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));
  auto document=configuration(directory.path(),true);auto actions=document["actions"].toObject();
  auto clipboard=actions["clipboard"].toObject();clipboard["destination"]="upload";actions["clipboard"]=clipboard;document["actions"]=actions;
  QVERIFY(settings.save(document,{}));CaptureSource source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  controller.invokeShortcut("region");
  auto archive=document["archive"].toObject();archive["keepUploads"]=false;document["archive"]=archive;QVERIFY(settings.save(document,settings.revision()));
  controller.invokeShortcut("clipboard");
  QVERIFY2(views.isEmpty(),"Clipboard bypasses the selection guard but admission errors must not map a view");
  PUTGD_TRY_COMPARE(source.actions,QStringList{"region"});QCOMPARE(source.copies,0);
 }
 void rejectedCaptureCallbackCanDestroyItsTray() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  PUTGD_TRY_VERIFY(operations.ready());
  std::unique_ptr<TrayController> controller;
  QString opened;
  controller = std::make_unique<TrayController>(operations, [&](const QString& view) {
   opened = view;
   controller.reset();
  }, [] {});
  auto* tray = controller->findChild<QSystemTrayIcon*>();
  QPointer<QAction> action = actionNamed(tray, "Capture region");
  QVERIFY(action);
  action->trigger();
  QVERIFY(opened.startsWith("actionRecovery:"));
  QVERIFY(!controller);
  QVERIFY(action.isNull());
 }

 void missingOutputOpensCaptureRecoveryWithoutOwningAWindow() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  auto* tray = controller.findChild<QSystemTrayIcon*>();
  auto* action = actionNamed(tray, "Capture region");
  QVERIFY(action);
  action->trigger();
  PUTGD_TRY_COMPARE(views.size(),1);QVERIFY(views.first().startsWith("actionRecovery:"));const auto recovery=QJsonDocument::fromJson(views.first().mid(15).toUtf8()).object();QCOMPARE(recovery["code"].toString(),QString("output_required"));QCOMPARE(recovery["action"].toString(),QString("region"));
  QVERIFY(source.actions.isEmpty());
 }
 // The tray entry opens a recording (a dragged region) and raises it while one is
 // active; the global shortcut toggles it: prepare, start, stop.
 void recordingTrayEntryOpensAndRaisesWhileTheShortcutToggles() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  auto fake=std::make_unique<FakeRecordingBackend>();auto* backend=fake.get();operations.setRecordingBackend(std::move(fake));
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();QVERIFY(tray);
  auto state=[&]{QString s;bool done=false;operations.recordingCommand("inspect",{{"id",operations.recordingActiveId()}},[&](Error,QJsonObject r){s=r["state"].toString();done=true;});(void)putgd::test::waitFor([&]{return done;});return s;};
  auto* entry=actionNamed(tray,"Record screen");QVERIFY(entry);
  entry->trigger();PUTGD_TRY_VERIFY(backend->log.contains("prepare"));PUTGD_TRY_COMPARE(state(),QString("ready"));
  QCOMPARE(backend->lastConfig.format,QString("mp4"));
  entry->trigger();PUTGD_TRY_VERIFY(backend->log.contains("raise"));QCOMPARE(state(),QString("ready"));
  controller.invokeShortcut("record");PUTGD_TRY_COMPARE(state(),QString("recording"));
  controller.invokeShortcut("record");PUTGD_TRY_COMPARE(state(),QString("reviewing"));
  const int raises=backend->log.count("raise");controller.invokeShortcut("record");PUTGD_TRY_COMPARE(backend->log.count("raise"),raises+1);QCOMPARE(state(),QString("reviewing"));
 }

 // A recording keeps the idle icon through capture, review and Save copy; Upload
 // turns it amber while the clip is encoded and saved, then the ordinary upload
 // feedback takes over: dark green filling up, bright green on completion.
 void recordingKeepsIdleColorUntilUploadThenAmberThenGreen() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  auto fake=std::make_unique<FakeRecordingBackend>();auto* backend=fake.get();backend->deferExports=true;backend->exportBytes=QByteArray("\0\0\0\x18" "ftypisom",12)+QByteArray(4*1024*1024,'r');operations.setRecordingBackend(std::move(fake));
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();QVERIFY(tray);
  const auto idle=tray->icon().pixmap(24,24).toImage();
  const auto amber=VisualStyle::icon("tray",QColor("#f3c66f")).pixmap(24,24).toImage();
  const auto green=VisualStyle::icon("tray",QColor("#62d69b"),0).pixmap(24,24).toImage();
  QVERIFY(amber!=idle&&green!=idle);
  auto command=[&](const QString& name,const QJsonObject& params){QJsonObject out;bool done=false;operations.recordingCommand(name,params,[&](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(name+": "+e.message));out=r;done=true;});(void)putgd::test::waitFor([&]{return done;});return out;};
  const auto id=command("prepare",{{"target",QJsonObject{{"kind","region"},{"rect",QJsonArray{100,100,1280,720}}}},{"format","mp4"}})["recordingId"].toString();QVERIFY(!id.isEmpty());
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Ready to record"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  command("start",{{"id",id}});PUTGD_TRY_VERIFY(tray->toolTip().contains("Recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  command("pause",{{"id",id}});PUTGD_TRY_VERIFY(tray->toolTip().contains("Recording paused"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  command("resume",{{"id",id}});command("stop",{{"id",id}});PUTGD_TRY_VERIFY(tray->toolTip().contains("Reviewing the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  bool saved=false;operations.recordingCommand("saveCopy",{{"id",id},{"path",directory.filePath("copy.mp4")}},[&](Error e,QJsonObject){QVERIFY(!e);saved=true;});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Saving a copy of the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  PUTGD_TRY_COMPARE(backend->pendingExports.size(),1);backend->pendingExports.takeFirst()();PUTGD_TRY_VERIFY(saved);
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Reviewing the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  bool uploaded=false;operations.recordingCommand("upload",{{"id",id},{"revision",1}},[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));uploaded=true;});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Encoding the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),amber);
  PUTGD_TRY_COMPARE(backend->pendingExports.size(),1);backend->pendingExports.takeFirst()();PUTGD_TRY_VERIFY(uploaded);
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  PUTGD_TRY_COMPARE(provider->requests.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),green);
  provider->requests.first().progress({"uploading",2*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("(50%)"));
  const auto half=tray->icon().pixmap(24,24).toImage();QVERIFY(half.pixelColor(12,5)!=QColor("#62d69b"));QCOMPARE(half.pixelColor(12,20),QColor("#62d69b"));
  provider->requests.first().progress({"finalizing",4*1024*1024,4*1024*1024});
  std::exchange(provider->completions,{}).first()();
  const auto success=VisualStyle::icon("tray",QColor("#62d69b")).pixmap(24,24).toImage();
  PUTGD_TRY_VERIFY(tray->icon().pixmap(24,24).toImage()==success&&tray->toolTip().contains("Upload complete"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idle);
 }
};

QTEST_MAIN(ResidentTray)
#include "test_tray.moc"
