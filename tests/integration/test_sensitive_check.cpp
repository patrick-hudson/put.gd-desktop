// #1804: Check captures for sensitive text before uploading, through the
// resident's operation service, the real editor helper and the real putgd-ocr.
// Only test values: Visa's documented test number and AWS's documented example key.
#include "fixtures/U2Fixtures.h"
#include "core/SanitizedPng.h"
#include "core/AllocatorPolicy.h"
#include "fixtures/Waits.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QFont>
#include <QPainter>
using namespace putgd;using namespace putgd::test;

namespace {
// Black text on white, large enough to read at once.
QImage page(const QStringList& lines){
 QImage image(900,80+70*int(lines.size()),QImage::Format_RGB32);image.fill(Qt::white);
 QPainter painter(&image);QFont font("DejaVu Sans Mono");font.setPixelSize(40);painter.setFont(font);painter.setPen(Qt::black);
 for(int i=0;i<lines.size();++i)painter.drawText(40,80+70*i,lines[i]);
 return image;
}
struct Call {Error error;QJsonObject result;bool done=false;};
Call call(std::function<void(JsonReply)> start){
 Call outcome;start([&](Error e,QJsonObject r){outcome={e,r,true};});
 (void)putgd::test::waitFor([&]{return outcome.done;},15000);
 return outcome;
}
struct Fixture {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<OpaqueProvider> provider=std::make_shared<OpaqueProvider>();
 std::unique_ptr<SettingsService> settings;Source source;std::unique_ptr<OperationService> operations;
 Fixture(QImage image,bool check=true,bool keepLocal=false){
  registry.add("opaque",provider);
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=keepLocal;doc["archive"]=archive;
  auto preferences=doc["preferences"].toObject();preferences["sensitiveTextCheck"]=QJsonObject{{"enabled",check},{"categories",QJsonArray{"card","key"}}};preferences["textRecognitionLanguages"]=QJsonArray{"eng"};doc["preferences"]=preferences;
  if(!settings->save(doc,{}))qFatal("fixture configuration rejected");
  source.payload.kind="image";source.payload.image=std::move(image);
  operations=std::make_unique<OperationService>(dir.filePath("data"),*settings,registry,source);
 }
 QJsonObject inspect(const QString& id){return call([&](JsonReply r){operations->inspect(id,r);}).result;}
 // Waits for a phase, and names the error when the operation ended another way.
 bool reaches(const QString& id,const QString& phase,int ms=40000){
  const bool reached=putgd::test::waitFor([&]{const auto op=inspect(id);return op["phase"].toString()==phase||!Operation::activeState(op["state"].toString());},ms)&&inspect(id)["phase"].toString()==phase;
  if(!reached)qWarning().noquote()<<"operation ended"<<inspect(id)["state"].toString()<<QJsonDocument(inspect(id)["error"].toObject()).toJson(QJsonDocument::Compact);
  return reached;
 }
 QString start(const QJsonObject& options={}){return call([&](JsonReply r){operations->invoke("region",r,options);}).result["operationId"].toString();}
};
const QStringList card{"Card 4111 1111 1111 1111"};
const QStringList key{"key AKIAIOSFODNN7EXAMPLE"};
}

class SensitiveCheckTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){
  QVERIFY(applyResidentAllocatorPolicy());
  if(!QFileInfo(QCoreApplication::applicationDirPath()+"/putgd-ocr").isExecutable())QSKIP("putgd-ocr is not built here");
 }
 // Nothing found: the upload goes ahead with no decision and no findings.
 void aCleanCaptureUploadsWithoutAsking(){
  Fixture f(page({"Meeting notes for Tuesday"}));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),40000);
  QCOMPARE(f.provider->uploads.load(),1);QVERIFY(!f.inspect(id).contains("sensitive"));
 }
 // The option off is today's behaviour: no check phase at all.
 void theOptionOffNeverChecks(){
  Fixture f(page(card),false);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  QCOMPARE(f.provider->uploads.load(),1);
 }
 // A card number pauses the upload until operation.decide; Upload anyway sends the original.
 void aCardNumberWaitsAndUploadAnywaySendsTheOriginal(){
  Fixture f(page(card));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  const auto found=f.inspect(id)["sensitive"].toObject();
  QCOMPARE(found["found"].toObject()["card"].toInt(),1);QVERIFY(found["complete"].toBool());QCOMPARE(f.provider->uploads.load(),0);
  // Only the kinds and counts: nothing recognized reaches the operation.
  QVERIFY(!QJsonDocument(f.inspect(id)).toJson().contains("4111"));
  QCOMPARE(call([&](JsonReply r){f.operations->decideSensitive(id,"later",r);}).error.code,QString("invalid_request"));
  const auto decided=call([&](JsonReply r){f.operations->decideSensitive(id,"upload",r);});QVERIFY2(!decided.error,qPrintable(decided.error.code));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  QCOMPARE(f.provider->uploads.load(),1);
  QCOMPARE(call([&](JsonReply r){f.operations->decideSensitive(id,"upload",r);}).error.code,QString("sensitive_decision_unavailable"));
 }
 // Cancel uploads nothing; with Keep a local copy on, the capture stays as a local item.
 void cancelKeepsOnlyALocalCopy_data(){QTest::addColumn<bool>("keep");QTest::newRow("kept")<<true;QTest::newRow("discarded")<<false;}
 void cancelKeepsOnlyALocalCopy(){
  QFETCH(bool,keep);
  Fixture f(page(key),true,keep);PUTGD_TRY_VERIFY(f.operations->ready());
  // The prompt itself, as the tray and main window ask for it, settled the way an agent settles it.
  const auto id=f.start({{"sensitiveText","prompt"}});
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  QCOMPARE(f.inspect(id)["sensitive"].toObject()["found"].toObject()["key"].toInt(),1);
  // A second decision right behind the first is refused, and the first one stands.
  Call first,second;
  f.operations->decideSensitive(id,"cancel",[&](Error e,QJsonObject r){first={e,r,true};});
  f.operations->decideSensitive(id,"upload",[&](Error e,QJsonObject r){second={e,r,true};});
  PUTGD_TRY_VERIFY(first.done&&second.done);
  QVERIFY2(!first.error,qPrintable(first.error.code));QVERIFY(bool(second.error));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString(keep?"completed":"cancelled"),20000);
  QCOMPARE(f.provider->uploads.load(),0);
  if(keep){const auto items=f.inspect(id)["items"].toArray();QCOMPARE(items.size(),1);QCOMPARE(items.first().toObject()["uploadStatus"].toString(),QString("local_only"));}
 }
 // Redact and upload opens the editor with the suggestions placed; its PNG is what uploads.
 void redactOpensTheEditorWithTheSuggestionsPlaced(){
  Fixture f(page(card));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  QCOMPARE(call([&](JsonReply r){f.operations->editorCommand(id,"inspect",{},r);}).error.code,QString("editor_unavailable"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"redact",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["phase"].toString(),QString("editing"),20000);
  Call state;PUTGD_TRY_VERIFY((state=call([&](JsonReply r){f.operations->editorCommand(id,"inspect",{},r);}),!state.error));
  const auto shapes=state.result["shapes"].toArray();QVERIFY(!shapes.isEmpty());
  QRect covered;for(const auto& value:shapes){const auto shape=value.toObject();QCOMPARE(shape["suggested"].toString(),QString("card"));const auto r=shape["rect"].toArray();covered|=QRect(r[0].toInt(),r[1].toInt(),r[2].toInt(),r[3].toInt());}
  QVERIFY(!call([&](JsonReply r){f.operations->editorCommand(id,"submit",{},r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  QCOMPARE(f.provider->uploads.load(),1);
  HistoryStore journal(f.operations->databasePath());const auto item=journal.operation(id).items.first();
  QFile artifact(item.artifact.path);QVERIFY(artifact.open(QIODevice::ReadOnly));const auto bytes=artifact.readAll();QVERIFY(!inspectSanitizedPng(bytes));
  QCOMPARE(QImage::fromData(bytes,"PNG").convertToFormat(QImage::Format_RGB32).pixel(covered.center()),qRgb(0,0,0));
 }
};
QTEST_MAIN(SensitiveCheckTest)
#include "test_sensitive_check.moc"
