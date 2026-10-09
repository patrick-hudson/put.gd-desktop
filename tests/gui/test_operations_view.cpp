#include "ui/RecentOperations.h"
#include "fixtures/UiSessionFixture.h"
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QPaintEvent>
using namespace putgd;using namespace putgd::test;
class CopyFailureSource final:public SourceAdapter {
public:quint64 revision=0;bool fail=true;int reads=0,copies=0;
 quint64 clipboardRevision()const override{return revision;}QJsonObject capabilities()const override{return {{"clipboard",true}};}
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++reads;reply({textPayload(),{}});}
 void cancelAcquisition()override{}
 void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;if(fail)reply({"clipboard_unavailable","Synthetic clipboard refusal",{}});else{++revision;reply({});}}
};
class ActivityPaintObserver final:public QObject {
public:int paints=0;QRegion region;
 bool eventFilter(QObject*,QEvent* event) override {if(event->type()==QEvent::Paint){++paints;region|=static_cast<QPaintEvent*>(event)->region();}return false;}
};
class OperationsGui:public QObject {
 Q_OBJECT

private slots:
 void recreatedActivityPreservesSelectionAndExpansionById(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);QJsonObject state;
  {RecentOperations view(fixture.session);auto* rows=view.findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(rows->topLevelItemCount(),1);rows->setCurrentItem(rows->topLevelItem(0));rows->topLevelItem(0)->setExpanded(false);state=view.saveViewState();}
  RecentOperations restored(fixture.session);QVERIFY(restored.restoreViewState(state));auto* rows=restored.findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(rows->topLevelItemCount(),1);QVERIFY(rows->currentItem());QCOMPARE(rows->currentItem()->data(0,Qt::UserRole).toString(),state["selectedId"].toString());QVERIFY(!rows->topLevelItem(0)->isExpanded());QCOMPARE(source.reads,1);QCOMPARE(source.copies,1);
 }
 void uploadedResultCanRetryCopyWithoutAnotherUpload() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto s){return registry.validate(id,s);});QVERIFY(settings.save(configuration(d.path()),{}));CopyFailureSource source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);RecentOperations view(fixture.session);QString id;operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);id=data["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());auto* rows=view.findChild<QTreeWidget*>("recentOperations");QTRY_VERIFY(rows->topLevelItemCount()>0);QTRY_COMPARE(rows->topLevelItem(0)->text(3),QString("failed"));QCOMPARE(rows->topLevelItem(0)->child(0)->text(2),QString("uploaded"));QVERIFY(rows->topLevelItem(0)->child(0)->text(1).startsWith("https://example.invalid/"));QCOMPARE(rows->topLevelItem(0)->data(0,Qt::UserRole).toString(),id);QCOMPARE(rows->topLevelItem(0)->toolTip(1),id);QVERIFY(rows->topLevelItem(0)->text(1).size()<id.size());
  // Child selection must still copy its parent operation's result using the full ID.
  rows->setCurrentItem(rows->topLevelItem(0)->child(0));source.fail=false;view.findChild<QPushButton*>("copyOperationResult")->click();QTRY_COMPARE(rows->topLevelItem(0)->text(3),QString("copied"));QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,2);
 }
 void localOperationShowsIndependentItemAndCopyStates() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload=textPayload();source.payload.bytes=QByteArray(1572864,'x');OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);RecentOperations view(fixture.session);ActivityPaintObserver paint;view.installEventFilter(&paint);view.resize(1024,700);view.show();QTRY_VERIFY(view.findChild<QLabel*>("activityEmpty")->isVisible());QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);id=data["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());auto* rows=view.findChild<QTreeWidget*>("recentOperations");QVERIFY(rows);QTRY_VERIFY(rows->topLevelItemCount()>0);QTRY_COMPARE(rows->topLevelItem(0)->text(2),QString("completed"));QVERIFY(rows->topLevelItem(0)->childCount()>0);QTRY_COMPARE(source.copied.kind,QString("text"));QVERIFY(rows->topLevelItem(0)->child(0)->text(3).contains("saved",Qt::CaseInsensitive));QVERIFY(rows->topLevelItem(0)->child(0)->text(4).contains("1.5 MiB"));
  QVERIFY2(view.width()<=1024&&view.height()<=700,"Activity must fit a compact desktop window");QVERIFY(!view.findChild<QLabel*>("activityEmpty")->isVisible());
  QVERIFY(rows->header()->sectionSize(0)>rows->header()->sectionSize(1));QCOMPARE(rows->topLevelItem(0)->toolTip(1),id);QCOMPARE(rows->topLevelItem(0)->data(0,Qt::UserRole).toString(),id);QVERIFY(rows->topLevelItem(0)->child(0)->text(4).contains("Remote: Not requested"));QVERIFY(!rows->topLevelItem(0)->child(0)->text(4).contains("not_requested"));QVERIFY(rows->topLevelItem(0)->child(0)->toolTip(4).contains("not_requested"));
  for(const auto& name:QStringList{"copyOperationResult","retryOperation","retryLocalSave","cancelOperation"}){
   auto* button=view.findChild<QPushButton*>(name);QVERIFY(button);QVERIFY(button->isVisible());
   QVERIFY(view.rect().contains(QRect(button->mapTo(&view,QPoint{}),button->size())));
  }
  rows->setCurrentItem(rows->topLevelItem(0));const auto selected=rows->currentItem()->data(0,Qt::UserRole).toString();
  for(int cycle=0;cycle<3;++cycle){
   view.hide();QTRY_VERIFY_WITH_TIMEOUT(!view.testAttribute(Qt::WA_WState_Created),500);QVERIFY(!view.backingStore());QCOMPARE(view.findChild<QTreeWidget*>("recentOperations"),rows);QCOMPARE(rows->currentItem()->data(0,Qt::UserRole).toString(),selected);
   paint.paints=0;paint.region=QRegion{};view.show();QTRY_VERIFY(view.testAttribute(Qt::WA_WState_Created));QTRY_VERIFY(paint.paints>0);QVERIFY(paint.region.boundingRect().contains(view.rect()));QCOMPARE(rows->currentItem()->data(0,Qt::UserRole).toString(),selected);
  }
  QCOMPARE(source.reads,1);QCOMPARE(source.copies,1);
 }
};QTEST_MAIN(OperationsGui)
#include "test_operations_view.moc"
