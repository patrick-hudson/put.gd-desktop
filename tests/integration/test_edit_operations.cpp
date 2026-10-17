#include "fixtures/U2Fixtures.h"
#include "core/SanitizedPng.h"
#include <QDirIterator>
#include <QImageReader>
#include <QRandomGenerator>
using namespace putgd;using namespace putgd::test;

namespace {
const QRect secret(40,30,80,40);
QImage capture(){
 QImage image(320,200,QImage::Format_RGB32);QRandomGenerator random(77);
 for(int y=0;y<200;++y)for(int x=0;x<320;++x)image.setPixel(x,y,random.generate()|0xff000000U);
 // A saturated synthetic secret that never occurs outside the mask.
 for(int y=secret.top();y<=secret.bottom();++y)for(int x=secret.left();x<=secret.right();++x)image.setPixel(x,y,qRgb(255,0,255));
 return image;
}
struct Call {Error error;QJsonObject result;bool done=false;};
Call call(std::function<void(JsonReply)> start){
 Call outcome;start([&](Error e,QJsonObject r){outcome={e,r,true};});
 for(int i=0;i<1500&&!outcome.done;++i)QTest::qWait(10);
 return outcome;
}
// Commands sent before the window is up report editor_starting; retry until ready.
Call command(OperationService& operations,const QString& id,const QString& name,const QJsonObject& params={}){
 Call outcome;
 for(int attempt=0;attempt<300;++attempt){outcome=call([&](JsonReply r){operations.editorCommand(id,name,params,r);});if(outcome.error.code!="editor_starting")break;QTest::qWait(50);}
 return outcome;
}
// Every retained raster must show only the redacted result.
void verifyNoSecret(const QString& root){
 QDirIterator it(root,QDir::Files|QDir::Hidden,QDirIterator::Subdirectories);int images=0;
 while(it.hasNext()){
  const auto path=it.next();QImageReader reader(path);if(!reader.canRead())continue;
  const auto image=reader.read().convertToFormat(QImage::Format_RGB32);++images;
  for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)QVERIFY2(image.pixel(x,y)!=qRgb(255,0,255),qPrintable(path));
 }
 QVERIFY(images>0);
}
QJsonObject redactSecret(){return {{"commands",QJsonArray{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{secret.x(),secret.y(),secret.width(),secret.height()}}}}}}}};}
struct Fixture {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<OpaqueProvider> provider=std::make_shared<OpaqueProvider>();
 std::unique_ptr<SettingsService> settings;Source source;std::unique_ptr<OperationService> operations;
 explicit Fixture(bool keepLocal=true){
  registry.add("opaque",provider);
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=keepLocal;doc["archive"]=archive;
  if(!settings->save(doc,{}))qFatal("fixture configuration rejected");
  source.payload.kind="image";source.payload.image=capture();
  operations=std::make_unique<OperationService>(dir.filePath("data"),*settings,registry,source);
 }
};
}

class EditOperationsTest:public QObject {
 Q_OBJECT
private slots:
 void uploadPublishesOnlyTheRedactedResult(){
  Fixture f;QTRY_VERIFY(f.operations->ready());
  QVERIFY(f.operations->capabilities()["editor"].toObject()["available"].toBool());
  auto invoked=call([&](JsonReply r){f.operations->invoke("edit",r);});QVERIFY2(!invoked.error,qPrintable(invoked.error.code));
  const auto id=invoked.result["operationId"].toString();QCOMPARE(invoked.result["operation"].toObject()["action"].toString(),QString("edit"));
  HistoryStore journal(f.operations->databasePath());
  auto state=command(*f.operations,id,"inspect");QVERIFY2(!state.error,qPrintable(state.error.code));QCOMPARE(state.result["operationId"].toString(),id);
  QCOMPARE(state.result["destination"].toString(),QString("Upload to Synthetic opaque provider"));
  QTRY_COMPARE(journal.operation(id).state,QString("editing"));QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(f.provider->uploads.load(),0);
  // One editor per resident: a second request raises it and names the open edit.
  auto second=call([&](JsonReply r){f.operations->invoke("edit",r);});QCOMPARE(second.error.code,QString("editor_active"));QCOMPARE(second.error.details["operationId"].toString(),id);
  QCOMPARE(command(*f.operations,"another-id","inspect").error.code,QString("editor_unavailable"));
  QVERIFY(!command(*f.operations,id,"apply",redactSecret()).error);
  QVERIFY(!command(*f.operations,id,"submit").error);
  QTRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString("completed"),20000);
  QTRY_COMPARE(f.operations->editingOperation(),QString());
  const auto op=journal.operation(id);QCOMPARE(op.items.size(),1);QCOMPARE(f.provider->uploads.load(),1);
  const auto item=op.items.first();QCOMPARE(item.artifact.mime,QString("image/png"));QCOMPARE(item.uploadStatus,QString("uploaded"));QCOMPARE(item.local.status,QString("saved"));
  QFile artifact(item.artifact.path);QVERIFY(artifact.open(QIODevice::ReadOnly));const auto bytes=artifact.readAll();QVERIFY(!inspectSanitizedPng(bytes));
  const auto uploaded=QImage::fromData(bytes,"PNG").convertToFormat(QImage::Format_RGB32);QCOMPARE(uploaded.size(),QSize(320,200));
  QCOMPARE(uploaded.pixel(secret.center()),qRgb(0,0,0));QCOMPARE(uploaded.pixel(0,0),capture().pixel(0,0));
  QVERIFY(!item.artifact.previewPath.isEmpty());
  verifyNoSecret(f.dir.filePath("data"));verifyNoSecret(f.dir.filePath("archive"));
  QCOMPARE(QDir(f.dir.filePath("data/editing")).entryList(QDir::NoDotAndDotDot|QDir::AllEntries).size(),0);
  QCOMPARE(f.source.reads,1);
 }
 void cancellingAnOpenEditStoresAndUploadsNothing(){
  Fixture f;QTRY_VERIFY(f.operations->ready());
  const auto id=call([&](JsonReply r){f.operations->invoke("edit",r);}).result["operationId"].toString();QVERIFY(!id.isEmpty());
  QVERIFY(!command(*f.operations,id,"inspect").error);
  QVERIFY(!command(*f.operations,id,"apply",redactSecret()).error);
  QVERIFY(!call([&](JsonReply r){f.operations->cancel(id,r);}).error);
  HistoryStore journal(f.operations->databasePath());QTRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString("cancelled"),20000);
  QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(f.provider->uploads.load(),0);
  QTRY_COMPARE(f.operations->editingOperation(),QString());
  QVERIFY(!QDir(f.dir.filePath("archive")).exists()||QDir(f.dir.filePath("archive")).isEmpty());
  QDirIterator it(f.dir.filePath("data/artifacts"),QDir::Files,QDirIterator::Subdirectories);QVERIFY(!it.hasNext());
  // The editor's own discard reaches the same outcome.
  const auto next=call([&](JsonReply r){f.operations->invoke("edit",r);}).result["operationId"].toString();QVERIFY(!next.isEmpty());
  QVERIFY(!command(*f.operations,next,"cancel").error);QTRY_COMPARE_WITH_TIMEOUT(journal.operation(next).state,QString("cancelled"),20000);
  QCOMPARE(journal.operation(next).error.code,QString("cancelled"));QCOMPARE(f.provider->uploads.load(),0);
 }
 void fileSourceOpensStaticImagesOnly(){
  Fixture f(false);QTRY_VERIFY(f.operations->ready());
  const auto jpeg=f.dir.filePath("synthetic.jpg");QVERIFY(capture().save(jpeg,"JPEG"));
  auto opened=call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","file"},{"path",jpeg}});});QVERIFY2(!opened.error,qPrintable(opened.error.code));
  const auto id=opened.result["operationId"].toString();QCOMPARE(f.source.reads,0);
  HistoryStore journal(f.operations->databasePath());
  auto state=command(*f.operations,id,"inspect");QVERIFY2(!state.error,qPrintable(state.error.code+" "+journal.operation(id).state+" "+journal.operation(id).error.code+" "+journal.operation(id).error.message));QCOMPARE(state.result["width"].toInt(),320);
  QVERIFY(!command(*f.operations,id,"submit").error);
  QTRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString("completed"),20000);
  QCOMPARE(journal.operation(id).items.first().artifact.mime,QString("image/png"));
  QFile original(jpeg);QVERIFY(original.open(QIODevice::ReadOnly));QVERIFY(QImage::fromData(original.readAll(),"JPEG").size()==QSize(320,200));
  const auto text=f.dir.filePath("synthetic.txt");QFile file(text);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic text");file.close();
  const auto rejected=call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","file"},{"path",text}});}).result["operationId"].toString();
  QTRY_COMPARE_WITH_TIMEOUT(journal.operation(rejected).state,QString("failed"),20000);QCOMPARE(journal.operation(rejected).error.code,QString("edit_source_unsupported"));
  QCOMPARE(call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","file"},{"path","relative.png"}});}).error.code,QString("invalid_request"));
  QCOMPARE(call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","shell"}});}).error.code,QString("invalid_request"));
  QCOMPARE(call([&](JsonReply r){f.operations->invoke("region",r,{{"source","file"}});}).error.code,QString("invalid_request"));
 }
 void interruptedEditRecoversWithoutUploading(){
  QTemporaryDir dir;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(dir.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(dir.path()),{}));
  QVERIFY(QDir().mkpath(dir.filePath("data/editing/pge-abcdef")));QFile staged(dir.filePath("data/editing/pge-abcdef/input.png"));QVERIFY(staged.open(QIODevice::WriteOnly));staged.write("synthetic");staged.close();
  {HistoryStore journal(dir.filePath("data/history.sqlite"));Operation op;op.id="interrupted-edit";op.action="edit";op.state="editing";op.phase="editing";QVERIFY(!journal.saveOperation(op));}
  Source source;OperationService operations(dir.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  HistoryStore journal(operations.databasePath());const auto op=journal.operation("interrupted-edit");
  QCOMPARE(op.state,QString("interrupted"));QCOMPARE(op.error.code,QString("edit_interrupted"));QVERIFY(op.items.isEmpty());
  QVERIFY(!QDir(dir.filePath("data/editing")).exists());QCOMPARE(provider->uploads.load(),0);
  QCOMPARE(call([&](JsonReply r){operations.retry("interrupted-edit",r);}).error.code,QString("artifact_missing"));
 }
 void legacyConfigurationInheritsRegionRouteWithoutShortcut(){
  auto doc=Settings::defaults();auto actions=doc["actions"].toObject();actions.remove("edit");auto region=actions["region"].toObject();region["destination"]="local";actions["region"]=region;doc["actions"]=actions;
  QVERIFY(!Settings::validate(doc));const auto settings=Settings::fromValidated(doc);
  QCOMPARE(settings.actions.value("edit").destination,QString("local"));QCOMPARE(settings.actions.value("edit").shortcut,QString());
  QVERIFY(settings.document["actions"].toObject().contains("edit"));
  QCOMPARE(Settings::defaults()["actions"].toObject()["edit"].toObject()["shortcut"].toString(),QString());
  auto extra=doc;auto bad=extra["actions"].toObject();bad["video"]=region;extra["actions"]=bad;QVERIFY(Settings::validate(extra));
 }
};
QTEST_MAIN(EditOperationsTest)
#include "test_edit_operations.moc"
