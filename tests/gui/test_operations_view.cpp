#include "ui/RecentOperations.h"
#include "fixtures/UiSessionFixture.h"
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QPaintEvent>
#include <QPainter>
#include <QStyleOptionViewItem>
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
 void narrowFileDetailsKeepSizeAndOutcomesOnSeparateVisibleLines(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload.kind="files";
  const QString name="a-long-synthetic-file-name-that-must-remain-complete-in-the-tooltip.txt";
  source.payload.files={fixture(d.path(),name,QByteArray(1572864,'x'))};QVERIFY(!source.payload.files.first().isEmpty());
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture session(settings,operations,history);RecentOperations view(session.session);view.resize(1024,700);view.show();
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);auto* rows=view.findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(rows->topLevelItemCount(),1);QTRY_COMPARE(rows->topLevelItem(0)->childCount(),1);
  QTRY_VERIFY(rows->topLevelItem(0)->child(0)->text(3).startsWith("Saved\n"));auto* child=rows->topLevelItem(0)->child(0);QVERIFY(child->text(4).startsWith("1.5 MiB\nRemote: Not requested"));QCOMPARE(child->toolTip(0),name);QVERIFY(child->toolTip(3).contains(source.copied.files.first()));QVERIFY(child->toolTip(4).contains("not_requested"));
  // Test the actual cell renderer at narrow widths and a larger native-family font.
  // Both lines must have visible ink; a tall empty row does not satisfy the contract.
  for(int growth:{0,6}){
   QFont font=rows->font();font.setPixelSize(rows->fontMetrics().height()+growth);for(int column:{3,4})child->setFont(column,font);rows->doItemsLayout();
   for(int column:{3,4}){
    const auto index=rows->model()->index(0,column,rows->model()->index(0,0));QStyleOptionViewItem option;option.initFrom(rows);option.widget=rows;option.font=font;option.fontMetrics=QFontMetrics(font);option.state=QStyle::State_Enabled|QStyle::State_Active;
    const auto height=rows->visualRect(index).height();QVERIFY2(height>=2*QFontMetrics(font).height()+12,"Actual file rows must grow with the chosen native font without clipping either line");
    option.rect=QRect(0,0,column==3?170:190,height);QImage image(option.rect.size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);QPainter painter(&image);rows->itemDelegateForColumn(column)->paint(&painter,option,index);painter.end();
    auto hasText=[&](int top,int bottom){for(int y=top;y<bottom;++y)for(int x=8;x<image.width()-8;++x){const auto color=image.pixelColor(x,y);if(color.alpha()>0&&color.red()>140&&color.green()>140&&color.blue()>140)return true;}return false;};
    QVERIFY2(hasText(0,height/2),"File size/local state must remain visible in the first line");QVERIFY2(hasText(height/2,height),"Remote outcome/local path must remain visible in the second line");
   }
  }
  rows->setCurrentItem(child);view.findChild<QPushButton*>("copyOperationResult")->click();QTRY_COMPARE(source.copies,2);QCOMPARE(source.reads,1);QVERIFY(source.copied.files.first().endsWith(name));
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
