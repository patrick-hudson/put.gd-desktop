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
  if (action->text().section('\t',0,0) == text) return action;
 return nullptr;
}

int amberPixels(const QImage& image) {
 int count=0;
 for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)
  if(image.pixelColor(x,y)==Theme::trayTints(false).encoding)++count;
 return count;
}

class CaptureSource final : public SourceAdapter {
public:
 QStringList actions;QJsonObject shortcutState;
 int copies = 0;
 quint64 clipboardRevision() const override { return 0; }
 QJsonObject capabilities() const override { return {{"shortcuts",shortcutState}}; }
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
 void menuShowsDisplayOnlyShortcutsAndCancelsUploads(){
  QTemporaryDir dir;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);SettingsService settings(dir.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});auto doc=configuration(dir.path());auto actions=doc["actions"].toObject();for(const auto& name:Settings::presentationOrder()){auto action=actions[name].toObject();action["shortcut"]="Ctrl+Shift+R";actions[name]=action;}doc["actions"]=actions;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService operations(dir.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();QVERIFY(tray);auto* menu=tray->contextMenu();QVERIFY(menu->findChild<QAction*>("trayOutputSummary"));QVERIFY(menu->findChild<QAction*>("trayChangeOutput"));
  for(auto* entry:menu->actions())if(entry->property("commandLabel").isValid()){QVERIFY(!entry->text().contains('\t'));QVERIFY(entry->shortcut().isEmpty());}
  auto* status=menu->findChild<QAction*>("trayUploadStatus");auto* cancel=menu->findChild<QAction*>("trayCancelUpload");QVERIFY(status&&cancel);QVERIFY(!status->isVisible());QString id;operations.invoke("clipboard",[&](Error error,QJsonObject value){QVERIFY(!error);id=value["operationId"].toString();});auto drain=qScopeGuard([provider]{provider->deferUploads=false;for(auto complete:std::exchange(provider->completions,{}))complete();});PUTGD_TRY_COMPARE(provider->requests.size(),1);PUTGD_TRY_VERIFY(cancel->isVisible());QCOMPARE(menu->actions()[0],status);QVERIFY(menu->actions()[1]->isSeparator());QCOMPARE(menu->actions()[2],cancel);QVERIFY(!status->isEnabled());QCOMPARE(status->text(),QString("Uploading 1 file…"));cancel->trigger();provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);PUTGD_TRY_VERIFY(!status->isVisible());bool canceled=false;operations.inspect(id,[&](Error error,QJsonObject op){QVERIFY(!error);canceled=op["state"]=="cancelled";});PUTGD_TRY_VERIFY(canceled);
 }
 void trayShortcutsRequireDesktopConfirmationAndClearStaleBindings(){
  QTemporaryDir dir;ProviderRegistry providers;providers.add("opaque",std::make_shared<OpaqueProvider>());SettingsService settings(dir.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});auto doc=configuration(dir.path());auto actions=doc["actions"].toObject();auto region=actions["region"].toObject();region["shortcut"]="Ctrl+Shift+R";actions["region"]=region;doc["actions"]=actions;QVERIFY(settings.save(doc,{}));CaptureSource source;OperationService operations(dir.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();auto* menu=tray->contextMenu();auto* entry=actionNamed(tray,"Capture region");QVERIFY(entry);QVERIFY(!entry->text().contains('\t'));source.shortcutState={{"available",true},{"bindings",QJsonObject{{"region",QJsonObject{{"bound",true},{"requested","Ctrl+Shift+R"},{"actual","Ctrl+Shift+R"}}}}}};QMetaObject::invokeMethod(menu,"aboutToShow");QVERIFY(entry->text().contains("\tCtrl+Shift+R"));source.shortcutState={};QMetaObject::invokeMethod(menu,"aboutToShow");QVERIFY(!entry->text().contains('\t'));menu->findChild<QAction*>("trayChangeOutput")->trigger();QCOMPARE(views.last(),QString("captureSettings"));
 }
 void multipleUploadsDelegateBoundedConfirmation(){
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->concurrency=8;provider->dispatcher=this;providers.add("opaque",provider);SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});auto doc=configuration(directory.path());auto naming=doc["naming"].toObject();naming["mode"]="generated";doc["naming"]=naming;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});
  // Invocation IDs arrive before the fake provider queues its upload callbacks.
  // Keep pumping and draining through completion before shutdown joins workers.
  auto drain=qScopeGuard([&]{QVERIFY(putgd::test::waitFor([&]{for(auto complete:std::exchange(provider->completions,{}))complete();return operations.diagnostics()["activeOperations"].toInt()==0;}));});QStringList ids;for(int i=0;i<2;++i){operations.invoke("clipboard",[&](Error error,QJsonObject value){QVERIFY(!error);ids<<value["operationId"].toString();});PUTGD_TRY_COMPARE(ids.size(),i+1);}auto* tray=controller.findChild<QSystemTrayIcon*>();auto* cancel=tray->contextMenu()->findChild<QAction*>("trayCancelUpload");PUTGD_TRY_COMPARE(cancel->text(),QString("Cancel all uploads…"));cancel->trigger();QCOMPARE(views.size(),1);QVERIFY(views[0].startsWith("historyCancelAll:"));QStringList bounded;for(const auto& value:QJsonDocument::fromJson(views[0].mid(17).toUtf8()).array())bounded<<value.toString();QCOMPARE(bounded.size(),2);for(const auto& id:ids)QVERIFY(bounded.contains(id));QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),2);
 }

 void localSaveRetryRetainsItsOperationAndRecovery(){
  QTemporaryDir directory;QFile blocked(directory.filePath("blocked"));QVERIFY(blocked.open(QIODevice::WriteOnly));blocked.write("synthetic blocker");blocked.close();ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));auto doc=configuration(directory.path(),true);auto archive=doc["archive"].toObject();archive["root"]=blocked.fileName();doc["archive"]=archive;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();auto* recovery=tray->contextMenu()->findChild<QAction*>("trayFailedUploads");QString id;operations.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty()&&recovery->isVisible());QCOMPARE(recovery->text(),QString("Local save needs attention"));QCOMPARE(recovery->property("operationId").toString(),id);recovery->trigger();QVERIFY(views.last().contains("archive_failed"));bool done=false;operations.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);done=true;});PUTGD_TRY_VERIFY(done&&recovery->isVisible());QCOMPARE(recovery->property("operationId").toString(),id);QCOMPARE(recovery->text(),QString("Local save needs attention"));
 }
 void outcomeTitlesDistinguishLocalAndUploadResults(){
  QJsonObject operation{{"state","completed"},{"items",QJsonArray{QJsonObject{{"uploadStatus","local_only"},{"local",QJsonObject{{"status","saved"}}}}}}};QCOMPARE(TrayController::completionTitle(operation),QString("Saved locally"));operation["items"]=QJsonArray{QJsonObject{{"uploadStatus","uploaded"}}};QCOMPARE(TrayController::completionTitle(operation),QString("Upload complete"));operation["state"]="partial";QCOMPARE(TrayController::completionTitle(operation),QString("Completed with a problem"));operation["items"]=QJsonArray{QJsonObject{{"uploadStatus","local_only"},{"local",QJsonObject{{"status","failed"}}}}};QCOMPARE(TrayController::completionTitle(operation),QString("Local save failed"));operation["items"]=QJsonArray{QJsonObject{{"uploadStatus","failed"},{"local",QJsonObject{{"status","failed"}}}}};QCOMPARE(TrayController::completionTitle(operation),QString("Completed with a problem"));
 }
 void errorRecoveryKeepsItsCategory(){
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));Source source;OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  controller.notifyFailure({"archive_failed","Synthetic local-save failure",{}});QVERIFY(tray->toolTip().contains("Local save needs attention"));QVERIFY(tray->toolTip().startsWith("Needs attention"));auto* recovery=tray->contextMenu()->findChild<QAction*>("trayFailedUploads");recovery->trigger();QVERIFY(views.last().startsWith("actionRecovery:"));QVERIFY(views.last().contains("archive_failed"));controller.notifyFailure({"output_required","Choose an output",{}});QVERIFY(tray->toolTip().contains("Destination needs attention"));recovery->trigger();QVERIFY(views.last().contains("output_required"));controller.notifyFailure({"unknown_failure","Synthetic error",{}});QVERIFY(tray->toolTip().contains("Action needs attention"));recovery->trigger();QCOMPARE(views.last(),QString("history"));controller.invokeShortcut("synthetic-unknown-action");PUTGD_TRY_VERIFY(tray->toolTip().contains("Action needs attention"));
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
  QCOMPARE(TrayController::describeActivity({batch}),QString("Uploading: 14 MB of 24 MB (58%) · 2 uploading, 3 waiting, 1 done"));
  // One file transferring still names it.
  const QJsonObject single{{"id","b"},{"action","clipboard"},{"state","uploading"},{"phase","uploading"},{"created","2026-10-20T06:00:00.000Z"},
   {"items",QJsonArray{item("uploading","uploading",3*mib,4*mib),item("pending","queued",-1,4*mib)}}};
  QCOMPARE(TrayController::describeActivity({single}),QString("Uploading file 1/2: 3 MB of 4 MB (75%)"));
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
  provider->requests.first().progress({"uploading",1,2});PUTGD_TRY_VERIFY(tray->icon().pixmap(24,24).toImage()!=idle);QElapsedTimer visible;visible.start();
  bool idleGap=false;QTimer sampler;sampler.setInterval(1);connect(&sampler,&QTimer::timeout,this,[&]{idleGap=idleGap||tray->icon().pixmap(24,24).toImage()==idle;});sampler.start();
  provider->completions.takeFirst()();PUTGD_TRY_VERIFY(tray->toolTip().startsWith("Upload complete"));
  putgd::test::settle(qMax(0,450-int(visible.elapsed())));PUTGD_TRY_VERIFY2(!idleGap,"Visible progress must keep its active frame through receipt lookup and completion");sampler.stop();
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
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).idle,10).pixmap(24,24).toImage());
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
  auto* information=tray->contextMenu()->findChild<QAction*>("trayUploadStatus");QVERIFY(information);QVERIFY(!information->isEnabled());QCOMPARE(information->property("visualRole").toString(),QString("information"));const auto entries=tray->contextMenu()->actions();QVERIFY(entries.value(entries.indexOf(information)+1)->isSeparator());
  const auto idleIcon=tray->icon().pixmap(24,24).toImage();const auto idleTooltip=tray->toolTip();
  QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);operationId=value["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Acquiring"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  source.acquisitions.takeFirst()({source.payload,{}});
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  PUTGD_TRY_COMPARE(provider->requests.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).idle,0).pixmap(24,24).toImage());
  provider->requests.first().progress({"uploading",2*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("2 MB of 4 MB"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).idle,50).pixmap(24,24).toImage());
  const auto half=tray->icon().pixmap(24,24).toImage();
  provider->requests.first().progress({"uploading",0,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("(0%)"));
  const auto empty=tray->icon().pixmap(24,24).toImage();
  QVERIFY2(amberPixels(empty)<amberPixels(half),"The amber fill must grow as transferred bytes increase");
  int previous=amberPixels(empty);
  for(int percent:{25,50,75,100}) {
   provider->requests.first().progress({"uploading",qint64(percent)*4*1024*1024/100,4*1024*1024});
   PUTGD_TRY_VERIFY(tray->toolTip().contains(QString("(%1%)").arg(percent)));
   const auto current=tray->icon().pixmap(24,24).toImage();
   QVERIFY(amberPixels(current)>previous);previous=amberPixels(current);
   if(percent==50) {
    QCOMPARE(current,half);
    QVERIFY(current.pixelColor(12,5)!=Theme::trayTints(false).encoding);
    QCOMPARE(current.pixelColor(12,20),Theme::trayTints(false).encoding);
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
  PUTGD_TRY_VERIFY(tray->toolTip().contains("8 MB transferred"));
  QVERIFY(tray->toolTip().contains("Source size: 4 MB"));QVERIFY(tray->toolTip().contains("Estimated 99%"));
  provider->requests.first().progress({"uploading",2*1024*1024,{}});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Estimated 50%"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),half);
  provider->requests.first().progress({"finalizing",4*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Finalizing"));
  const auto full=tray->icon().pixmap(24,24).toImage();
  QCOMPARE(full,VisualStyle::icon("tray",Theme::trayTints(false).idle,100).pixmap(24,24).toImage());
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
  PUTGD_TRY_VERIFY(tray->toolTip().startsWith("Upload complete"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).upload).pixmap(24,24).toImage());
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
  provider->requests.first().progress({"uploading",1,2});PUTGD_TRY_VERIFY(tray->toolTip().contains("Queued"));PUTGD_TRY_VERIFY(tray->icon().pixmap(24,24).toImage()!=idleIcon);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(provider->requests.size(),2);
  QVERIFY(tray->toolTip()!=idleTooltip);
  provider->failUpload=true;provider->completions.takeFirst()();
  const auto red=VisualStyle::icon("tray",Theme::trayTints(false).error,-2).pixmap(24,24).toImage();
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(400);QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QVERIFY(tray->contextMenu()->findChild<QAction*>("trayFailedUploads")->isVisible());
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
  QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  const auto blue=tray->icon().pixmap(24,24).toImage();const auto progressIcon=VisualStyle::icon("tray",Theme::trayTints(false).idle,0).pixmap(24,24).toImage();
  auto drain=qScopeGuard([provider]{provider->deferChecks=false;provider->deferUploads=false;for(auto complete:std::exchange(provider->checkCompletions,{}))complete();for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->checkCompletions.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),progressIcon);
  QList<bool> transitions;connect(&operations,&OperationService::storageActivityChanged,this,[&]{transitions.append(operations.storageActive());});
  provider->checkCompletions.takeFirst()();PUTGD_TRY_COMPARE(provider->requests.size(),1);
  QVERIFY2(!transitions.contains(true),"An upload still preparing must not produce even a transient green event");
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),progressIcon);
  provider->requests.first().progress({"uploading",0,provider->requests.first().artifact.bytes});PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),progressIcon);
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
  const auto success=VisualStyle::icon("tray",Theme::trayTints(false).upload).pixmap(24,24).toImage();
  QElapsedTimer watch;watch.start();while(watch.elapsed()<1000){const auto current=tray->icon().pixmap(24,24).toImage();QVERIFY(current==blue||(current==success&&tray->toolTip().startsWith("Upload complete")));putgd::test::settle(20);}
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
  QVERIFY(tooltip.contains("1 MB of 8 MB"));QVERIFY(tooltip.contains("2 MB of 8 MB"));QVERIFY(tooltip.contains("3 MB of 8 MB"));QVERIFY(!tooltip.contains("4 MB of 8 MB"));
  QVERIFY(tooltip.indexOf("1 MB")<tooltip.indexOf("2 MB"));QVERIFY(tooltip.indexOf("2 MB")<tooltip.indexOf("3 MB"));
  putgd::test::settle(300);PUTGD_TRY_COMPARE(batches.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).idle,31).pixmap(24,24).toImage());
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
  TrayController controller(operations,[](const QString&){},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idleTooltip=tray->toolTip();const auto idleIcon=tray->icon().pixmap(24,24).toImage();
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryStore store(operations.databasePath());const auto prior=store.operation(id);QCOMPARE(prior.items[0].local.status,QString("failed"));
  const auto red=VisualStyle::icon("tray",Theme::trayTints(false).error,-2).pixmap(24,24).toImage();
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3100);
  bool failedRetryDone=false;operations.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);failedRetryDone=true;});PUTGD_TRY_VERIFY(failedRetryDone);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QVERIFY(QFile::remove(blocker));QVERIFY(QDir().mkpath(blocker));
  tray->contextMenu()->findChild<QAction*>("trayFailedUploads")->trigger();
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
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);QVERIFY(tray->contextMenu()->findChild<QAction*>("trayFailedUploads")->isVisible());QVERIFY(operations.activeObservations().isEmpty());
 }

 void acquisitionAndPreparationErrorsPersistWithoutNotifications_data() {
  QTest::addColumn<bool>("preparation");
  QTest::newRow("capture-failure") << false;
  QTest::newRow("preparation-failure") << true;
 }
 void acquisitionAndPreparationErrorsPersistWithoutNotifications() {
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
  const auto red=VisualStyle::icon("tray",Theme::trayTints(false).error,-2).pixmap(24,24).toImage();
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3400);QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);QVERIFY(tray->contextMenu()->findChild<QAction*>("trayFailedUploads")->isVisible());
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
  const auto progressIcon=VisualStyle::icon("tray",Theme::trayTints(false).idle,0).pixmap(24,24).toImage();
  const auto red=VisualStyle::icon("tray",Theme::trayTints(false).error,-2).pixmap(24,24).toImage();
  auto drain=qScopeGuard([provider]{for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->requests.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),progressIcon);
  if(storageFailure) {
   controller.invokeShortcut("clipboard");PUTGD_TRY_COMPARE(provider->requests.size(),2);
   provider->failUpload=true;provider->completions.takeLast()();provider->failUpload=false;
  } else {
   source.defer=true;controller.invokeShortcut("region");PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
   source.acquisitions.takeFirst()({{}, {"fixture_capture_failed","Synthetic capture failure",{}}});
  }
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  auto* recovery=tray->contextMenu()->findChild<QAction*>("trayFailedUploads");QCOMPARE(recovery->text(),storageFailure?QString("Failed uploads (1)"):QString("Capture needs attention"));QVERIFY(!recovery->property("operationId").toString().isEmpty());
  // Progress from the successful request must not mask the error pulse.
  provider->requests.first().progress({"uploading",1,100});
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(400);QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
 }

 void failuresStayBadgedUntilAcknowledged() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views<<view;},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  const auto blue=tray->icon().pixmap(24,24).toImage();const auto red=VisualStyle::icon("tray",Theme::trayTints(false).error,-2).pixmap(24,24).toImage();
  controller.notifyFailure({"fixture_failed","Synthetic first error",{}});QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(2100);controller.notifyFailure({"fixture_failed","Synthetic second error",{}});
  putgd::test::settle(1100);PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  putgd::test::settle(3100);QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);auto* failed=tray->contextMenu()->findChild<QAction*>("trayFailedUploads");QVERIFY(failed->isVisible());failed->trigger();QCOMPARE(views,QStringList{"history"});QVERIFY(!failed->isVisible());QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
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
  for (const auto& name : QStringList{"History", "Destinations", "Settings", "About put.gd"}) {
   auto* action = actionNamed(tray, name);
   QVERIFY(action);
   action->trigger();
  }
  QCOMPARE(views, QStringList({"history", "destinations", "settings", "about"}));
  // "Recent activity" opened the same History page, so it is gone.
  QVERIFY(!actionNamed(tray, "Recent activity"));
  QStringList data; for (auto* action : menu->actions()) data.append(action->data().toString());
  QVERIFY(!data.contains("activity"));
  // A single & would be eaten as a mnemonic marker in the menu.
  QVERIFY(actionNamed(tray, "Capture region && edit"));
  // Capture entries come first in a fixed order, then the editor sources and the clipboard upload.
  QStringList texts; for (auto* action : menu->actions()) { if(!action->isVisible())continue;if (action->property("commandLabel").isValid()||action->data()=="edit-sources") texts.append(action->text().section('\t',0,0)); }
  QCOMPARE(texts, QStringList({"Capture region", "Capture window", "Capture desktop", "Capture region && edit", "Edit image", "Record screen", "Upload clipboard"}));
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

 // Recording and pause have independent overlays; transfer fills amber and only
 // a confirmed upload turns the glyph green.
 void recordingOverlayPersistsAlongsideFailuresUntilTransfer() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  auto fake=std::make_unique<FakeRecordingBackend>();auto* backend=fake.get();backend->deferExports=true;backend->exportBytes=QByteArray("\0\0\0\x18" "ftypisom",12)+QByteArray(4*1024*1024,'r');operations.setRecordingBackend(std::move(fake));
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();QVERIFY(tray);
  const auto idle=tray->icon().pixmap(24,24).toImage();
  const auto progressIcon=VisualStyle::icon("tray",Theme::trayTints(false).idle,0).pixmap(24,24).toImage();
  QCOMPARE(progressIcon,idle);
  auto command=[&](const QString& name,const QJsonObject& params){QJsonObject out;bool done=false;operations.recordingCommand(name,params,[&](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(name+": "+e.message));out=r;done=true;});(void)putgd::test::waitFor([&]{return done;});return out;};
  const auto id=command("prepare",{{"target",QJsonObject{{"kind","region"},{"rect",QJsonArray{100,100,1280,720}}}},{"format","mp4"}})["recordingId"].toString();QVERIFY(!id.isEmpty());
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Ready to record"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  command("start",{{"id",id}});PUTGD_TRY_VERIFY(tray->toolTip().contains("Recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).idle,-1,1).pixmap(24,24).toImage());QVERIFY(actionNamed(tray,"Record screen — Recording"));
  command("pause",{{"id",id}});PUTGD_TRY_VERIFY(tray->toolTip().contains("Recording paused"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).idle,-1,2).pixmap(24,24).toImage());controller.notifyFailure({"synthetic_failure","Synthetic failed upload",{}});QVERIFY(tray->toolTip().contains("Action needs attention"));QVERIFY(tray->toolTip().contains("Recording paused"));QVERIFY(actionNamed(tray,"Record screen — Recording paused"));QVERIFY(actionNamed(tray,"Action needs attention"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).error,-2,2).pixmap(24,24).toImage());actionNamed(tray,"Action needs attention")->trigger();
  command("resume",{{"id",id}});command("stop",{{"id",id}});PUTGD_TRY_VERIFY(tray->toolTip().contains("Reviewing the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  bool saved=false;operations.recordingCommand("saveCopy",{{"id",id},{"path",directory.filePath("copy.mp4")}},[&](Error e,QJsonObject){QVERIFY(!e);saved=true;});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Saving a copy of the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  PUTGD_TRY_COMPARE(backend->pendingExports.size(),1);backend->pendingExports.takeFirst()();PUTGD_TRY_VERIFY(saved);
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Reviewing the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  bool uploaded=false;operations.recordingCommand("upload",{{"id",id},{"revision",1}},[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));uploaded=true;});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("Encoding the recording"));QCOMPARE(tray->icon().pixmap(24,24).toImage(),idle);
  PUTGD_TRY_COMPARE(backend->pendingExports.size(),1);backend->pendingExports.takeFirst()();PUTGD_TRY_VERIFY(uploaded);
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  PUTGD_TRY_COMPARE(provider->requests.size(),1);
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),progressIcon);
  provider->requests.first().progress({"uploading",2*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(tray->toolTip().contains("(50%)"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("tray",Theme::trayTints(false).idle,50).pixmap(24,24).toImage());
  const auto half=tray->icon().pixmap(24,24).toImage();QVERIFY(half.pixelColor(12,5)!=Theme::trayTints(false).encoding);QCOMPARE(half.pixelColor(12,20),Theme::trayTints(false).encoding);
  provider->requests.first().progress({"finalizing",4*1024*1024,4*1024*1024});
  std::exchange(provider->completions,{}).first()();
  const auto success=VisualStyle::icon("tray",Theme::trayTints(false).upload).pixmap(24,24).toImage();
  PUTGD_TRY_VERIFY(tray->icon().pixmap(24,24).toImage()==success&&tray->toolTip().startsWith("Upload complete"));
  PUTGD_TRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idle);
 }
};

QTEST_MAIN(ResidentTray)
#include "test_tray.moc"
