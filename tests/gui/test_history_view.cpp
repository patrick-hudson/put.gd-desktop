#include "ui/HistoryModel.h"
#include "ui/HistoryWindow.h"
#include "ui/RedesignTheme.h"
#include "fixtures/UiSessionFixture.h"
#include "app/DiagnosticsBundle.h"
#include <QFileInfo>
#include <QLayout>
#include <QListView>
#include <QComboBox>
#include <QMenu>
#include <QClipboard>
#include <QScrollArea>
#include <QLineEdit>
#include "ui/ViewState.h"
#include <QPushButton>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <QScopeGuard>
#include <utility>
#include <QLabel>
#include <QToolButton>
#include <QImage>
#include <QPlainTextEdit>
#include <QPaintEvent>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
using namespace putgd;using namespace putgd::test;
class HistoryPaintObserver final:public QObject {
public:int paints=0;QRegion region;
 bool eventFilter(QObject*,QEvent* event) override {if(event->type()==QEvent::Paint){++paints;region|=static_cast<QPaintEvent*>(event)->region();}return false;}
};
// A real resident sharing job stays pending while this synthetic provider is
// paused. Reopening its view may inspect the job, but must never call share again.
class PendingSharingProvider final:public StorageProvider {
public:
 OpaqueProvider storage;std::atomic_int mutations{0},inspections{0};std::atomic_bool entered{false},released{false};QString outcome="shared";
 ProviderCapabilities capabilities(const Destination& d)const override{auto c=storage.capabilities(d);c.sharing=true;c.privateUploads=true;return c;}
 QString defaultPublicationMode(const Destination&)const override{return "private";}
 QString serializationKey(const Destination& d,const QJsonObject& ref,const QString& name)const override{return storage.serializationKey(d,ref,name);}
 Error validateSettings(const QJsonObject& s)const override{return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& name,std::function<void(NameCheck)> reply)override{storage.checkName(d,name,std::move(reply));}
 void upload(const UploadRequest& request,std::function<void(UploadResult)> reply)override{storage.upload(request,[reply](UploadResult result){result.receipt.directUrl.clear();reply(result);});}
 void stat(const StorageReceipt& receipt,std::function<void(StatResult)> reply)override{storage.stat(receipt,std::move(reply));}
 void reconcile(const UploadRequest& request,std::function<void(StatResult)> reply)override{storage.reconcile(request,std::move(reply));}
 void remove(const DeleteRequest& request,DurableCheckpoint checkpoint,std::function<void(DeleteResult)> reply)override{storage.remove(request,std::move(checkpoint),std::move(reply));}
 void share(const ShareRequest& request,DurableCheckpoint checkpoint,std::function<void(ShareResult)> reply)override{
  if(request.action=="inspect"){++inspections;auto receipt=request.receipt;receipt.directUrl=outcome=="shared"?"https://example.invalid/shared#synthetic-key":QString{};reply({outcome,receipt,{}, {}});return;}
  const QJsonObject progress{{"schemaVersion",1},{"phase","recorded-intent"}};
  checkpoint(progress,[this,request,progress,reply](Error error){
   if(error){reply({"failed",request.receipt,progress,error});return;}++mutations;entered.store(true);
   while(!released.load()&&!request.cancellation->load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
   auto receipt=request.receipt;receipt.directUrl=outcome=="shared"?"https://example.invalid/shared#synthetic-key":QString{};
   reply({outcome,receipt,progress,outcome=="shared"?Error{}:Error{"synthetic_share_failed","Synthetic sharing failure",{}}});
  });
 }
};
class HistoryCopyFailureSource final:public SourceAdapter {
public:quint64 revision=0;bool fail=true;int reads=0,copies=0;
 quint64 clipboardRevision()const override{return revision;}QJsonObject capabilities()const override{return {{"clipboard",true}};}
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++reads;reply({textPayload(),{}});}
 void cancelAcquisition()override{}
 void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;if(fail)reply({"clipboard_unavailable","Synthetic clipboard refusal",{}});else{++revision;reply({});}}
};
class HistoryGui:public QObject {
 Q_OBJECT
 static bool sql(const QString& path,const QString& statement){
  const auto connection=QUuid::createUuid().toString();bool ok=false;
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);if(db.open()){QSqlQuery query(db);ok=query.exec(statement);}db.close();}
  QSqlDatabase::removeDatabase(connection);return ok;
 }

 static void selectRow(QListView* view,int row){view->selectionModel()->select(view->model()->index(row,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);view->setCurrentIndex(view->model()->index(row,0));}
 static int operationRow(QListView* list,const QString& id){auto* model=qobject_cast<HistoryModel*>(list->model());for(int row=0;row<model->rowCount();++row)if(model->record(row)["operationId"]==id)return row;return -1;}
 static bool rowMenuAction(QListView* list,const QString& text){
  list->window()->activateWindow();list->setFocus();QTest::qWait(20);auto triggered=std::make_shared<bool>(false);QTimer::singleShot(0,list,[triggered,text]{for(auto* widget:QApplication::topLevelWidgets())if(auto* menu=qobject_cast<QMenu*>(widget);menu&&menu->isVisible()){for(auto* action:menu->actions())if(action->text()==text&&action->isEnabled()){action->trigger();*triggered=true;break;}menu->close();}});list->setFocus();QTest::keyClick(list,Qt::Key_Menu);QCoreApplication::processEvents();return *triggered;
 }
private slots:
 void externalDeletionAndClearRefreshOpenViews() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);const QSignalBlocker settingsSignals(settings),operationSignals(operations);HistoryWindow window(fixture.session);window.show();
  auto* table=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(table->model());QTRY_COMPARE(model->rowCount(),1);selectRow(table,0);QTest::qWait(100);
  DispatchSessionClient external(fixture.server);const auto id=model->record(0)["id"].toString();QJsonObject preview;external.request("delete.prepare",{{"ids",QJsonArray{id}}},this,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());
  bool accepted=false;external.request("delete.confirm",{{"ids",QJsonArray{id}},{"acknowledgement",preview["acknowledgement"]}},this,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});QTRY_VERIFY(accepted);QTRY_COMPARE(provider->deletes.load(),1);
  QTRY_COMPARE(model->record(0)["remoteStatus"].toString(),QString("removed"));QTRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Deleted from destination"));
  bool cleared=false;external.request("history.clear",{},this,[&](Error e,QJsonObject){QVERIFY(!e);cleared=true;});QTRY_VERIFY(cleared);QTRY_COMPARE(model->rowCount(),0);QVERIFY(window.saveViewState()["selectedIds"].toArray().isEmpty());
 }
 void batchCapturesFoldFilesAndKeepOriginalDetailsRoutedById(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1024,700);window.show();
  auto* list=window.findChild<QListView*>("historyList");auto* inspector=window.findChild<QScrollArea*>("historyInspectorScroll");QVERIFY(list);QVERIFY(inspector);QVERIFY(!inspector->isVisible());auto* empty=window.findChild<QLabel*>("historyEmpty");QTRY_VERIFY(empty->isVisible());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-batch-history";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();op.destination.profileId="original-synthetic-destination";
   for(int i=0;i<2;++i){OperationItem item;item.artifact.id=QString("batch-history-item-%1").arg(i);item.artifact.name=QString("synthetic-file-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";op.items.append(item);}QVERIFY(!ledger.saveOperation(op));}
  window.refresh();QTRY_COMPARE(list->model()->rowCount(),1);QVERIFY(!empty->isVisible());selectRow(list,0);auto* items=window.findChild<QComboBox*>("historyItems");QTRY_COMPARE(items->count(),2);QVERIFY(items->isVisible());auto* details=window.findChild<QPlainTextEdit*>("historyDetails");auto* summary=window.findChild<QLabel*>("historySummary");
  for(int item=0;item<items->count();++item){items->setCurrentIndex(item);QTRY_COMPARE(QJsonDocument::fromJson(details->toPlainText().toUtf8()).object()["id"].toString(),items->currentData().toString());QVERIFY(summary->text().contains("text/plain"));QVERIFY(!summary->text().contains("original-synthetic-destination"));}
  auto* gallery=window.findChild<QToolButton*>("historyGridMode");gallery->click();QCOMPARE(list->viewMode(),QListView::IconMode);QVERIFY(!inspector->isVisible());QCOMPARE(list->selectionModel()->selectedRows().size(),1);const auto saved=window.saveViewState();QVERIFY(saved["gridMode"].toBool());HistoryWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));QTRY_COMPARE(reopened.findChild<QListView*>("historyList")->selectionModel()->selectedRows().size(),1);QCOMPARE(reopened.findChild<QListView*>("historyList")->viewMode(),QListView::IconMode);QCOMPARE(reopened.saveViewState()["selectedItemId"],saved["selectedItemId"]);
  window.findChild<QToolButton*>("historyListMode")->click();QVERIFY(inspector->isVisible());for(auto size:{QSize(1024,700),QSize(1280,700),QSize(1024,700)}){window.resize(size);QTest::qWait(20);QVERIFY(window.width()<=size.width());QVERIFY(window.height()<=size.height());QCOMPARE(list->selectionModel()->selectedRows().size(),1);}list->clearSelection();QVERIFY(!inspector->isVisible());QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void groupedLocalFailureSurvivesRecentOperationEviction_data(){QTest::addColumn<bool>("evicted");QTest::newRow("recent")<<false;QTest::newRow("evicted")<<true;}
 void groupedLocalFailureSurvivesRecentOperationEviction(){
  QFETCH(bool,evicted);QTemporaryDir d;SettingsService settings(d.filePath("config.json"));auto config=configuration(d.path(),true);auto retention=config["retention"].toObject();retention["recentActivityLimit"]=1;config["retention"]=retention;QVERIFY(settings.save(config,{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryStore ledger(operations.databasePath());Operation op;op.id="mixed-local-batch";op.created="2020-01-01T00:00:00.000Z";op.state="failed";op.settings=settings.snapshot();
  for(int i=0;i<2;++i){OperationItem item;item.artifact.id=QString("mixed-item-%1").arg(i);item.artifact.name=QString("mixed-file-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";item.local.status=i?"failed":"saved";if(i)item.local.error={"archive_io","Synthetic local failure",{}};op.items.append(item);}QVERIFY(!ledger.saveOperation(op));
  if(evicted){Operation newer;newer.id="newer-local-operation";newer.created="2021-01-01T00:00:00.000Z";newer.state="completed";newer.settings=settings.snapshot();QVERIFY(!ledger.saveOperation(newer));}Error error;const auto recent=ledger.recent(1,&error);QVERIFY(!error);QCOMPARE(recent.first().toObject()["id"].toString(),evicted?QString("newer-local-operation"):op.id);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);model.refresh();QTRY_COMPARE(model.rowCount(),1);const auto index=model.index(0);QVERIFY(model.data(index,HistoryModel::FailedRole).toBool());QCOMPARE(model.data(index,HistoryModel::PrimaryActionRole).toString(),QString("Retry local save"));QCOMPARE(HistoryModel::statusText(model.record(0)),QString("Local save failed"));const auto children=model.record(0)["records"].toArray();QCOMPARE(children[0].toObject()["local"].toObject()["status"].toString(),QString("saved"));QCOMPARE(children[1].toObject()["local"].toObject()["status"].toString(),QString("failed"));
  op.items[1].local.status="saved";op.items[1].local.error={};op.state="completed";QVERIFY(!ledger.saveOperation(op));model.refresh();QTRY_VERIFY(!model.data(index,HistoryModel::FailedRole).toBool());QCOMPARE(model.data(index,HistoryModel::PrimaryActionRole).toString(),QString("Copy result"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void recreatedHistoryRestoresRawFiltersSelectionAndClearsVanishedIds(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="state-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="state-history-id";item.artifact.name="state.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);QJsonObject state;
  {HistoryWindow window(fixture.session);auto* table=window.findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);window.findChild<QLineEdit*>("historySearch")->setText("raw unapplied search");window.findChild<QLineEdit*>("historyFilterfrom")->setText("invalid raw date");state=window.saveViewState();}
  QVERIFY(!validateUiState({{"schemaVersion",1},{"history",state}}));QVERIFY(!QJsonDocument(state).toJson().contains("state.txt"));HistoryWindow restored(fixture.session);QVERIFY(restored.restoreViewState(state));auto* table=restored.findChild<QListView*>("historyList");QTRY_COMPARE(table->selectionModel()->selectedRows().size(),1);QCOMPARE(restored.findChild<QLineEdit*>("historySearch")->text(),QString("raw unapplied search"));QCOMPARE(restored.findChild<QLineEdit*>("historyFilterfrom")->text(),QString("invalid raw date"));
  bool cleared=false;history.clear([&](Error e,QJsonObject){QVERIFY(!e);cleared=true;});QTRY_VERIFY(cleared);QTRY_COMPARE(table->model()->rowCount(),0);QVERIFY(restored.saveViewState()["selectedIds"].toArray().isEmpty());QCOMPARE(source.reads,0);
 }
 void restoringHistoryKeepsActiveDeletionCancellation() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  std::atomic_bool entered{false},released{false};provider->checkpointObserver=[&](const DeleteRequest&){entered.store(true);while(!released.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));};
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);auto original=std::make_unique<HistoryWindow>(fixture.session);auto& window=*original;window.show();auto* table=window.findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);
  auto drain=qScopeGuard([&]{released.store(true);});
  QTimer accept;accept.setInterval(10);connect(&accept,&QTimer::timeout,this,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(widget);box&&box->windowTitle()=="Confirm exact remote deletion")box->button(QMessageBox::Yes)->click();});accept.start();
  window.findChild<QPushButton*>("deleteHistoryRemote")->click();QTRY_VERIFY(entered.load());accept.stop();
  const auto saved=window.saveViewState();QVERIFY(!saved["deletionId"].toString().isEmpty());original.reset();HistoryWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));reopened.show();QTRY_VERIFY(!reopened.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().isEmpty());QPushButton* cancel=nullptr;for(auto* button:reopened.findChildren<QPushButton*>())if(button->text()=="Cancel deletion")cancel=button;QVERIFY(cancel);
  cancel->click();QLabel* status=nullptr;for(auto* label:reopened.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);QTRY_VERIFY(status->text().contains("Cancellation requested"));
  released.store(true);operations.shutdown(); // Drain the delayed worker while its service/fixture still exist.
 }
 void recreatedHistoryResumesSharingInspectionWithoutReplay_data(){
  QTest::addColumn<QString>("outcome");QTest::newRow("shared")<<QString("shared");QTest::newRow("failed")<<QString("failed");
 }
 void recreatedHistoryResumesSharingInspectionWithoutReplay(){
  QFETCH(QString,outcome);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<PendingSharingProvider>();provider->outcome=outcome;registry.add("opaque",provider);
  // Keep journal directory metadata outside the config directory watcher.
  SettingsService settings(d.filePath("config/config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->storage.uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  auto drain=qScopeGuard([&]{provider->released.store(true);operations.shutdown();});
  auto original=std::make_unique<HistoryWindow>(fixture.session);original->show();auto* table=original->findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);auto* create=original->findChild<QPushButton*>("shareHistoryLink");QTRY_VERIFY(create->isVisible()&&create->isEnabled());
  QJsonObject acceptedState;connect(original.get(),&HistoryWindow::viewStateChanged,this,[&]{const auto state=original->saveViewState();if(!state["sharingId"].toString().isEmpty())acceptedState=state;});
  create->click();QTRY_VERIFY(provider->entered.load());QTRY_VERIFY(!acceptedState["sharingId"].toString().isEmpty());QVERIFY(!validateUiState({{"schemaVersion",1},{"history",acceptedState}}));
  auto legacyState=acceptedState;legacyState.remove("sharingId");QVERIFY(!validateUiState({{"schemaVersion",1},{"history",legacyState}}));auto invalidState=acceptedState;invalidState["sharingId"]=QJsonObject{{"unsupported","value"}};QCOMPARE(validateUiState({{"schemaVersion",1},{"history",invalidState}}).code,QString("ui_state_invalid"));
  // Selection is restored before the asynchronous inspector details arrive.
  fixture.client.replyDelayMs["history.detail"]=350;original.reset();HistoryWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(acceptedState));reopened.show();QCOMPARE(reopened.saveViewState()["sharingId"].toString(),acceptedState["sharingId"].toString());QTRY_COMPARE(reopened.findChild<QListView*>("historyList")->selectionModel()->selectedRows().size(),1);QTRY_VERIFY(!reopened.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().isEmpty());QVERIFY(!reopened.findChild<QPushButton*>("shareHistoryLink")->isEnabled());
  provider->released.store(true);QLabel* status=nullptr;for(auto* label:reopened.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);
  QTRY_VERIFY(status->text().contains("Link: "+outcome));if(outcome=="failed")QVERIFY(status->text().contains("Synthetic sharing failure"));
  QTRY_VERIFY(reopened.findChild<QLabel*>("historySummary")->text().contains(outcome=="shared"?"Shared link":"Sharing failed"));QVERIFY(status->text().contains("Link: "+outcome));
  QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,0);
 }
 void stoppedSharingJournalCanBeInspectedAfterStorageRecovers(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<PendingSharingProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->storage.uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);auto drain=qScopeGuard([&]{provider->released.store(true);operations.shutdown();});HistoryWindow window(fixture.session);window.show();
  auto* table=window.findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);auto* create=window.findChild<QPushButton*>("shareHistoryLink");auto* inspect=window.findChild<QPushButton*>("inspectHistoryLink");QTRY_VERIFY(create->isVisible()&&create->isEnabled());
  create->click();QTRY_VERIFY(provider->entered.load());QTRY_VERIFY(!window.saveViewState()["sharingId"].toString().isEmpty());const auto stoppedId=window.saveViewState()["sharingId"].toString();
  QVERIFY(sql(operations.databasePath(),"CREATE TRIGGER fail_share BEFORE UPDATE ON sharing_operations BEGIN SELECT RAISE(FAIL,'synthetic journal failure'); END"));provider->released.store(true);
  QLabel* status=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);QTRY_VERIFY(status->text().contains("Link: unknown"));QVERIFY(status->text().contains("Persistent journal"));
  HistoryStore store(operations.databasePath());QCOMPARE(store.sharing(stoppedId)["state"].toString(),QString("running"));QTRY_VERIFY(inspect->isVisible()&&inspect->isEnabled());QVERIFY(!create->isEnabled());QVERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->inspections.load(),0);
  QVERIFY(sql(operations.databasePath(),"DROP TRIGGER fail_share"));inspect->click();QTRY_VERIFY(window.saveViewState()["sharingId"].toString()!=stoppedId);QTRY_COMPARE(provider->inspections.load(),1);QTRY_VERIFY(status->text().contains("Link: shared"));QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Shared link"));
  QCOMPARE(store.sharing(stoppedId)["state"].toString(),QString("interrupted"));QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,0);
 }
 void imageInspectorAndExpandedControlsFitCompactWindow() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto imagePath=d.filePath("synthetic-preview.png");QImage image(320,180,QImage::Format_RGB32);image.fill(QColor("#a9f1cb"));QVERIFY(image.save(imagePath));
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-image-inspector";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-preview-item";item.artifact.name="synthetic-preview.png";item.artifact.mime="image/png";item.artifact.category="images";item.artifact.previewPath=imagePath;item.artifact.bytes=1572864;item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);HistoryPaintObserver paint;window.installEventFilter(&paint);window.resize(1000,700);window.show();auto* table=window.findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);auto* preview=window.findChild<QLabel*>("historyPreview");QVERIFY(preview);QTRY_VERIFY(!preview->pixmap().isNull());QVERIFY(preview->isVisible());QVERIFY(preview->height()>0);QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("1.5 MiB"));
  QToolButton *filters=nullptr,*manage=nullptr;for(auto* button:window.findChildren<QToolButton*>()){if(button->text()=="Filters")filters=button;if(button->text()=="Manage")manage=button;}QVERIFY(filters);QVERIFY(manage);filters->setChecked(true);manage->setChecked(true);QTest::qWait(20);
  QVERIFY2(window.width()<=1000&&window.height()<=700,qPrintable(QString("Expanded history grew to %1×%2").arg(window.width()).arg(window.height())));
  for(const auto& name:QStringList{"copyHistoryLink","deleteHistoryRemote"}){auto* button=window.findChild<QPushButton*>(name);window.findChild<QScrollArea*>("historyInspectorScroll")->ensureWidgetVisible(button);QVERIFY(button->isVisible());QVERIFY(window.rect().contains(button->mapTo(&window,button->rect().bottomRight())));}
  QVERIFY(window.rect().contains(manage->mapTo(&window,manage->rect().bottomRight())));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
  for(int cycle=0;cycle<3;++cycle){
   window.hide();QVERIFY2(preview->pixmap().isNull(),"Hidden history must release its decoded thumbnail");QTRY_VERIFY_WITH_TIMEOUT(!window.testAttribute(Qt::WA_WState_Created),500);QVERIFY(!window.backingStore());
   QCOMPARE(window.findChild<QListView*>("historyList"),table);QVERIFY(filters->isChecked());QVERIFY(manage->isChecked());QCOMPARE(table->selectionModel()->selectedRows().size(),1);
   paint.paints=0;paint.region=QRegion{};window.show();QTRY_VERIFY(window.testAttribute(Qt::WA_WState_Created));QTRY_VERIFY(paint.paints>0);QVERIFY(paint.region.boundingRect().contains(window.rect()));QTRY_VERIFY(!preview->pixmap().isNull());
  }
  table->clearSelection();QVERIFY(preview->pixmap().isNull());

 }

 // #112: a video's poster shows a play glyph and its length, in the list and
 // in the inspector, and the length comes from the poster the helper wrote.
 void videoPosterCarriesPlayGlyphAndLength() {
  QCOMPARE(HistoryModel::duration(0),QString());QCOMPARE(HistoryModel::duration(400),QString("0:01"));QCOMPARE(HistoryModel::duration(65000),QString("1:05"));QCOMPARE(HistoryModel::duration(3723000),QString("1:02:03"));
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto posterPath=d.filePath("synthetic-poster.png");QImage poster(256,144,QImage::Format_RGB32);poster.fill(QColor("#f0f0f0"));poster.setText(HistoryModel::durationKey,"65000");QVERIFY(poster.save(posterPath));
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-video-inspector";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-video-item";item.artifact.name="recording.mp4";item.artifact.mime="video/mp4";item.artifact.category="videos";item.artifact.previewPath=posterPath;item.artifact.bytes=1572864;item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1000,700);window.show();auto* table=window.findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),1);
  const auto index=table->model()->index(0,0);QTRY_VERIFY(!qvariant_cast<QImage>(index.data(HistoryModel::ThumbnailRole)).isNull());QCOMPARE(qvariant_cast<QImage>(index.data(HistoryModel::ThumbnailRole)).text(HistoryModel::durationKey),QString("65000"));
  // The list row draws the scrim-backed glyph over the light poster.
  // The light poster gets a scrim-backed glyph: mid-grey pixels no plain poster or dark row has.
  // Only inside the poster's own pixels (a uniform 240 grey), never in the row's text.
  auto scrim=[](const QImage& image){QRect poster;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(qGray(image.pixel(x,y))==240)poster|=QRect(x,y,1,1);
   for(int y=poster.top();y<=poster.bottom();++y)for(int x=poster.left();x<=poster.right();++x){const int g=qGray(image.pixel(x,y));if(g>70&&g<130)return true;}return false;};
  QTest::qWait(50);QVERIFY(scrim(table->grab(table->visualRect(index)).toImage()));
  selectRow(table,0);auto* preview=window.findChild<QLabel*>("historyPreview");QTRY_VERIFY(!preview->pixmap().isNull());QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("1:05"));QVERIFY(scrim(preview->pixmap().toImage()));
 }
 // #190: a selected image or video whose preview is missing is asked for
 // once the selection settles; what the resident makes shows in the inspector
 // and the list. One with nothing retained keeps "Preview unavailable" and is
 // not asked for again.
 void missingPreviewIsMadeOnSelection() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QImage capture(320,180,QImage::Format_RGB32);capture.fill(QColor("#2a7fd4"));QByteArray png;{QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QVERIFY(capture.save(&buffer,"PNG"));}
  const auto imageId=QUuid::createUuid().toString(QUuid::WithoutBraces),videoId=QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto artifact=d.filePath("data/artifacts/synthetic-image-op/"+imageId);QVERIFY(QDir().mkpath(QFileInfo(artifact).path()));{QFile file(artifact);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(png),png.size());}
  const auto cleaned=d.filePath("data/previews/"+imageId+".png");
  {HistoryStore ledger(operations.databasePath());
   // A stored capture whose preview cleanup removed.
   Operation image;image.id="synthetic-image-op";image.created="2020-01-02T00:00:00.000Z";image.state="completed";image.settings=settings.snapshot();OperationItem stored;stored.artifact.id=imageId;stored.artifact.path=artifact;stored.artifact.name="capture.png";stored.artifact.mime="image/png";stored.artifact.category="images";stored.artifact.bytes=png.size();stored.artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex());stored.artifact.previewPath=cleaned;stored.uploadStatus="local_only";image.items.append(stored);QVERIFY(!ledger.saveOperation(image));
   // A video stored before posters whose file is gone.
   Operation video;video.id="synthetic-video-op";video.created="2020-01-01T00:00:00.000Z";video.state="completed";video.settings=settings.snapshot();OperationItem gone;gone.artifact.id=videoId;gone.artifact.path=d.filePath("data/artifacts/synthetic-video-op/"+videoId);gone.artifact.name="recording.mp4";gone.artifact.mime="video/mp4";gone.artifact.category="videos";gone.artifact.bytes=4096;gone.uploadStatus="uploaded";video.items.append(gone);QVERIFY(!ledger.saveOperation(video));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);QStringList asked;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="history.preview")asked.append(params["id"].toString());};
  // Settings status changes reload History several times a second here, as
  // resident changes do during an upload: the wait must survive reloads.
  HistoryWindow window(fixture.session);window.resize(1000,700);window.show();auto* table=window.findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),2);auto* preview=window.findChild<QLabel*>("historyPreview");
  QCOMPARE(table->model()->index(0,0).data(HistoryModel::RecordRole).toJsonObject()["id"].toString(),imageId);
  // Passing over the video on the way to the image asks only for the image.
  selectRow(table,1);QTest::qWait(100);selectRow(table,0);QTRY_VERIFY(!preview->pixmap().isNull());QCOMPARE(asked,QStringList{imageId});
  QVERIFY(QFileInfo(cleaned).isFile());QTRY_VERIFY(!qvariant_cast<QImage>(table->model()->index(0,0).data(HistoryModel::ThumbnailRole)).isNull());
  QCOMPARE(preview->pixmap().toImage().pixelColor(preview->pixmap().width()/2,preview->pixmap().height()/2),QColor("#2a7fd4"));
  selectRow(table,1);QTRY_COMPARE(asked.size(),2);QCOMPARE(asked.last(),videoId);QTRY_COMPARE(preview->text(),QString("Preview unavailable"));QVERIFY(preview->pixmap().isNull());
  selectRow(table,0);QTRY_VERIFY(!preview->pixmap().isNull());selectRow(table,1);QTRY_COMPARE(preview->text(),QString("Preview unavailable"));QTest::qWait(600);QCOMPARE(asked.size(),2);
 }
 void openingHistoryDuringInitializationRefreshesWhenReady() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));QVERIFY(QDir().mkpath(d.filePath("data")));
  {HistoryStore ledger(d.filePath("data/history.sqlite"));Operation op;op.id="synthetic-startup";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="startup-item";item.artifact.name="startup.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QVERIFY(!operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);auto* table=window.findChild<QListView*>("historyList");QLabel* status=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);QVERIFY(status->text().contains("Waiting for the resident journal"));QTRY_VERIFY(operations.ready());QTRY_COMPARE(table->model()->rowCount(),1);QTRY_VERIFY(!status->text().contains("Waiting for the resident journal"));QVERIFY(status->text().contains("Select a capture"));QCOMPARE(source.reads,0);
 }
 void tenThousandEntryLedgerPagesAndFiltersOffline() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-large-ledger";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();for(int i=0;i<10000;++i){OperationItem item;item.artifact.id=QString("ledger-id-%1").arg(i);item.artifact.name=QString("ledger-name-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";op.items.append(item);}QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);int ticks=0;QTimer pulse;pulse.setInterval(1);connect(&pulse,&QTimer::timeout,this,[&]{++ticks;});pulse.start();UiHistoryQuery page;page.offset=9950;model.setQuery(page);QTRY_COMPARE(model.total(),qint64(10000));QCOMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["records"].toArray().size(),50);QVERIFY(ticks>0);page.offset=0;page.search="ledger-name-9999.txt";model.setQuery(page);QTRY_COMPARE(model.rowCount(),1);QTRY_COMPARE(model.record(0)["name"].toString(),QString("ledger-name-9999.txt"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void cancellingExactDeletionConfirmationSendsNoDelete() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto s){return registry.validate(id,s);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);operationId=data["operationId"].toString();});QTRY_COMPARE(provider->uploads.load(),1);QTRY_VERIFY(!operationId.isEmpty());QJsonObject completed;operations.inspect(operationId,[&](Error,QJsonObject data){completed=data;});QTRY_VERIFY(!completed.isEmpty());
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);auto* table=window.findChild<QListView*>("historyList");QTRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);bool confirmed=false;QTimer dismiss;dismiss.setInterval(10);connect(&dismiss,&QTimer::timeout,this,[&]{for(auto* widget:QApplication::topLevelWidgets()){auto* box=qobject_cast<QMessageBox*>(widget);if(box&&box->windowTitle()=="Confirm exact remote deletion"){confirmed=box->text().contains("Original destination / exact target")&&box->text().contains("Remove only the recorded opaque file revision");box->done(QMessageBox::Cancel);}}});dismiss.start();window.findChild<QPushButton*>("deleteHistoryRemote")->click();QTRY_VERIFY(confirmed);QCOMPARE(provider->deletes.load(),0);QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);
 }
 void unifiedActivityCancelRetryAndCopyUseResidentActions(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();source.defer=true;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.acquisitions.size(),1);QTRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);fixture.client.replyDelayMs["history.list"]=200;window.refresh();QTest::qWait(30);source.acquisitions.takeFirst()({source.payload,{}});QTRY_COMPARE(provider->uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QTRY_COMPARE(list->model()->rowCount(),1);QTRY_COMPARE(list->selectionModel()->selectedRows().size(),1);fixture.client.replyDelayMs["history.list"]=0;source.defer=false;auto* retry=window.findChild<QPushButton*>("retryOperation");QTRY_VERIFY2(retry->isVisible(),qPrintable(QString("selection=%1 failed=%2 current=%3").arg(list->selectionModel()->selectedRows().size()).arg(list->model()->data(list->model()->index(0,0),HistoryModel::FailedRole).toBool()).arg(list->currentIndex().isValid())));provider->failUpload=false;retry->click();QTRY_COMPARE(provider->uploads.load(),2);QTRY_COMPARE(source.copies,1);QTRY_VERIFY(window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());window.findChild<QPushButton*>("copyHistoryLink")->click();QTRY_COMPARE(source.copies,2);QCOMPARE(source.reads,1);
  source.defer=true;operations.invoke("region",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.reads,2);QTRY_COMPARE(list->model()->rowCount(),2);selectRow(list,0);auto* cancel=window.findChild<QPushButton*>("cancelOperation");QTRY_VERIFY(cancel->isVisible());cancel->click();QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QTRY_COMPARE(list->model()->rowCount(),1);auto* show=window.findChild<QToolButton*>("historyShowCancelled");QTRY_VERIFY(show->isVisible());show->click();QTRY_COMPARE(list->model()->rowCount(),2);QVERIFY(window.saveViewState()["showCancelled"].toBool());QCOMPARE(provider->uploads.load(),2);
 }
 void uploadProgressRendersBeforeCompletionDuringDelayedRecentReplies(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();source.defer=true;
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});QTRY_COMPARE(source.acquisitions.size(),1);QTRY_COMPARE(model->rowCount(),1);QTRY_VERIFY(model->data(model->index(0),HistoryModel::ActiveRole).toBool());QTRY_VERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("Reading the clipboard"));selectRow(list,0);
  source.acquisitions.takeFirst()({source.payload,{}});QTRY_COMPARE(provider->completions.size(),1);auto report=provider->requests.first().progress;QVERIFY(bool(report));report({"uploading",1048576,2097152});QTRY_COMPARE(model->data(model->index(0),HistoryModel::ProgressRole).toInt(),50);QVERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("50%"));report({"uploading",2097152,2097152});QTRY_COMPARE(model->data(model->index(0),HistoryModel::ProgressRole).toInt(),100);QVERIFY(model->data(model->index(0),HistoryModel::ActiveRole).toBool());QCOMPARE(source.copies,0);
  fixture.client.replyDelayMs["operation.recent"]=600;int reports=0;QTimer updates;updates.setInterval(50);connect(&updates,&QTimer::timeout,this,[&]{report({"uploading",1048576+ ++reports,{}});});updates.start();QTRY_VERIFY_WITH_TIMEOUT(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("1 MiB transferred"),2500);QVERIFY(reports>10);QCOMPARE(model->data(model->index(0),HistoryModel::ProgressRole).toInt(),-1);QCOMPARE(list->selectionModel()->selectedRows().size(),1);QCOMPARE(model->record(0)["operationId"].toString(),id);QCOMPARE(source.copies,0);
  updates.stop();fixture.client.replyDelayMs["operation.recent"]=0;provider->failUpload=true;provider->completions.takeFirst()();QTRY_VERIFY(model->data(model->index(0),HistoryModel::FailedRole).toBool());QTRY_VERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("Synthetic failure"));
 }
 void queuedRetryRetainsOriginalIdentityAndCanBeCancelledFromHistory(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");HistoryStore journal(operations.databasePath());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);const auto original=journal.operation(id);provider->failUpload=false;operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->completions.size(),1);QTRY_VERIFY(operationRow(list,id)>=0);selectRow(list,operationRow(list,id));QTRY_VERIFY(window.findChild<QPushButton*>("retryOperation")->isVisible());window.findChild<QPushButton*>("retryOperation")->click();QTRY_COMPARE(journal.operation(id).phase,QString("queued"));QCOMPARE(journal.operation(id).destination.toJson(),original.destination.toJson());QCOMPARE(journal.operation(id).items.first().publicationId,original.items.first().publicationId);QCOMPARE(provider->completions.size(),1);
  auto* model=qobject_cast<HistoryModel*>(list->model());QTRY_VERIFY(operationRow(list,id)>=0);QTRY_VERIFY(model->data(model->index(operationRow(list,id)),HistoryModel::SubtitleRole).toString().contains("Waiting to upload"));QTRY_VERIFY(window.findChild<QPushButton*>("cancelOperation")->isVisible());window.findChild<QPushButton*>("cancelOperation")->click();QTRY_COMPARE(journal.operation(id).state,QString("cancelled"));QCOMPARE(provider->completions.size(),1);QCOMPARE(source.reads,2);
 }
 void failedAutomaticCopyCanRetryTheFullResultWithoutAnotherUpload(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));HistoryCopyFailureSource source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});QTRY_COMPARE(source.copies,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());QTRY_COMPARE(model->rowCount(),1);selectRow(list,0);QTRY_VERIFY(window.findChild<QLabel*>("historyOutcome")->text().contains("Could not copy"));QVERIFY(!model->data(model->index(0)).toString().contains(id));QVERIFY(!model->data(model->index(0),Qt::ToolTipRole).toString().contains(id));QVERIFY(!window.findChild<QLabel*>("historySummary")->text().contains(id));QVERIFY(!window.findChild<QPlainTextEdit*>("historyDetails")->isVisible());source.fail=false;QVERIFY(rowMenuAction(list,"Copy result"));QTRY_COMPARE(source.copies,2);QTRY_VERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("Link copied"));QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);QVERIFY(rowMenuAction(list,"Copy details"));QTRY_VERIFY(QApplication::clipboard()->text().contains(id));QVERIFY(!QApplication::clipboard()->text().contains("synthetic source content"));
 }
 void failedUploadSavesDiagnosticsForThatOperation(){
  // #122: a failed row offers Save diagnostics for this upload, for that one operation.
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});
  auto config=configuration(d.path());QTemporaryDir out;auto preferences=config["preferences"].toObject();preferences["diagnostics"]=QJsonObject{{"debugLogging",false},{"packageFolder",out.path()}};config["preferences"]=preferences;QVERIFY(settings.save(config,{}));
  Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  DiagnosticsBundles bundles(operations,d.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);QJsonArray notified;fixture.server.setDiagnosticsNotifications([&](const QJsonObject& saved){notified.append(saved);},[](const QString&){return Error{};});
  HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});QTRY_COMPARE(provider->uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QTRY_COMPARE(list->model()->rowCount(),1);QTRY_VERIFY(list->model()->data(list->model()->index(0,0),HistoryModel::FailedRole).toBool());selectRow(list,0);
  auto* save=window.findChild<QPushButton*>("saveHistoryDiagnostics");QVERIFY(save);QTRY_VERIFY(save->isVisible());QCOMPARE(save->text(),QString("Save diagnostics for this upload"));
  save->click();QTRY_COMPARE(notified.size(),1);
  const auto path=notified[0].toObject()["path"].toString();QCOMPARE(QFileInfo(path).absolutePath(),QFileInfo(out.path()).absoluteFilePath());QVERIFY(QFileInfo(path).isFile());QCOMPARE(notified[0].toObject()["operations"].toInt(),1);
  // The UI-driving path (ui.historySaveDiagnostics) selects the row by operation ID.
  bool done=false;Error error;QJsonObject result;
  fixture.server.setUiCommand([&](const QString& name,const QJsonObject& params,JsonReply reply){QCOMPARE(name,QString("historySaveDiagnostics"));window.saveDiagnosticsFor(params["id"].toString(),std::move(reply));});
  QTest::qWait(1100); // a second package in the same second gets a -2 suffix; either is fine
  fixture.server.dispatch({{"schemaVersion",1},{"requestId","ui"},{"session",fixture.server.socketPath()},{"method","ui.historySaveDiagnostics"},{"params",QJsonObject{{"id",id}}}},[&](Error e,QJsonObject r){error=e;result=r;done=true;});
  QTRY_VERIFY_WITH_TIMEOUT(done,15000);QVERIFY2(!error,qPrintable(error.message));QVERIFY(QFileInfo(result["path"].toString()).isFile());QCOMPARE(notified.size(),2);
  done=false;window.saveDiagnosticsFor("missing-operation",[&](Error e,QJsonObject){error=e;done=true;});QVERIFY(done);QCOMPARE(error.code,QString("history_row_not_found"));
 }
 void localSaveRetryUsesRetainedArtifactWithoutRecaptureOrUpload(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto config=configuration(d.path());const auto blocked=d.filePath("blocked-archive");QVERIFY(!putgd::test::fixture(d.path(),"blocked-archive","synthetic blocker").isEmpty());auto archive=config["archive"].toObject();archive["root"]=blocked;archive["keepUploads"]=true;config["archive"]=archive;QVERIFY(settings.save(config,{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});QTRY_COMPARE(provider->uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);auto* list=window.findChild<QListView*>("historyList");QTRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Local save failed"));QVERIFY(QFile::remove(blocked));QVERIFY(QDir().mkpath(blocked));QVERIFY(rowMenuAction(list,"Retry local save"));QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Saved locally"));HistoryStore ledger(operations.databasePath());QVERIFY(QFileInfo::exists(ledger.operation(id).items.first().local.path));QCOMPARE(source.reads,1);QCOMPARE(provider->uploads.load(),1);
 }
 void localOnlyFailureRetriesLocalSaveAndRecoveredResultCanBeCopied(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));auto config=configuration(d.path(),true);const auto blocked=d.filePath("blocked-local-archive");QVERIFY(!putgd::test::fixture(d.path(),"blocked-local-archive","synthetic blocker").isEmpty());auto archive=config["archive"].toObject();archive["root"]=blocked;config["archive"]=archive;QVERIFY(settings.save(config,{}));ProviderRegistry registry;Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});QTRY_COMPARE(source.reads,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());QTRY_COMPARE(model->rowCount(),1);selectRow(list,0);QTRY_COMPARE(model->data(model->index(0),HistoryModel::PrimaryActionRole).toString(),QString("Retry local save"));auto* retry=window.findChild<QPushButton*>("retryOperation");QTRY_COMPARE(retry->text(),QString("Retry local save"));QVERIFY(QFile::remove(blocked));QVERIFY(QDir().mkpath(blocked));retry->click();HistoryStore ledger(operations.databasePath());QTRY_COMPARE(ledger.operation(id).items.first().local.status,QString("saved"));QVERIFY(QFileInfo::exists(ledger.operation(id).items.first().local.path));QTRY_VERIFY(!model->data(model->index(0),HistoryModel::FailedRole).toBool());QTRY_COMPARE(model->data(model->index(0),HistoryModel::PrimaryActionRole).toString(),QString("Copy result"));QTRY_VERIFY(window.findChild<QLabel*>("historyOutcome")->text().contains("Saved locally"));auto* copy=window.findChild<QPushButton*>("copyHistoryLink");QTRY_COMPARE(copy->text(),QString("Copy result"));QTRY_VERIFY(copy->isEnabled());copy->click();QTRY_COMPARE(source.copies,1);QCOMPARE(source.copied.kind,QString("text"));QCOMPARE(source.copied.text,QString::fromUtf8(source.payload.bytes));QCOMPARE(source.reads,1);
 }
 void originalImageCopyUsesResidentOriginalNotPreview(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload.kind="image";source.payload.image=QImage(91,57,QImage::Format_RGB32);source.payload.image.fill(Qt::magenta);OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.reads,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");QTRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);auto* copy=window.findChild<QPushButton*>("copyHistoryImage");QTRY_VERIFY(copy->isEnabled());QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("91×57"));QTRY_COMPARE(source.copies,1);QApplication::clipboard()->setText("helper clipboard sentinel");copy->click();QTRY_COMPARE(source.copies,2);QCOMPARE(source.copied.kind,QString("image"));const auto original=QImage::fromData(source.copied.encodedImage,"PNG");QCOMPARE(original.size(),QSize(91,57));QCOMPARE(original.pixelColor(0,0),QColor(Qt::magenta));QCOMPARE(QApplication::clipboard()->text(),QString("helper clipboard sentinel"));window.close();QCOMPARE(source.copied.encodedImage.isEmpty(),false);QCOMPARE(source.reads,1);
 }
 void editAgainOpensTheRetainedLocalImageAndHeaderMenuOffersOtherSources(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload.kind="image";source.payload.image=QImage(40,30,QImage::Format_RGB32);source.payload.image.fill(Qt::cyan);
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();
  QStringList requests;connect(&window,&HistoryWindow::editRequested,this,[&](const QString& from,const QString& path){requests.append(from+"|"+path);});int chooser=0;connect(&window,&HistoryWindow::chooseImageRequested,this,[&]{++chooser;});
  auto* header=window.findChild<QPushButton*>("historyEditImage");QVERIFY(header);QVERIFY(header->menu());QStringList labels;for(auto* action:header->menu()->actions())labels.append(action->text());
  QCOMPARE(labels,QStringList({"Edit clipboard image","Edit an image file…"}));
  // The label fits in any font: the button sizes from its child row in the current font and theme.
  RedesignTheme::apply(window);auto* text=header->findChild<QLabel*>("historyEditText");auto* chevron=header->findChild<QLabel*>("historyEditChevron");QVERIFY(text&&chevron);QCOMPARE(text->text(),QString("Edit image"));
  for(const int extra:{0,6}){QFont font=header->font();if(font.pixelSize()>0)font.setPixelSize(font.pixelSize()+extra);else font.setPointSizeF(font.pointSizeF()+extra);header->setFont(font);header->adjustSize();header->layout()->activate();
   QVERIFY2(text->width()>=text->fontMetrics().horizontalAdvance(text->text()),qPrintable(QString("%1 < %2").arg(text->width()).arg(text->fontMetrics().horizontalAdvance(text->text()))));
   QVERIFY(chevron->geometry().left()>=text->geometry().right());QVERIFY(chevron->geometry().right()<header->width());}
  header->menu()->actions()[0]->trigger();header->menu()->actions()[1]->trigger();QCOMPARE(requests,QStringList({"clipboard|"}));QCOMPARE(chooser,1);requests.clear();
  auto* list=window.findChild<QListView*>("historyList");QTRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);auto* again=window.findChild<QPushButton*>("editHistoryImage");QVERIFY(again);QTRY_VERIFY(again->isEnabled());
  again->click();QCOMPARE(requests.size(),1);QVERIFY(requests.first().startsWith("file|"));const auto path=requests.first().section('|',1);QVERIFY(QFileInfo(path).isAbsolute());
  const QImage retained(path);QCOMPARE(retained.size(),QSize(40,30));QCOMPARE(retained.pixelColor(0,0),QColor(Qt::cyan));
  // Without the retained file the button explains why instead of opening the editor.
  QVERIFY(QFile::remove(path));list->clearSelection();QTRY_VERIFY(!again->isEnabled());selectRow(list,0);QTest::qWait(300);QVERIFY(!again->isEnabled());QVERIFY(again->toolTip().contains("local copy"));
 }
 void failureThatStoredNothingOffersNoRetry(){
  // An edit refused before anything was stored (here, not an image) has no file
  // to upload again. History names the failure and offers no retry.
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto text=d.filePath("notes.txt");{QFile file(text);QVERIFY(file.open(QIODevice::WriteOnly));file.write("not an image");}
  operations.invoke("edit",[](Error,QJsonObject){},{{"source","file"},{"path",text}});QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();
  auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());QTRY_COMPARE(model->rowCount(),1);
  const auto row=model->index(0);const auto record=model->record(0);
  QVERIFY(row.data(HistoryModel::FailedRole).toBool());QVERIFY(HistoryModel::storedNothing(record));
  QCOMPARE(HistoryModel::failureLabel(record),QString("Edit failed"));QVERIFY(row.data(HistoryModel::PrimaryActionRole).toString().isEmpty());
  QVERIFY(!HistoryModel::statusText(record).contains("Upload"));QVERIFY(HistoryModel::statusText(record).contains("PNG or JPEG"));
  selectRow(list,0);QTest::qWait(200);QVERIFY(!window.findChild<QPushButton*>("retryOperation")->isVisible());
  // Disabled row-menu entries must look disabled in the app theme.
  RedesignTheme::apply(window);QMenu menu(&window);auto* enabled=menu.addAction("Copy details");auto* disabled=menu.addAction("Retry upload");disabled->setEnabled(false);
  menu.popup(window.mapToGlobal(QPoint(20,20)));QTRY_VERIFY(menu.isVisible());const auto image=menu.grab().toImage();
  auto brightest=[&](QAction* action){int best=0;const auto r=menu.actionGeometry(action);for(int y=r.top();y<=r.bottom();++y)for(int x=r.left();x<=r.right();++x)best=std::max(best,qGray(image.pixel(x,y)));return best;};
  QVERIFY2(brightest(disabled)+40<brightest(enabled),qPrintable(QString("%1 vs %2").arg(brightest(disabled)).arg(brightest(enabled))));menu.close();
 }
 void offlineSearchUsesOriginalReceiptsAndRemovedLinksAreDisabled() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="old-item";item.artifact.name="old-image.png";item.artifact.bytes=2147483649LL;item.artifact.mime="image/png";item.artifact.category="images";item.uploadStatus="uploaded";item.remoteStatus="removed";item.receipt.directUrl="https://example.invalid/original.png";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);UiHistoryQuery query;query.search="old-image";model.setQuery(query);QTRY_COMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["receipt"].toObject()["directUrl"].toString(),QString("https://example.invalid/original.png"));QVERIFY(!model.linkAvailable(0));
  HistoryWindow window(fixture.session);window.resize(1000,700);window.show();if(window.width()>1000)for(auto* child:window.findChildren<QWidget*>())if(child->minimumSizeHint().width()>200)qInfo()<<child->metaObject()->className()<<child->objectName()<<child->minimumSizeHint();QVERIFY2(window.height()<=700,"History must preserve a 700px logical-pixel height");QVERIFY2(window.width()<=1000,qPrintable(QString("History controls expanded a 1000px window to %1px").arg(window.width())));auto* table=window.findChild<QListView*>("historyList");QVERIFY(table);QTRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);QTRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QTRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("2 GiB"));QCOMPARE(QJsonDocument::fromJson(window.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().toUtf8()).object()["bytes"].toInteger(),qint64(2147483649LL));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
};QTEST_MAIN(HistoryGui)
#include "test_history_view.moc"
