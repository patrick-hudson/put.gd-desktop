#include "ui/RecentOperations.h"
#include "fixtures/UiSessionFixture.h"
#include <QTreeWidget>
#include <QScopeGuard>
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
 void acquisitionAndUploadRefreshBeforeCompletion(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();source.defer=true;
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);RecentOperations view(fixture.session);
  auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.acquisitions.size(),1);
  auto* rows=view.findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(rows->topLevelItemCount(),1);QTRY_COMPARE(rows->topLevelItem(0)->text(2),QString("Acquiring"));
  source.acquisitions.takeFirst()({source.payload,{}});QTRY_COMPARE(provider->completions.size(),1);QTRY_COMPARE(rows->topLevelItem(0)->text(2),QString("Uploading"));
  auto report=provider->requests.first().progress;QVERIFY(bool(report));report({"uploading",1048576,2097152});QTRY_VERIFY(rows->topLevelItem(0)->child(0)->text(2).contains("50%"));QVERIFY(rows->topLevelItem(0)->child(0)->text(4).contains("1 MiB transferred"));
  report({"uploading",2097152,2097152});QTRY_VERIFY(rows->topLevelItem(0)->child(0)->text(2).contains("100%"));QCOMPARE(source.copies,0);QVERIFY(rows->topLevelItem(0)->text(2)=="Uploading");
  report({"uploading",524288,{}});QTRY_VERIFY(rows->topLevelItem(0)->child(0)->text(2).contains("0.5 MiB"));QVERIFY(!rows->topLevelItem(0)->child(0)->text(2).contains('%'));
  provider->failUpload=true;provider->completions.takeFirst()();QTRY_COMPARE(rows->topLevelItem(0)->text(2),QString("partial"));QVERIFY(rows->topLevelItem(0)->child(0)->text(4).contains("Synthetic failure"));QCOMPARE(rows->topLevelItem(0)->child(0)->text(2),QString("failed"));
 }

 void sustainedProgressRendersWhileRecentRepliesAreDelayed(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);RecentOperations view(fixture.session);
  auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);
  auto* rows=view.findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(rows->topLevelItemCount(),1);QTRY_COMPARE(rows->topLevelItem(0)->childCount(),1);QTRY_COMPARE(rows->topLevelItem(0)->text(2),QString("Uploading"));
  rows->setCurrentItem(rows->topLevelItem(0)->child(0));const auto selected=rows->currentItem()->data(0,Qt::UserRole).toString();rows->topLevelItem(0)->setExpanded(false);
  fixture.client.replyDelayMs["operation.recent"]=600;auto report=provider->requests.first().progress;QVERIFY(bool(report));int reports=0;QTimer progress;progress.setInterval(50);connect(&progress,&QTimer::timeout,this,[&]{report({"uploading",1048576+ ++reports,{}});});progress.start();
  auto rendered=[&]{return rows->topLevelItemCount()==1&&rows->topLevelItem(0)->childCount()==1&&rows->topLevelItem(0)->child(0)->text(2).contains("1 MiB");};
  QTRY_VERIFY_WITH_TIMEOUT(rendered(),2500);QVERIFY(progress.isActive());QVERIFY(reports>10);QCOMPARE(provider->completions.size(),1);QCOMPARE(source.copies,0);QCOMPARE(rows->topLevelItem(0)->data(0,Qt::UserRole).toString(),id);QVERIFY(rows->currentItem());QCOMPARE(rows->currentItem()->data(0,Qt::UserRole).toString(),selected);QVERIFY(!rows->topLevelItem(0)->isExpanded());
 }

 void queuedRetryIsDurableAndVisibleBeforeAWorkerBecomesAvailable(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->failUpload=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);RecentOperations view(fixture.session);HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString retryId;
  operations.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);retryId=result["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(journal.operation(retryId).state,QString("partial"));const auto original=journal.operation(retryId);
  provider->failUpload=false;operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->completions.size(),1);
  bool accepted=false;operations.retry(retryId,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});QTRY_VERIFY(accepted);
  QCOMPARE(journal.operation(retryId).state,QString("stored"));QCOMPARE(journal.operation(retryId).phase,QString("queued"));QCOMPARE(journal.operation(retryId).destination.toJson(),original.destination.toJson());QCOMPARE(journal.operation(retryId).items.first().publicationId,original.items.first().publicationId);QCOMPARE(provider->completions.size(),1);
  QJsonObject inspected;bool replied=false;operations.inspect(retryId,[&](Error e,QJsonObject value){QVERIFY(!e);inspected=value;replied=true;});QTRY_VERIFY(replied);QCOMPARE(inspected["phase"].toString(),QString("queued"));
  auto* rows=view.findChild<QTreeWidget*>("recentOperations");auto phase=[&]{for(int i=0;i<rows->topLevelItemCount();++i)if(rows->topLevelItem(i)->data(0,Qt::UserRole).toString()==retryId)return rows->topLevelItem(i)->text(2);return QString{};};QTRY_COMPARE(phase(),QString("Queued"));
  bool cancelled=false;operations.cancel(retryId,[&](Error e,QJsonObject){QVERIFY(!e);cancelled=true;});QTRY_VERIFY(cancelled);QTRY_COMPARE(journal.operation(retryId).state,QString("cancelled"));QCOMPARE(journal.operation(retryId).phase,QString("cancelled"));QCOMPARE(provider->completions.size(),1);
 }

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
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);
  // Clipboard delivery precedes its final journal commit. Wait for durable
  // completion before testing an explicit copy, then resolve the current row.
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  auto* rows=view.findChild<QTreeWidget*>("recentOperations");QTRY_COMPARE(rows->topLevelItemCount(),1);QTRY_COMPARE(rows->topLevelItem(0)->childCount(),1);
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
