#include "fixtures/U2Fixtures.h"
#include "core/SanitizedPng.h"
#include <QDirIterator>
#include <QImageReader>
#include <QRandomGenerator>
#include <QScopeGuard>
#include "core/AllocatorPolicy.h"
#include "fixtures/Waits.h"
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
 (void)putgd::test::waitFor([&]{return outcome.done;},15000);
 return outcome;
}
// Commands sent before the window is up report editor_starting; retry until ready.
Call command(OperationService& operations,const QString& id,const QString& name,const QJsonObject& params={}){
 Call outcome;
 for(int attempt=0;attempt<300;++attempt){outcome=call([&](JsonReply r){operations.editorCommand(id,name,params,r);});if(outcome.error.code!="editor_starting")break;putgd::test::settle(50);}
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
 explicit Fixture(bool keepLocal=true,int concurrentUploads=0){
  registry.add("opaque",provider);
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=keepLocal;doc["archive"]=archive;
  if(concurrentUploads){auto limits=doc["limits"].toObject();limits["concurrentUploads"]=concurrentUploads;doc["limits"]=limits;}
  if(!settings->save(doc,{}))qFatal("fixture configuration rejected");
  source.payload.kind="image";source.payload.image=capture();
  operations=std::make_unique<OperationService>(dir.filePath("data"),*settings,registry,source);
 }
};
}

class EditOperationsTest:public QObject {
 Q_OBJECT
private slots:
 // Like the resident's main(), fix the allocator policy before any work runs.
 void initTestCase(){QVERIFY(applyResidentAllocatorPolicy());}
 void appearanceChangesReachTheAlreadyOpenEditor(){
  Fixture f;PUTGD_TRY_VERIFY(f.operations->ready());f.operations->setEditorAppearance("light");
  const auto id=call([&](JsonReply r){f.operations->invoke("edit",r);}).result["operationId"].toString();QVERIFY(!id.isEmpty());
  // A change during startup must survive the captured bootstrap preference.
  f.operations->setEditorAppearance("dark");
  auto state=command(*f.operations,id,"inspect");QVERIFY(!state.error);QCOMPARE(state.result["view"].toObject()["scheme"].toString(),QString("dark"));
  for(const auto& scheme:{QStringLiteral("light"),QStringLiteral("dark")}){
   f.operations->setEditorAppearance(scheme);PUTGD_TRY_COMPARE(command(*f.operations,id,"inspect").result["view"].toObject()["scheme"].toString(),scheme);
  }
  QVERIFY(!command(*f.operations,id,"cancel").error);PUTGD_TRY_VERIFY(f.operations->editingOperation().isEmpty());QCOMPARE(f.provider->uploads.load(),0);
 }
 void existingImagesWithoutOutput_data(){QTest::addColumn<QString>("source");QTest::newRow("clipboard")<<QString("clipboard");QTest::newRow("file")<<QString("file");}
 void existingImagesWithoutOutput(){
  QFETCH(QString,source);Fixture f(false);auto doc=Settings::defaults();QVERIFY(f.settings->save(doc,f.settings->revision()));PUTGD_TRY_VERIFY(f.operations->ready());
  QJsonObject options{{"source",source}};if(source=="file"){const auto path=f.dir.filePath("input.png");QVERIFY(capture().save(path));options["path"]=path;}
  const auto opened=call([&](JsonReply r){f.operations->invoke("edit",r,options);});QVERIFY2(!opened.error,qPrintable(opened.error.message));const auto id=opened.result["operationId"].toString();QVERIFY(!id.isEmpty());
  QVERIFY(!command(*f.operations,id,"inspect").error);const auto refused=command(*f.operations,id,"submit");QCOMPARE(refused.error.code,QString("output_required"));QCOMPARE(refused.error.message,QString("No output is enabled. Choose a destination or turn on Keep a local copy in Settings → Capture."));
  const auto state=command(*f.operations,id,"inspect");QVERIFY(!state.error);QCOMPARE(state.result["phase"].toString(),QString("editing"));QCOMPARE(state.result["preparation"].toObject()["code"].toString(),QString("output_required"));QCOMPARE(state.result["view"].toObject()["preparationFailure"].toObject()["text"].toString(),refused.error.message);
  const auto copy=f.dir.filePath("saved-copy.png");const auto saved=command(*f.operations,id,"saveCopy",{{"path",copy}});QVERIFY2(!saved.error,qPrintable(saved.error.message));QVERIFY(QFileInfo(copy).isFile());QCOMPARE(QImage(copy).size(),capture().size());QCOMPARE(f.provider->uploads.load(),0);QVERIFY(!command(*f.operations,id,"cancel").error);PUTGD_TRY_COMPARE(f.operations->diagnostics()["activeOperations"].toInt(),0);
 }
 void capturesWithoutOutputStayGated(){
  Fixture f(false);QVERIFY(f.settings->save(Settings::defaults(),f.settings->revision()));PUTGD_TRY_VERIFY(f.operations->ready());for(const auto& action:Settings::actionNames()){const auto result=call([&](JsonReply r){f.operations->invoke(action,r);});QCOMPARE(result.error.code,QString("output_required"));}QCOMPARE(f.source.reads,0);QCOMPARE(f.provider->uploads.load(),0);
 }
 void submittedEditorWaitsOutsideEditing_data(){
  QTest::addColumn<bool>("cancelQueued");
  QTest::newRow("drain")<<false;
  QTest::newRow("cancel")<<true;
 }
 void submittedEditorWaitsOutsideEditing(){
  QFETCH(bool,cancelQueued);
  Fixture f(false,1);PUTGD_TRY_VERIFY(f.operations->ready());
  f.provider->deferUploads=true;f.provider->dispatcher=this;
  auto drain=qScopeGuard([&]{for(auto complete:std::exchange(f.provider->completions,{}))complete();});
  // Occupy the only worker until after the real editor has submitted and exited.
  const auto blocker=call([&](JsonReply r){f.operations->invoke("clipboard",r);}).result["operationId"].toString();
  PUTGD_TRY_COMPARE(f.provider->completions.size(),1);
  const auto id=call([&](JsonReply r){f.operations->invoke("edit",r);}).result["operationId"].toString();QVERIFY(!id.isEmpty());
  QVERIFY(!command(*f.operations,id,"inspect").error);
  HistoryStore journal(f.operations->databasePath());
  QCOMPARE(journal.operation(id).state,QString("editing"));
  QVERIFY(!command(*f.operations,id,"submit").error);
  PUTGD_TRY_COMPARE(f.operations->editingOperation(),QString());
  PUTGD_TRY_COMPARE(journal.operation(id).phase,QString("queued"));
  QCOMPARE(journal.operation(id).state,QString("acquiring"));
  QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(f.provider->uploads.load(),1);
  QCOMPARE(command(*f.operations,id,"inspect").error.code,QString("editor_unavailable"));
  const auto inspected=call([&](JsonReply r){f.operations->inspect(id,r);});QVERIFY(!inspected.error);
  QCOMPARE(inspected.result["state"].toString(),QString("acquiring"));QCOMPARE(inspected.result["phase"].toString(),QString("queued"));
  const auto recent=call([&](JsonReply r){f.operations->recent(r);});QVERIFY(!recent.error);bool found=false;
  for(const auto& value:recent.result["operations"].toArray())if(value.toObject()["id"]==id){found=true;QCOMPARE(value.toObject()["state"].toString(),QString("acquiring"));QCOMPARE(value.toObject()["phase"].toString(),QString("queued"));}
  QVERIFY(found);
  if(cancelQueued)QVERIFY(!call([&](JsonReply r){f.operations->cancel(id,r);}).error);
  f.provider->completions.takeFirst()();
  if(!cancelQueued){PUTGD_TRY_COMPARE(f.provider->completions.size(),1);f.provider->completions.takeFirst()();}
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString(cancelQueued?"cancelled":"completed"));
  PUTGD_TRY_COMPARE(journal.operation(blocker).state,QString("completed"));
  QCOMPARE(f.provider->uploads.load(),cancelQueued?1:2);
  QCOMPARE(journal.operation(id).items.size(),cancelQueued?0:1);
  PUTGD_TRY_COMPARE(f.operations->diagnostics()["activeOperations"].toInt(),0);
 }
 void editorSummaryUsesResolvedSharingAndTheCompleteDestination_data(){
  QTest::addColumn<QString>("mode");QTest::addColumn<bool>("local");QTest::addColumn<QString>("prefix");QTest::addColumn<QString>("suffix");
  QTest::newRow("private")<<QString("private")<<false<<QString("Private upload to ")<<QString();
  QTest::newRow("shared")<<QString("share")<<false<<QString("Upload to ")<<QString(" · Shared link");
  QTest::newRow("provider-default")<<QString("default")<<false<<QString("Upload to ")<<QString(" · Shared link");
  QTest::newRow("local")<<QString("default")<<true<<QString("Save to the local archive")<<QString();
 }
 void editorSummaryUsesResolvedSharingAndTheCompleteDestination(){
  QFETCH(QString,mode);QFETCH(bool,local);QFETCH(QString,prefix);QFETCH(QString,suffix);
  Fixture f;PUTGD_TRY_VERIFY(f.operations->ready());auto doc=f.settings->document();auto actions=doc["actions"].toObject();auto edit=actions["edit"].toObject();
  edit["publicationMode"]=mode;if(local){edit["destination"]="local";edit["profileId"]=QJsonValue();}actions["edit"]=edit;doc["actions"]=actions;
  const QString label=QString(128,QChar('W'));auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["label"]=label;profiles[0]=profile;doc["profiles"]=profiles;
  QVERIFY(f.settings->save(doc,f.settings->revision()));
  const auto invoked=call([&](JsonReply r){f.operations->invoke("edit",r);});QVERIFY2(!invoked.error,qPrintable(invoked.error.message));
  const auto id=invoked.result["operationId"].toString();auto state=command(*f.operations,id,"inspect");QVERIFY2(!state.error,qPrintable(state.error.message));
  const auto expected=local?prefix:prefix+label+suffix;QCOMPARE(state.result["destination"].toString(),expected);QCOMPARE(state.result["view"].toObject()["destination"].toObject()["text"].toString(),expected);
  // The new view action crosses the resident/helper boundary, then undo restores it.
  QVERIFY(!command(*f.operations,id,"apply",redactSecret()).error);auto selected=command(*f.operations,id,"view",{{"action","selectNext"}});QVERIFY(!selected.error);
  QVERIFY(!command(*f.operations,id,"view",{{"action","removeMark"}}).error);QCOMPARE(command(*f.operations,id,"inspect").result["shapes"].toArray().size(),0);
  QVERIFY(!command(*f.operations,id,"apply",{{"commands",QJsonArray{QJsonObject{{"op","undo"}}}}}).error);QCOMPARE(command(*f.operations,id,"inspect").result["shapes"].toArray().size(),1);
  QVERIFY(!command(*f.operations,id,"cancel").error);
 }
 void uploadPublishesOnlyTheRedactedResult(){
  Fixture f;PUTGD_TRY_VERIFY(f.operations->ready());
  QVERIFY(f.operations->capabilities()["editor"].toObject()["available"].toBool());
  auto invoked=call([&](JsonReply r){f.operations->invoke("edit",r);});QVERIFY2(!invoked.error,qPrintable(invoked.error.code));
  const auto id=invoked.result["operationId"].toString();QCOMPARE(invoked.result["operation"].toObject()["action"].toString(),QString("edit"));
  HistoryStore journal(f.operations->databasePath());
  auto state=command(*f.operations,id,"inspect");QVERIFY2(!state.error,qPrintable(state.error.code));QCOMPARE(state.result["operationId"].toString(),id);
  QCOMPARE(state.result["destination"].toString(),QString("Upload to Synthetic opaque provider · Shared link"));
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString("editing"));QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(f.provider->uploads.load(),0);
  // One editor per resident: a second request raises it and names the open edit.
  auto second=call([&](JsonReply r){f.operations->invoke("edit",r);});QCOMPARE(second.error.code,QString("editor_active"));QCOMPARE(second.error.details["operationId"].toString(),id);
  QCOMPARE(command(*f.operations,"another-id","inspect").error.code,QString("editor_unavailable"));
  QVERIFY(!command(*f.operations,id,"apply",redactSecret()).error);
  QVERIFY(!command(*f.operations,id,"submit").error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString("completed"),20000);
  PUTGD_TRY_COMPARE(f.operations->editingOperation(),QString());
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
 void failedUploadRetriesTheEditedBytesOnly(){
  // Retry binds to the finalized digest: it never reacquires or reopens the source.
  Fixture f;PUTGD_TRY_VERIFY(f.operations->ready());f.provider->failUpload=true;
  const auto id=call([&](JsonReply r){f.operations->invoke("edit",r);}).result["operationId"].toString();QVERIFY(!id.isEmpty());
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("editing"));
  QVERIFY(!command(*f.operations,id,"apply",redactSecret()).error);QVERIFY(!command(*f.operations,id,"submit").error);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(QStringList({"failed","partial"}).contains(journal.operation(id).state),20000);
  const auto failed=journal.operation(id);QCOMPARE(failed.items.size(),1);QCOMPARE(failed.items.first().uploadStatus,QString("failed"));
  const auto digest=failed.items.first().artifact.sha256;QVERIFY(!digest.isEmpty());QCOMPARE(f.source.reads,1);PUTGD_TRY_COMPARE(f.operations->editingOperation(),QString());
  f.provider->failUpload=false;
  // Retry is offered once the failed operation has finished its follow-up steps.
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.operations->diagnostics()["activeOperations"].toInt(),0,10000);
  auto retried=call([&](JsonReply r){f.operations->retry(id,r);});QVERIFY2(!retried.error,qPrintable(retried.error.code));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString("completed"),20000);
  const auto done=journal.operation(id);QCOMPARE(done.items.size(),1);QCOMPARE(done.items.first().uploadStatus,QString("uploaded"));
  QCOMPARE(done.items.first().artifact.sha256,digest);QCOMPARE(f.source.reads,1);QCOMPARE(f.operations->editingOperation(),QString());
  QFile artifact(done.items.first().artifact.path);QVERIFY(artifact.open(QIODevice::ReadOnly));
  const auto uploaded=QImage::fromData(artifact.readAll(),"PNG").convertToFormat(QImage::Format_RGB32);QCOMPARE(uploaded.pixel(secret.center()),qRgb(0,0,0));
  verifyNoSecret(f.dir.filePath("data"));
 }
 void cancellingAnOpenEditStoresAndUploadsNothing(){
  Fixture f;PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=call([&](JsonReply r){f.operations->invoke("edit",r);}).result["operationId"].toString();QVERIFY(!id.isEmpty());
  QVERIFY(!command(*f.operations,id,"inspect").error);
  QVERIFY(!command(*f.operations,id,"apply",redactSecret()).error);
  QVERIFY(!call([&](JsonReply r){f.operations->cancel(id,r);}).error);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString("cancelled"),20000);
  QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(f.provider->uploads.load(),0);
  PUTGD_TRY_COMPARE(f.operations->editingOperation(),QString());
  QVERIFY(!QDir(f.dir.filePath("archive")).exists()||QDir(f.dir.filePath("archive")).isEmpty());
  QDirIterator it(f.dir.filePath("data/artifacts"),QDir::Files,QDirIterator::Subdirectories);QVERIFY(!it.hasNext());
  // The editor's own discard reaches the same outcome.
  const auto next=call([&](JsonReply r){f.operations->invoke("edit",r);}).result["operationId"].toString();QVERIFY(!next.isEmpty());
  QVERIFY(!command(*f.operations,next,"cancel").error);PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(next).state,QString("cancelled"),20000);
  QCOMPARE(journal.operation(next).error.code,QString("cancelled"));QCOMPARE(f.provider->uploads.load(),0);
 }
 void fileSourceOpensStaticImagesOnly(){
  Fixture f(false);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto jpeg=f.dir.filePath("synthetic.jpg");QVERIFY(capture().save(jpeg,"JPEG"));
  auto opened=call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","file"},{"path",jpeg}});});QVERIFY2(!opened.error,qPrintable(opened.error.code));
  const auto id=opened.result["operationId"].toString();QCOMPARE(f.source.reads,0);
  HistoryStore journal(f.operations->databasePath());
  auto state=command(*f.operations,id,"inspect");QVERIFY2(!state.error,qPrintable(state.error.code+" "+journal.operation(id).state+" "+journal.operation(id).error.code+" "+journal.operation(id).error.message));QCOMPARE(state.result["width"].toInt(),320);
  QVERIFY(!command(*f.operations,id,"submit").error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString("completed"),20000);
  QCOMPARE(journal.operation(id).items.first().artifact.mime,QString("image/png"));
  QFile original(jpeg);QVERIFY(original.open(QIODevice::ReadOnly));QVERIFY(QImage::fromData(original.readAll(),"JPEG").size()==QSize(320,200));
  const auto text=f.dir.filePath("synthetic.txt");QFile file(text);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic text");file.close();
  const auto rejected=call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","file"},{"path",text}});}).result["operationId"].toString();
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(rejected).state,QString("failed"),20000);QCOMPARE(journal.operation(rejected).error.code,QString("edit_source_unsupported"));
  QCOMPARE(call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","file"},{"path","relative.png"}});}).error.code,QString("invalid_request"));
  QCOMPARE(call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","shell"}});}).error.code,QString("invalid_request"));
  QCOMPARE(call([&](JsonReply r){f.operations->invoke("region",r,{{"source","file"}});}).error.code,QString("invalid_request"));
 }
 void interruptedEditRecoversWithoutUploading(){
  QTemporaryDir dir;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(dir.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(dir.path()),{}));
  QVERIFY(QDir().mkpath(dir.filePath("data/editing/pge-abcdef")));QFile staged(dir.filePath("data/editing/pge-abcdef/input.png"));QVERIFY(staged.open(QIODevice::WriteOnly));staged.write("synthetic");staged.close();
  {HistoryStore journal(dir.filePath("data/history.sqlite"));Operation op;op.id="interrupted-edit";op.action="edit";op.state="editing";op.phase="editing";QVERIFY(!journal.saveOperation(op));}
  Source source;OperationService operations(dir.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  HistoryStore journal(operations.databasePath());const auto op=journal.operation("interrupted-edit");
  QCOMPARE(op.state,QString("interrupted"));QCOMPARE(op.error.code,QString("edit_interrupted"));QVERIFY(op.items.isEmpty());
  QVERIFY(!QDir(dir.filePath("data/editing")).exists());QCOMPARE(provider->uploads.load(),0);
  QCOMPARE(call([&](JsonReply r){operations.retry("interrupted-edit",r);}).error.code,QString("artifact_missing"));
 }

 void residentReturnsEditBuffersAfterEachCycle(){
  // Cinnamon acceptance showed the resident ratcheting 29 -> 140 MiB across edits:
  // freed 4K rasters stayed in per-thread arena tops. The production policy must
  // keep freed-but-retained heap small. glibc only; other allocators skip.
#if !defined(__GLIBC__)
  QSKIP("glibc allocator policy");
#else
  Fixture f(false);PUTGD_TRY_VERIFY(f.operations->ready());
  QImage big(3840,2160,QImage::Format_RGB32);QRandomGenerator random(5);for(int y=0;y<big.height();++y){auto* line=reinterpret_cast<QRgb*>(big.scanLine(y));for(int x=0;x<big.width();++x)line[x]=random.generate()|0xff000000U;}
  const auto path=f.dir.filePath("uhd.png");QVERIFY(big.save(path,"PNG",80));big=QImage();
  HistoryStore journal(f.operations->databasePath());
  malloc_trim(0);const auto baseline=mallinfo2().fordblks;
  for(int cycle=0;cycle<4;++cycle){
   const auto id=call([&](JsonReply r){f.operations->invoke("edit",r,{{"source","file"},{"path",path}});}).result["operationId"].toString();
   QVERIFY(!command(*f.operations,id,"inspect").error);
   const bool submit=cycle%2==0;QVERIFY(!command(*f.operations,id,submit?"submit":"cancel").error);
   PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(id).state,QString(submit?"completed":"cancelled"),60000);
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(f.operations->diagnostics()["activeOperations"].toInt()==0,10000);
   malloc_trim(0);const auto heap=mallinfo2();
   // One retained 4K raster is about 32 MiB per cycle; allow small-chunk noise only.
   QVERIFY2(heap.fordblks<baseline+8*1024*1024,qPrintable(QString("cycle %1 retained %2 free heap bytes over a %3 baseline").arg(cycle).arg(heap.fordblks).arg(baseline)));
  }
#endif
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
