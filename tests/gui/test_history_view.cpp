#include "ui/HistoryModel.h"
#include "ui/HistoryWindow.h"
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
using namespace putgd;using namespace putgd::test;
class HistoryPaintObserver final:public QObject {
public:int paints=0;QRegion region;
 bool eventFilter(QObject*,QEvent* event) override {if(event->type()==QEvent::Paint){++paints;region|=static_cast<QPaintEvent*>(event)->region();}return false;}
};
class HistoryGui:public QObject {
 Q_OBJECT

private slots:
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
  bool cleared=false;history.clear([&](Error e,QJsonObject){QVERIFY(!e);cleared=true;});QTRY_VERIFY(cleared);restored.refresh();QTRY_COMPARE(table->model()->rowCount(),0);QVERIFY(restored.saveViewState()["selectedIds"].toArray().isEmpty());QCOMPARE(source.reads,0);
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
