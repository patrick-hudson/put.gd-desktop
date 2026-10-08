#include "ui/HistoryModel.h"
#include "ui/HistoryWindow.h"
#include "fixtures/U2Fixtures.h"
#include <QTableView>
#include <QPushButton>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <QLabel>
#include <QToolButton>
#include <QImage>
#include <QPlainTextEdit>
using namespace putgd;using namespace putgd::test;
class HistoryGui:public QObject {
 Q_OBJECT

private slots:
 void imageInspectorAndExpandedControlsFitCompactWindow() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto imagePath=d.filePath("synthetic-preview.png");QImage image(320,180,QImage::Format_RGB32);image.fill(QColor("#a9f1cb"));QVERIFY(image.save(imagePath));
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-image-inspector";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-preview-item";item.artifact.name="synthetic-preview.png";item.artifact.mime="image/png";item.artifact.category="images";item.artifact.previewPath=imagePath;item.artifact.bytes=1572864;item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);HistoryWindow window(history,operations);window.resize(1000,700);window.show();auto* table=window.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);auto* preview=window.findChild<QLabel*>("historyPreview");QVERIFY(preview);QTRY_VERIFY(!preview->pixmap().isNull());QVERIFY(preview->isVisible());QVERIFY(preview->height()>0);QTRY_VERIFY(window.findChild<QPlainTextEdit*>("historySummary")->toPlainText().contains("1.5 MiB"));
  QToolButton *filters=nullptr,*manage=nullptr;for(auto* button:window.findChildren<QToolButton*>()){if(button->text()=="Filters")filters=button;if(button->text()=="Manage")manage=button;}QVERIFY(filters);QVERIFY(manage);filters->setChecked(true);manage->setChecked(true);QTest::qWait(20);
  QVERIFY2(window.width()<=1000&&window.height()<=700,qPrintable(QString("Expanded history grew to %1×%2").arg(window.width()).arg(window.height())));
  for(const auto& name:QStringList{"copyHistoryLink","deleteHistoryRemote"}){auto* button=window.findChild<QPushButton*>(name);QVERIFY(button->isVisible());QVERIFY(window.rect().contains(button->mapTo(&window,button->rect().bottomRight())));}
  QVERIFY(window.rect().contains(manage->mapTo(&window,manage->rect().bottomRight())));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }

 void openingHistoryDuringInitializationRefreshesWhenReady() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));QVERIFY(QDir().mkpath(d.filePath("data")));
  {HistoryStore ledger(d.filePath("data/history.sqlite"));Operation op;op.id="synthetic-startup";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="startup-item";item.artifact.name="startup.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QVERIFY(!operations.ready());HistoryService history(operations);HistoryWindow window(history,operations);auto* table=window.findChild<QTableView*>("historyTable");QLabel* status=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);QVERIFY(status->text().contains("waiting for the resident journal"));QTRY_VERIFY(operations.ready());QTRY_COMPARE(table->model()->rowCount(),1);QTRY_VERIFY(!status->text().contains("waiting for the resident journal"));QVERIFY(status->text().contains("Select an entry"));QCOMPARE(source.reads,0);
 }
 void tenThousandEntryLedgerPagesAndFiltersOffline() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-large-ledger";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();for(int i=0;i<10000;++i){OperationItem item;item.artifact.id=QString("ledger-id-%1").arg(i);item.artifact.name=QString("ledger-name-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";op.items.append(item);}QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);HistoryModel model(history);int ticks=0;QTimer pulse;pulse.setInterval(1);connect(&pulse,&QTimer::timeout,this,[&]{++ticks;});pulse.start();HistoryQuery page;page.offset=9950;model.setQuery(page);QTRY_COMPARE(model.total(),qint64(10000));QCOMPARE(model.rowCount(),50);QVERIFY(ticks>0);page.offset=0;page.search="ledger-name-9999.txt";model.setQuery(page);QTRY_COMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["name"].toString(),QString("ledger-name-9999.txt"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void cancellingExactDeletionConfirmationSendsNoDelete() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto s){return registry.validate(id,s);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);operationId=data["operationId"].toString();});QTRY_COMPARE(provider->uploads.load(),1);QTRY_VERIFY(!operationId.isEmpty());QJsonObject completed;operations.inspect(operationId,[&](Error,QJsonObject data){completed=data;});QTRY_VERIFY(!completed.isEmpty());
  HistoryService history(operations);HistoryWindow window(history,operations);auto* table=window.findChild<QTableView*>("historyTable");QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);bool confirmed=false;QTimer dismiss;dismiss.setInterval(10);connect(&dismiss,&QTimer::timeout,this,[&]{for(auto* widget:QApplication::topLevelWidgets()){auto* box=qobject_cast<QMessageBox*>(widget);if(box&&box->windowTitle()=="Confirm exact remote deletion"){confirmed=box->text().contains("Original destination / exact target")&&box->text().contains("Remove only the recorded opaque file revision");box->done(QMessageBox::Cancel);}}});dismiss.start();window.findChild<QPushButton*>("deleteHistoryRemote")->click();QTRY_VERIFY(confirmed);QCOMPARE(provider->deletes.load(),0);QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);
 }
 void offlineSearchUsesOriginalReceiptsAndRemovedLinksAreDisabled() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="old-item";item.artifact.name="old-image.png";item.artifact.bytes=2147483649LL;item.artifact.mime="image/png";item.artifact.category="images";item.uploadStatus="uploaded";item.remoteStatus="removed";item.receipt.directUrl="https://example.invalid/original.png";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);HistoryModel model(history);HistoryQuery query;query.search="old-image";model.setQuery(query);QTRY_COMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["receipt"].toObject()["directUrl"].toString(),QString("https://example.invalid/original.png"));QVERIFY(!model.linkAvailable(0));
  HistoryWindow window(history,operations);window.resize(1000,700);window.show();if(window.width()>1000)for(auto* child:window.findChildren<QWidget*>())if(child->minimumSizeHint().width()>200)qInfo()<<child->metaObject()->className()<<child->objectName()<<child->minimumSizeHint();QVERIFY2(window.height()<=700,"History must preserve a 700px logical-pixel height");QVERIFY2(window.width()<=1000,qPrintable(QString("History controls expanded a 1000px window to %1px").arg(window.width())));auto* table=window.findChild<QTableView*>("historyTable");QVERIFY(table);QTRY_COMPARE(table->model()->rowCount(),1);table->selectRow(0);QTRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QTRY_VERIFY(window.findChild<QPlainTextEdit*>("historySummary")->toPlainText().contains("2 GiB"));QCOMPARE(QJsonDocument::fromJson(window.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().toUtf8()).object()["bytes"].toInteger(),qint64(2147483649LL));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
};QTEST_MAIN(HistoryGui)
#include "test_history_view.moc"
