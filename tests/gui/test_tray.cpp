#include "fixtures/U2Fixtures.h"
#include "ui/TrayController.h"
#include "ui/VisualStyle.h"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSystemTrayIcon>

using namespace putgd;
using namespace putgd::test;

namespace {
QAction* actionNamed(QSystemTrayIcon* tray, const QString& text) {
 if (!tray || !tray->contextMenu()) return nullptr;
 for (auto* action : tray->contextMenu()->actions())
  if (action->text() == text) return action;
 return nullptr;
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
 void uploadProgressAndActiveColorWithoutOpeningViews() {
  QTemporaryDir directory; ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;
  providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.defer=true;source.payload.kind="text";source.payload.bytes=QByteArray(4*1024*1024,'x');
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();QVERIFY(tray);
  const auto idleIcon=tray->icon().pixmap(24,24).toImage();const auto idleTooltip=tray->toolTip();
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});
  QTRY_COMPARE(source.acquisitions.size(),1);
  QTRY_VERIFY(tray->toolTip().contains("Acquiring"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  source.acquisitions.takeFirst()({source.payload,{}});
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  QTRY_COMPARE(provider->requests.size(),1);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),VisualStyle::icon("brand",QColor("#62d69b")).pixmap(24,24).toImage());
  provider->requests.first().progress({"uploading",2*1024*1024,4*1024*1024});
  QTRY_VERIFY(tray->toolTip().contains("2 MiB of 4 MiB"));
  QVERIFY(views.isEmpty());
  provider->requests.first().progress({"preparing",0,4*1024*1024});
  QTRY_VERIFY(tray->toolTip().contains("Preparing"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  provider->requests.first().progress({"uploading",8*1024*1024,{}});
  QTRY_VERIFY(tray->toolTip().contains("8 MiB transferred"));
  QVERIFY(tray->toolTip().contains("Source size: 4 MiB"));QVERIFY(!tray->toolTip().contains('%'));
  provider->requests.first().progress({"finalizing",4*1024*1024,4*1024*1024});
  QTRY_VERIFY(tray->toolTip().contains("Finalizing"));
  provider->completions.takeFirst()();
  QTRY_COMPARE(tray->toolTip(),idleTooltip);
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  // Late provider observations cannot reactivate a finished transfer.
  provider->requests.first().progress({"uploading",1,4*1024*1024});QTest::qWait(300);
  QCOMPARE(tray->toolTip(),idleTooltip);QVERIFY(views.isEmpty());
 }

 void queuedWorkAndTemporaryErrorFeedback() {
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=1;document["limits"]=limits;
  QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idleTooltip=tray->toolTip();const auto idleIcon=tray->icon().pixmap(24,24).toImage();
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->requests.size(),1);
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.reads,2);
  QTRY_VERIFY(tray->toolTip().contains("Queued"));QVERIFY(tray->icon().pixmap(24,24).toImage()!=idleIcon);
  provider->completions.takeFirst()();QTRY_COMPARE(provider->requests.size(),2);
  QVERIFY(tray->toolTip()!=idleTooltip);
  provider->failUpload=true;provider->completions.takeFirst()();
  const auto red=VisualStyle::icon("brand",QColor("#ffb4ab")).pixmap(24,24).toImage();
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTest::qWait(3100);
  QCOMPARE(tray->toolTip(),idleTooltip);
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
 }
 void preparingAndQueuedOperationsStayBlueBeforeStorageDispatch() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Queue status must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  auto release=std::make_shared<std::promise<void>>();const auto gate=release->get_future().share();auto preparing=std::make_shared<std::atomic_bool>(false);
  provider->capabilitiesGate=[gate,preparing]{preparing->store(true);gate.wait();};
  // Release even when an assertion fails, before the service joins its workers.
  auto drain=qScopeGuard([release]{release->set_value();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_VERIFY(preparing->load());
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.reads,2);
  QTRY_VERIFY(tray->toolTip().contains("Queued"));QCOMPARE(provider->checks.load(),0);QCOMPARE(provider->uploads.load(),0);
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void uploadPreparationDoesNotEmitAnIntermediateGreenTransition() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  provider->dispatcher=this;provider->deferChecks=true;provider->deferUploads=true;provider->prepareUploads=true;
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  const auto blue=tray->icon().pixmap(24,24).toImage();const auto green=VisualStyle::icon("brand",QColor("#62d69b")).pixmap(24,24).toImage();
  auto drain=qScopeGuard([provider]{provider->deferChecks=false;provider->deferUploads=false;for(auto complete:std::exchange(provider->checkCompletions,{}))complete();for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->checkCompletions.size(),1);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),green);
  QList<bool> transitions;connect(&operations,&OperationService::storageActivityChanged,this,[&]{transitions.append(operations.storageActive());});
  provider->checkCompletions.takeFirst()();QTRY_COMPARE(provider->requests.size(),1);
  QVERIFY2(!transitions.contains(true),"An upload still preparing must not produce even a transient green event");
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
  provider->requests.first().progress({"uploading",0,provider->requests.first().artifact.bytes});QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),green);
  provider->completions.takeFirst()();QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void deliberateClipboardSupersessionDoesNotFlashAnError() {
  QTemporaryDir directory;ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));SupersedingSource source;
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);
  operations.invalidateCopyEligibility();QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryStore store(operations.databasePath());QCOMPARE(store.operation(id).copyStatus,QString("suppressed"));QCOMPARE(store.operation(id).state,QString("completed"));
  QElapsedTimer watch;watch.start();while(watch.elapsed()<1000){QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);QTest::qWait(20);}
 }

 void cancelledAcquisitionReturnsIdle() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idleTooltip=tray->toolTip();const auto blue=tray->icon().pixmap(24,24).toImage();QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});
  QTRY_COMPARE(source.acquisitions.size(),1);QTRY_VERIFY(tray->toolTip().contains("Acquiring"));
  operations.cancel(id,[](Error e,QJsonObject){QVERIFY(!e);});
  // Clipboard acquisition owns its asynchronous drain after cancellation.
  source.acquisitions.takeFirst()({{}, {"cancelled","Synthetic acquisition drained",{}}});
  QTRY_COMPARE(tray->toolTip(),idleTooltip);
  QTest::qWait(350);QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void multipleProgressUpdatesUseOneBatchAndBoundedRows() {
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=4;document["limits"]=limits;
  auto naming=document["naming"].toObject();naming["mode"]="generated";document["naming"]=naming;
  QVERIFY(settings.save(document,{}));Source source;source.payload.kind="text";source.payload.bytes=QByteArray(8*1024*1024,'x');
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  for(int index=1;index<=4;++index){operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->requests.size(),index);}
  QTest::qWait(300);QSignalSpy batches(&operations,&OperationService::observationsChanged);
  for(int index=0;index<4;++index)provider->requests[index].progress({"uploading",(index+1)*1024*1024,8*1024*1024});
  QTRY_VERIFY(tray->toolTip().contains("1 more active operations"));QTRY_COMPARE(batches.size(),1);
  const auto tooltip=tray->toolTip();
  QVERIFY(tooltip.contains("1 MiB of 8 MiB"));QVERIFY(tooltip.contains("2 MiB of 8 MiB"));QVERIFY(tooltip.contains("3 MiB of 8 MiB"));QVERIFY(!tooltip.contains("4 MiB of 8 MiB"));
  QVERIFY(tooltip.indexOf("1 MiB")<tooltip.indexOf("2 MiB"));QVERIFY(tooltip.indexOf("2 MiB")<tooltip.indexOf("3 MiB"));
  QTest::qWait(300);QCOMPARE(batches.size(),1);
  for(auto complete:std::exchange(provider->completions,{}))complete();
  QTRY_VERIFY(!tray->toolTip().contains("Working"));
 }

 void localSaveRetryShowsWorkAndClearsOnSuccessOrError() {
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto archive=document["archive"].toObject();archive["keepUploads"]=true;
  const auto blocker=fixture(directory.path(),"blocked","synthetic");archive["root"]=blocker;document["archive"]=archive;
  QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Retry progress must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto idleTooltip=tray->toolTip();const auto idleIcon=tray->icon().pixmap(24,24).toImage();
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  QTRY_COMPARE(source.copies,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryStore store(operations.databasePath());const auto prior=store.operation(id);QCOMPARE(prior.items[0].local.status,QString("failed"));
  const auto red=VisualStyle::icon("brand",QColor("#ffb4ab")).pixmap(24,24).toImage();
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTest::qWait(3100);
  bool failedRetryDone=false;operations.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);failedRetryDone=true;});QTRY_VERIFY(failedRetryDone);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QVERIFY(QFile::remove(blocker));QVERIFY(QDir().mkpath(blocker));
  QTest::qWait(3100);
  bool done=false;operations.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);done=true;});
  QVERIFY(tray->toolTip().contains("Saving local copy"));
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  QCOMPARE(store.operation(id).state,prior.state);
  QTRY_VERIFY(done);QCOMPARE(tray->toolTip(),idleTooltip);QCOMPARE(tray->icon().pixmap(24,24).toImage(),idleIcon);
  const auto retried=store.operation(id);QCOMPARE(retried.items[0].local.status,QString("saved"));QCOMPARE(provider->uploads.load(),1);
  QCOMPARE(retried.state,prior.state);QCOMPARE(retried.copyStatus,prior.copyStatus);
  auto before=prior.items[0].toJson(),after=retried.items[0].toJson();before.remove("local");after.remove("local");QCOMPARE(after,before);
  QString code;operations.retryLocalSave("missing-operation",[&](Error e,QJsonObject){code=e.code;});
  QVERIFY(tray->toolTip().contains("Saving local copy"));QTRY_VERIFY(!code.isEmpty());
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
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Feedback must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  controller.invokeShortcut("region");QTRY_COMPARE(source.acquisitions.size(),1);
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
  // An unsupported payload exercises ArtifactStore's preparation error.
  ClipboardPayload payload;payload.kind="unsupported";
  source.acquisitions.takeFirst()({payload,preparation?Error{}:Error{"fixture_capture_failed","Synthetic capture failure",{}}});
  const auto red=VisualStyle::icon("brand",QColor("#ffb4ab")).pixmap(24,24).toImage();
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTest::qWait(3100);QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void errorRecoveryResumesAnotherActiveStorageRequest_data() {
  QTest::addColumn<bool>("storageFailure");
  QTest::newRow("capture-failure") << false;
  QTest::newRow("upload-failure") << true;
 }
 void errorRecoveryResumesAnotherActiveStorageRequest() {
  QFETCH(bool,storageFailure);
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto document=configuration(directory.path());auto limits=document["limits"].toObject();limits["concurrentUploads"]=2;document["limits"]=limits;
  auto naming=document["naming"].toObject();naming["mode"]="generated";document["naming"]=naming;
  QVERIFY(settings.save(document,{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Feedback must not open a view");},[]{});
  auto* tray=controller.findChild<QSystemTrayIcon*>();const auto blue=tray->icon().pixmap(24,24).toImage();
  const auto green=VisualStyle::icon("brand",QColor("#62d69b")).pixmap(24,24).toImage();
  const auto red=VisualStyle::icon("brand",QColor("#ffb4ab")).pixmap(24,24).toImage();
  auto drain=qScopeGuard([provider]{for(auto complete:std::exchange(provider->completions,{}))complete();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->requests.size(),1);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),green);
  if(storageFailure) {
   controller.invokeShortcut("clipboard");QTRY_COMPARE(provider->requests.size(),2);
   provider->failUpload=true;provider->completions.takeLast()();provider->failUpload=false;
  } else {
   source.defer=true;controller.invokeShortcut("region");QTRY_COMPARE(source.acquisitions.size(),1);
   source.acquisitions.takeFirst()({{}, {"fixture_capture_failed","Synthetic capture failure",{}}});
  }
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  // Progress from the successful request must not mask the error pulse.
  provider->requests.first().progress({"uploading",1,100});
  QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
  QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTest::qWait(3100);QCOMPARE(tray->icon().pixmap(24,24).toImage(),green);
  provider->completions.takeFirst()();QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),blue);
 }

 void aNewErrorRestartsTheFeedbackInterval() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});auto* tray=controller.findChild<QSystemTrayIcon*>();
  const auto blue=tray->icon().pixmap(24,24).toImage();const auto red=VisualStyle::icon("brand",QColor("#ffb4ab")).pixmap(24,24).toImage();
  controller.notifyFailure({"fixture_failed","Synthetic first error",{}});QCOMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTest::qWait(2100);controller.notifyFailure({"fixture_failed","Synthetic second error",{}});
  QTest::qWait(1100);QTRY_COMPARE(tray->icon().pixmap(24,24).toImage(),red);
  QTest::qWait(3100);QCOMPARE(tray->icon().pixmap(24,24).toImage(),blue);
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
  for (const auto& name : QStringList{"Settings and setup", "Upload history", "Recent activity", "About put.gd"}) {
   auto* action = actionNamed(tray, name);
   QVERIFY(action);
   action->trigger();
  }
  QCOMPARE(views, QStringList({"settings", "history", "activity", "about"}));
  QCOMPARE(QApplication::topLevelWidgets(), windows);
  QCOMPARE(quits, 0);
  controller->activate();
  QCOMPARE(views.last(), QString("settings"));
  QVERIFY(QMetaObject::invokeMethod(tray, "activated", Qt::DirectConnection,
                                   Q_ARG(QSystemTrayIcon::ActivationReason, QSystemTrayIcon::Trigger)));
  QCOMPARE(views.last(), QString("activity"));
  auto* quit = actionNamed(tray, "Quit");
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

 void everyCaptureActionWorksWithNoViewsOpen() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(), true), {}));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QTRY_VERIFY(operations.ready());
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
   auto* action = actionNamed(tray, name == "clipboard" ? QString("Upload Clipboard") : "Capture " + name);
   QVERIFY(action);
   action->trigger();
   QTRY_VERIFY(source.actions.contains(name));
   // The acquisition callback is processed asynchronously by the resident.
   // Wait for its terminal event before beginning another screen selection.
   QTRY_VERIFY(terminalActions.contains(name));
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
  QTRY_VERIFY(operations.ready());
 }
 void repeatedNativeCaptureDoesNotOpenViewsOverSelection() {
  QTemporaryDir directory;ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  CaptureSource source;OperationService operations(directory.filePath("data"),settings,providers,source);
  QTRY_VERIFY(operations.ready());QStringList views;
  TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  controller.invokeShortcut("region");
  controller.invokeShortcut("region");
  QVERIFY2(views.isEmpty(),"A repeated native shortcut must not open Activity over an accepted selection");
  QTRY_COMPARE(source.actions.size(),1);
 }
 void nativeFullQueueDoesNotOpenViewsOverSelection() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));
  auto document=configuration(directory.path(),true);auto limits=document["limits"].toObject();
  limits["pendingOperations"]=1;limits["concurrentUploads"]=1;document["limits"]=limits;
  QVERIFY(settings.save(document,{}));CaptureSource source;
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  controller.invokeShortcut("clipboard");controller.invokeShortcut("region");
  QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),2);
  controller.invokeShortcut("region");
  QVERIFY2(views.isEmpty(),"queue_full precedes busy_selecting and must not map Activity over selection");
  QTRY_COMPARE(source.actions.size(),2);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
 }
 void nativeClipboardRejectionDoesNotOpenViewsOverSelection() {
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));
  auto document=configuration(directory.path(),true);auto actions=document["actions"].toObject();
  auto clipboard=actions["clipboard"].toObject();clipboard["destination"]="upload";actions["clipboard"]=clipboard;document["actions"]=actions;
  QVERIFY(settings.save(document,{}));CaptureSource source;
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});
  controller.invokeShortcut("region");
  auto archive=document["archive"].toObject();archive["keepUploads"]=false;document["archive"]=archive;QVERIFY(settings.save(document,settings.revision()));
  controller.invokeShortcut("clipboard");
  QVERIFY2(views.isEmpty(),"Clipboard bypasses the selection guard but admission errors must not map a view");
  QTRY_COMPARE(source.actions,QStringList{"region"});QCOMPARE(source.copies,0);
 }
 void rejectedCaptureCallbackCanDestroyItsTray() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QTRY_VERIFY(operations.ready());
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
  QCOMPARE(opened, QString("activity"));
  QVERIFY(!controller);
  QVERIFY(action.isNull());
 }

 void rejectedCaptureOpensActivityWithoutOwningAWindow() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QTRY_VERIFY(operations.ready());
  QStringList views;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  auto* tray = controller.findChild<QSystemTrayIcon*>();
  auto* action = actionNamed(tray, "Capture region");
  QVERIFY(action);
  action->trigger();
  QTRY_COMPARE(views, QStringList({"activity"}));
  QVERIFY(source.actions.isEmpty());
 }
};

QTEST_MAIN(ResidentTray)
#include "test_tray.moc"
