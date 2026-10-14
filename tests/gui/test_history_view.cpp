#include "ui/HistoryModel.h"
#include "ui/HistoryWindow.h"
#include "ui/RecentOperations.h"
#include <QTreeWidget>
#include "fixtures/UiSessionFixture.h"
#include <QTableView>
#include <QHeaderView>
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
class HistoryGui:public QObject {
 Q_OBJECT
 static bool sql(const QString& path,const QString& statement){
  const auto connection=QUuid::createUuid().toString();bool ok=false;
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);if(db.open()){QSqlQuery query(db);ok=query.exec(statement);}db.close();}
  QSqlDatabase::removeDatabase(connection);return ok;
 }

private slots:
 void columnBoundariesFollowPointer_data(){
  QTest::addColumn<bool>("activity");QTest::addColumn<int>("column");QTest::addColumn<int>("delta");
  for(bool activity:{false,true})for(int column:activity?QList<int>{0,1,2,3,4}:QList<int>{0,1,4,5,6})for(int delta:{40,-40})
   QTest::newRow(qPrintable(QString("%1-column-%2-%3").arg(activity?"activity":"history").arg(column).arg(delta)))<<activity<<column<<delta;
 }
 void columnBoundariesFollowPointer(){
  QFETCH(bool,activity);QFETCH(int,column);QFETCH(int,delta);
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  std::unique_ptr<QWidget> window;QHeaderView* header=nullptr;
  if(activity){auto* view=new RecentOperations(fixture.session);window.reset(view);auto* tree=view->findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(tree->topLevelItemCount(),1);header=tree->header();}
  else{auto* view=new HistoryWindow(fixture.session);window.reset(view);auto* table=view->findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);header=table->horizontalHeader();}
  if(activity){QTreeWidget defaults;QCOMPARE(header->sectionsMovable(),defaults.header()->sectionsMovable());QCOMPARE(header->sectionsClickable(),defaults.header()->sectionsClickable());}
  window->resize(1800,700);window->show();QTRY_VERIFY(header->isVisible());QTest::qWait(20);
  const auto left=header->sectionViewportPosition(column);const auto width=header->sectionSize(column);
  QVERIFY(width-40>=header->minimumSectionSize());
  const QPoint start(left+width-1,header->height()/2);
  auto drag=[&](QPoint from,int distance){
   QTest::mousePress(header->viewport(),Qt::LeftButton,Qt::NoModifier,from);
   QTest::mouseMove(header->viewport(),from+QPoint(distance,0));QTest::qWait(20);
   QTest::mouseRelease(header->viewport(),Qt::LeftButton,Qt::NoModifier,from+QPoint(distance,0));
  };
  QTest::mousePress(header->viewport(),Qt::LeftButton,Qt::NoModifier,start);
  for(int distance:{delta/2,delta}){
   QTest::mouseMove(header->viewport(),start+QPoint(distance,0));QTest::qWait(20);
   QCOMPARE(header->sectionViewportPosition(column),left);
   QCOMPARE(header->sectionSize(column),width+distance);
  }
  QTest::mouseRelease(header->viewport(),Qt::LeftButton,Qt::NoModifier,start+QPoint(delta,0));
  QCOMPARE(header->sectionViewportPosition(column),left);
  QCOMPARE(header->sectionSize(column),width+delta);
  QCOMPARE(header->sectionViewportPosition(column)+header->sectionSize(column),start.x()+1+delta);
  // Dragging farther left stops at the native minimum instead of moving neighbors.
  const auto minimum=header->minimumSectionSize();
  drag(QPoint(left+header->sectionSize(column)-1,start.y()),minimum-header->sectionSize(column)-20);
  QCOMPARE(header->sectionSize(column),minimum);QCOMPARE(header->sectionViewportPosition(column),left);
 }
 void externalDeletionAndClearRefreshOpenViews() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);const QSignalBlocker settingsSignals(settings),operationSignals(operations);HistoryWindow window(fixture.session);RecentOperations activity(fixture.session);window.show();activity.show();
  auto* table=window.findChild<QTableView*>("historyTable");auto* model=qobject_cast<HistoryModel*>(table->model());auto* tree=activity.findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(model->rowCount(),1);QTRY_COMPARE(tree->topLevelItemCount(),1);QTRY_COMPARE(tree->topLevelItem(0)->childCount(),1);table->selectRow(0);QTest::qWait(100);
  DispatchSessionClient external(fixture.server);const auto id=model->record(0)["id"].toString();QJsonObject preview;external.request("delete.prepare",{{"ids",QJsonArray{id}}},this,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());
  bool accepted=false;external.request("delete.confirm",{{"ids",QJsonArray{id}},{"acknowledgement",preview["acknowledgement"]}},this,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});QTRY_VERIFY(accepted);QTRY_COMPARE(provider->deletes.load(),1);
  QTRY_COMPARE(model->record(0)["remoteStatus"].toString(),QString("removed"));QTRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QTRY_VERIFY(tree->topLevelItem(0)->child(0)->text(4).contains("Remote: Removed"));
  bool cleared=false;external.request("history.clear",{},this,[&](Error e,QJsonObject){QVERIFY(!e);cleared=true;});QTRY_VERIFY(cleared);QTRY_COMPARE(model->rowCount(),0);QVERIFY(window.saveViewState()["selectedIds"].toArray().isEmpty());
 }
 void compactHistoryKeepsOutcomesVisibleAndOriginalDetailsRoutedById(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1024,700);window.show();
  auto* table=window.findChild<QTableView*>("historyTable");auto* inspector=window.findChild<QScrollArea*>("historyInspectorScroll");QVERIFY(table);QVERIFY(inspector);QVERIFY(!inspector->isVisible());auto* empty=window.findChild<QLabel*>("historyEmpty");QVERIFY(empty);QTRY_VERIFY(empty->isVisible());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-compact-history";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();op.destination.profileId="original-synthetic-destination";
   for(int i=0;i<2;++i){OperationItem item;item.artifact.id=QString("compact-history-item-%1").arg(i);item.artifact.name=QString("long-synthetic-filename-with-receipt-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";op.items.append(item);}QVERIFY(!ledger.saveOperation(op));}
  window.refresh();QTRY_COMPARE(table->model()->rowCount(),2);QTRY_VERIFY(table->isVisible());QVERIFY(!window.findChild<QLabel*>("historyEmpty")->isVisible());QCOMPARE(table->model()->columnCount(),7);QCOMPARE(table->model()->headerData(2,Qt::Horizontal).toString(),QString("Content type"));QCOMPARE(table->model()->headerData(3,Qt::Horizontal).toString(),QString("Original destination"));
  auto* model=qobject_cast<HistoryModel*>(table->model());QVERIFY(model);const auto selectedId=model->record(1)["id"].toString();table->selectRow(1);auto* details=window.findChild<QPlainTextEdit*>("historyDetails");auto* summary=window.findChild<QPlainTextEdit*>("historySummary");QTRY_COMPARE(QJsonDocument::fromJson(details->toPlainText().toUtf8()).object()["id"].toString(),selectedId);QVERIFY(summary->toPlainText().contains("text/plain"));QVERIFY(summary->toPlainText().contains("original-synthetic-destination"));QVERIFY(inspector->isVisible());
  for(const auto size:QList<QSize>{QSize(1024,700),QSize(1280,700),QSize(1024,700)}){
   window.resize(size);QTest::qWait(20);QVERIFY(window.width()<=size.width());QVERIFY(window.height()<=size.height());
   for(int column:{0,1,4,5,6}){QVERIFY(!table->isColumnHidden(column));const auto right=table->horizontalHeader()->sectionViewportPosition(column)+table->horizontalHeader()->sectionSize(column);QVERIFY2(right<=table->viewport()->width(),"Name, date and independent outcomes must fit without horizontal scrolling");}
   QCOMPARE(table->selectionModel()->selectedRows().size(),1);QCOMPARE(model->record(table->selectionModel()->selectedRows().first().row())["id"].toString(),selectedId);QVERIFY(window.rect().contains(QRect(inspector->mapTo(&window,QPoint{}),inspector->size())));
  }
  table->clearSelection();QVERIFY(!inspector->isVisible());QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void recreatedHistoryRestoresRawFiltersSelectionAndClearsVanishedIds(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="state-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="state-history-id";item.artifact.name="state.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);QJsonObject state;
  {HistoryWindow window(fixture.session);auto* table=window.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);window.findChild<QLineEdit*>("historySearch")->setText("raw unapplied search");window.findChild<QLineEdit*>("historyFilterfrom")->setText("invalid raw date");state=window.saveViewState();}
  QVERIFY(!validateUiState({{"schemaVersion",1},{"history",state}}));QVERIFY(!QJsonDocument(state).toJson().contains("state.txt"));HistoryWindow restored(fixture.session);QVERIFY(restored.restoreViewState(state));auto* table=restored.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->selectionModel()->selectedRows().size(),1);QCOMPARE(restored.findChild<QLineEdit*>("historySearch")->text(),QString("raw unapplied search"));QCOMPARE(restored.findChild<QLineEdit*>("historyFilterfrom")->text(),QString("invalid raw date"));
  bool cleared=false;history.clear([&](Error e,QJsonObject){QVERIFY(!e);cleared=true;});QTRY_VERIFY(cleared);QTRY_COMPARE(table->model()->rowCount(),0);QVERIFY(restored.saveViewState()["selectedIds"].toArray().isEmpty());QCOMPARE(source.reads,0);
 }
 void restoringHistoryKeepsActiveDeletionCancellation() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  std::atomic_bool entered{false},released{false};provider->checkpointObserver=[&](const DeleteRequest&){entered.store(true);while(!released.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));};
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);auto original=std::make_unique<HistoryWindow>(fixture.session);auto& window=*original;window.show();auto* table=window.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);
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
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->storage.uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  auto drain=qScopeGuard([&]{provider->released.store(true);operations.shutdown();});
  auto original=std::make_unique<HistoryWindow>(fixture.session);original->show();auto* table=original->findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);auto* create=original->findChild<QPushButton*>("shareHistoryLink");QTRY_VERIFY(create->isVisible()&&create->isEnabled());
  QJsonObject acceptedState;connect(original.get(),&HistoryWindow::viewStateChanged,this,[&]{const auto state=original->saveViewState();if(!state["sharingId"].toString().isEmpty())acceptedState=state;});
  create->click();QTRY_VERIFY(provider->entered.load());QTRY_VERIFY(!acceptedState["sharingId"].toString().isEmpty());QVERIFY(!validateUiState({{"schemaVersion",1},{"history",acceptedState}}));
  auto legacyState=acceptedState;legacyState.remove("sharingId");QVERIFY(!validateUiState({{"schemaVersion",1},{"history",legacyState}}));auto invalidState=acceptedState;invalidState["sharingId"]=QJsonObject{{"unsupported","value"}};QCOMPARE(validateUiState({{"schemaVersion",1},{"history",invalidState}}).code,QString("ui_state_invalid"));
  original.reset();HistoryWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(acceptedState));reopened.show();QCOMPARE(reopened.saveViewState()["sharingId"].toString(),acceptedState["sharingId"].toString());QTRY_COMPARE(reopened.findChild<QTableView*>("historyTable")->selectionModel()->selectedRows().size(),1);QVERIFY(!reopened.findChild<QPushButton*>("shareHistoryLink")->isEnabled());
  provider->released.store(true);QLabel* status=nullptr;for(auto* label:reopened.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);
  QTRY_VERIFY(status->text().contains("Link: "+outcome));if(outcome=="failed")QVERIFY(status->text().contains("Synthetic sharing failure"));
  QTRY_VERIFY(reopened.findChild<QPlainTextEdit*>("historySummary")->toPlainText().contains("Sharing: "+outcome));QVERIFY(status->text().contains("Link: "+outcome));
  QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,0);
 }
 void stoppedSharingJournalCanBeInspectedAfterStorageRecovers(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<PendingSharingProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->storage.uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);auto drain=qScopeGuard([&]{provider->released.store(true);operations.shutdown();});HistoryWindow window(fixture.session);window.show();
  auto* table=window.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);auto* create=window.findChild<QPushButton*>("shareHistoryLink");auto* inspect=window.findChild<QPushButton*>("inspectHistoryLink");QTRY_VERIFY(create->isVisible()&&create->isEnabled());
  create->click();QTRY_VERIFY(provider->entered.load());QTRY_VERIFY(!window.saveViewState()["sharingId"].toString().isEmpty());const auto stoppedId=window.saveViewState()["sharingId"].toString();
  QVERIFY(sql(operations.databasePath(),"CREATE TRIGGER fail_share BEFORE UPDATE ON sharing_operations BEGIN SELECT RAISE(FAIL,'synthetic journal failure'); END"));provider->released.store(true);
  QLabel* status=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);QTRY_VERIFY(status->text().contains("Link: unknown"));QVERIFY(status->text().contains("Persistent journal"));
  HistoryStore store(operations.databasePath());QCOMPARE(store.sharing(stoppedId)["state"].toString(),QString("running"));QTRY_VERIFY(inspect->isVisible()&&inspect->isEnabled());QVERIFY(!create->isEnabled());QVERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->inspections.load(),0);
  QVERIFY(sql(operations.databasePath(),"DROP TRIGGER fail_share"));inspect->click();QTRY_VERIFY(window.saveViewState()["sharingId"].toString()!=stoppedId);QTRY_COMPARE(provider->inspections.load(),1);QTRY_VERIFY(status->text().contains("Link: shared"));QTRY_VERIFY(window.findChild<QPlainTextEdit*>("historySummary")->toPlainText().contains("Sharing: shared"));
  QCOMPARE(store.sharing(stoppedId)["state"].toString(),QString("interrupted"));QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,0);
 }
 void imageInspectorAndExpandedControlsFitCompactWindow() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto imagePath=d.filePath("synthetic-preview.png");QImage image(320,180,QImage::Format_RGB32);image.fill(QColor("#a9f1cb"));QVERIFY(image.save(imagePath));
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-image-inspector";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-preview-item";item.artifact.name="synthetic-preview.png";item.artifact.mime="image/png";item.artifact.category="images";item.artifact.previewPath=imagePath;item.artifact.bytes=1572864;item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);HistoryPaintObserver paint;window.installEventFilter(&paint);window.resize(1000,700);window.show();auto* table=window.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);auto* preview=window.findChild<QLabel*>("historyPreview");QVERIFY(preview);QTRY_VERIFY(!preview->pixmap().isNull());QVERIFY(preview->isVisible());QVERIFY(preview->height()>0);QTRY_VERIFY(window.findChild<QPlainTextEdit*>("historySummary")->toPlainText().contains("1.5 MiB"));
  QToolButton *filters=nullptr,*manage=nullptr;for(auto* button:window.findChildren<QToolButton*>()){if(button->text()=="Filters")filters=button;if(button->text()=="Manage")manage=button;}QVERIFY(filters);QVERIFY(manage);filters->setChecked(true);manage->setChecked(true);QTest::qWait(20);
  QVERIFY2(window.width()<=1000&&window.height()<=700,qPrintable(QString("Expanded history grew to %1×%2").arg(window.width()).arg(window.height())));
  for(const auto& name:QStringList{"copyHistoryLink","deleteHistoryRemote"}){auto* button=window.findChild<QPushButton*>(name);QVERIFY(button->isVisible());QVERIFY(window.rect().contains(button->mapTo(&window,button->rect().bottomRight())));}
  QVERIFY(window.rect().contains(manage->mapTo(&window,manage->rect().bottomRight())));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
  for(int cycle=0;cycle<3;++cycle){
   window.hide();QVERIFY2(preview->pixmap().isNull(),"Hidden history must release its decoded thumbnail");QTRY_VERIFY_WITH_TIMEOUT(!window.testAttribute(Qt::WA_WState_Created),500);QVERIFY(!window.backingStore());
   QCOMPARE(window.findChild<QTableView*>("historyTable"),table);QVERIFY(filters->isChecked());QVERIFY(manage->isChecked());QCOMPARE(table->selectionModel()->selectedRows().size(),1);
   paint.paints=0;paint.region=QRegion{};window.show();QTRY_VERIFY(window.testAttribute(Qt::WA_WState_Created));QTRY_VERIFY(paint.paints>0);QVERIFY(paint.region.boundingRect().contains(window.rect()));QTRY_VERIFY(!preview->pixmap().isNull());
  }
  table->clearSelection();QVERIFY(preview->pixmap().isNull());

 }

 void openingHistoryDuringInitializationRefreshesWhenReady() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));QVERIFY(QDir().mkpath(d.filePath("data")));
  {HistoryStore ledger(d.filePath("data/history.sqlite"));Operation op;op.id="synthetic-startup";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="startup-item";item.artifact.name="startup.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QVERIFY(!operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);auto* table=window.findChild<QTableView*>("historyTable");QLabel* status=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);QVERIFY(status->text().contains("waiting for the resident journal"));QTRY_VERIFY(operations.ready());QTRY_COMPARE(table->model()->rowCount(),1);QTRY_VERIFY(!status->text().contains("waiting for the resident journal"));QVERIFY(status->text().contains("Select an entry"));QCOMPARE(source.reads,0);
 }
 void tenThousandEntryLedgerPagesAndFiltersOffline() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-large-ledger";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();for(int i=0;i<10000;++i){OperationItem item;item.artifact.id=QString("ledger-id-%1").arg(i);item.artifact.name=QString("ledger-name-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";op.items.append(item);}QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);int ticks=0;QTimer pulse;pulse.setInterval(1);connect(&pulse,&QTimer::timeout,this,[&]{++ticks;});pulse.start();UiHistoryQuery page;page.offset=9950;model.setQuery(page);QTRY_COMPARE(model.total(),qint64(10000));QCOMPARE(model.rowCount(),50);QVERIFY(ticks>0);page.offset=0;page.search="ledger-name-9999.txt";model.setQuery(page);QTRY_COMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["name"].toString(),QString("ledger-name-9999.txt"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void cancellingExactDeletionConfirmationSendsNoDelete() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto s){return registry.validate(id,s);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);operationId=data["operationId"].toString();});QTRY_COMPARE(provider->uploads.load(),1);QTRY_VERIFY(!operationId.isEmpty());QJsonObject completed;operations.inspect(operationId,[&](Error,QJsonObject data){completed=data;});QTRY_VERIFY(!completed.isEmpty());
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);auto* table=window.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);bool confirmed=false;QTimer dismiss;dismiss.setInterval(10);connect(&dismiss,&QTimer::timeout,this,[&]{for(auto* widget:QApplication::topLevelWidgets()){auto* box=qobject_cast<QMessageBox*>(widget);if(box&&box->windowTitle()=="Confirm exact remote deletion"){confirmed=box->text().contains("Original destination / exact target")&&box->text().contains("Remove only the recorded opaque file revision");box->done(QMessageBox::Cancel);}}});dismiss.start();window.findChild<QPushButton*>("deleteHistoryRemote")->click();QTRY_VERIFY(confirmed);QCOMPARE(provider->deletes.load(),0);QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);
 }
 void offlineSearchUsesOriginalReceiptsAndRemovedLinksAreDisabled() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="old-item";item.artifact.name="old-image.png";item.artifact.bytes=2147483649LL;item.artifact.mime="image/png";item.artifact.category="images";item.uploadStatus="uploaded";item.remoteStatus="removed";item.receipt.directUrl="https://example.invalid/original.png";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);UiHistoryQuery query;query.search="old-image";model.setQuery(query);QTRY_COMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["receipt"].toObject()["directUrl"].toString(),QString("https://example.invalid/original.png"));QVERIFY(!model.linkAvailable(0));
  HistoryWindow window(fixture.session);window.resize(1000,700);window.show();if(window.width()>1000)for(auto* child:window.findChildren<QWidget*>())if(child->minimumSizeHint().width()>200)qInfo()<<child->metaObject()->className()<<child->objectName()<<child->minimumSizeHint();QVERIFY2(window.height()<=700,"History must preserve a 700px logical-pixel height");QVERIFY2(window.width()<=1000,qPrintable(QString("History controls expanded a 1000px window to %1px").arg(window.width())));auto* table=window.findChild<QTableView*>("historyTable");QVERIFY(table);QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);QTRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QTRY_VERIFY(window.findChild<QPlainTextEdit*>("historySummary")->toPlainText().contains("2 GiB"));QCOMPARE(QJsonDocument::fromJson(window.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().toUtf8()).object()["bytes"].toInteger(),qint64(2147483649LL));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
};QTEST_MAIN(HistoryGui)
#include "test_history_view.moc"
