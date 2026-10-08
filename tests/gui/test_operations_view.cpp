#include "ui/RecentOperations.h"
#include "fixtures/U2Fixtures.h"
#include <QTreeWidget>
#include <QPushButton>
using namespace putgd;using namespace putgd::test;
class CopyFailureSource final:public SourceAdapter {
public:quint64 revision=0;bool fail=true;int reads=0,copies=0;
 quint64 clipboardRevision()const override{return revision;}QJsonObject capabilities()const override{return {{"clipboard",true}};}
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++reads;reply({textPayload(),{}});}
 void cancelAcquisition()override{}
 void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;if(fail)reply({"clipboard_unavailable","Synthetic clipboard refusal",{}});else{++revision;reply({});}}
};
class OperationsGui:public QObject {
 Q_OBJECT

private slots:
 void uploadedResultCanRetryCopyWithoutAnotherUpload() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto s){return registry.validate(id,s);});QVERIFY(settings.save(configuration(d.path()),{}));CopyFailureSource source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());RecentOperations view(operations);QString id;operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);id=data["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());auto* rows=view.findChild<QTreeWidget*>("recentOperations");QTRY_VERIFY(rows->topLevelItemCount()>0);QTRY_COMPARE(rows->topLevelItem(0)->text(3),QString("failed"));QCOMPARE(rows->topLevelItem(0)->child(0)->text(2),QString("uploaded"));QVERIFY(rows->topLevelItem(0)->child(0)->text(1).startsWith("https://example.invalid/"));rows->setCurrentItem(rows->topLevelItem(0));source.fail=false;view.findChild<QPushButton*>("copyOperationResult")->click();QTRY_COMPARE(rows->topLevelItem(0)->text(3),QString("copied"));QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,2);
 }
 void localOperationShowsIndependentItemAndCopyStates() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload=textPayload();source.payload.bytes=QByteArray(1572864,'x');OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());RecentOperations view(operations);view.resize(1000,700);view.show();QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);id=data["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());auto* rows=view.findChild<QTreeWidget*>("recentOperations");QVERIFY(rows);QTRY_VERIFY(rows->topLevelItemCount()>0);QTRY_COMPARE(rows->topLevelItem(0)->text(2),QString("completed"));QVERIFY(rows->topLevelItem(0)->childCount()>0);QTRY_COMPARE(source.copied.kind,QString("text"));QVERIFY(rows->topLevelItem(0)->child(0)->text(3).contains("saved"));QVERIFY(rows->topLevelItem(0)->child(0)->text(4).contains("1.5 MiB"));
  QVERIFY2(view.width()<=1000&&view.height()<=700,"Activity must fit a 1000×700 logical-pixel window");
  for(const auto& name:QStringList{"copyOperationResult","retryOperation","retryLocalSave","cancelOperation"}){
   auto* button=view.findChild<QPushButton*>(name);QVERIFY(button);QVERIFY(button->isVisible());
   QVERIFY(view.rect().contains(QRect(button->mapTo(&view,QPoint{}),button->size())));
  }
 }
};QTEST_MAIN(OperationsGui)
#include "test_operations_view.moc"
