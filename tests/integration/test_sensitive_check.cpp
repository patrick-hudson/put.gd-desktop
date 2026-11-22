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
 // `check` replaces the sensitiveTextCheck object; `languages` the recognition languages.
 Fixture(QImage image,bool check=true,bool keepLocal=false,QJsonObject checkObject={},QJsonArray languages={"eng"},QJsonArray terms={}){
  registry.add("opaque",provider);
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=keepLocal;doc["archive"]=archive;
  auto preferences=doc["preferences"].toObject();preferences["sensitiveTextCheck"]=checkObject.isEmpty()?QJsonObject{{"enabled",check},{"categories",QJsonArray{"card","key"}}}:checkObject;preferences["textRecognitionLanguages"]=languages;if(!terms.isEmpty())preferences["quickRedactTerms"]=terms;doc["preferences"]=preferences;
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
// #1820: the check object with each kind set; kinds not named are off.
QJsonObject kinds(const QJsonObject& chosen){QJsonObject all{{"email","off"},{"phone","off"},{"ip","off"},{"card","off"},{"key","off"},{"name","off"},{"list","off"}};for(auto it=chosen.begin();it!=chosen.end();++it)all[it.key()]=it.value();return {{"enabled",true},{"kinds",all}};}
const auto everyKind=kinds({{"email","redact"},{"phone","redact"},{"ip","redact"},{"card","redact"},{"key","redact"},{"name","redact"},{"list","redact"}});
// How much of a band of the uploaded or saved image is black: a cover fills the digits' band, text alone does not.
double blackShare(const QImage& image,const QRect& band){
 int black=0;const auto rgb=image.convertToFormat(QImage::Format_RGB32);
 for(int y=band.top();y<=band.bottom();++y)for(int x=band.left();x<=band.right();++x)black+=qGray(rgb.pixel(x,y))<40;
 return double(black)/(band.width()*band.height());
}
// The digits of page()'s line `row` (40 px monospace from x=40, baseline 80+70*row): after the first word.
QRect digits(int row){return QRect(220,52+70*row,320,24);}
QImage uploaded(OperationService& operations,const QString& id){
 HistoryStore journal(operations.databasePath());const auto item=journal.operation(id).items.first();
 QFile artifact(item.artifact.path);if(!artifact.open(QIODevice::ReadOnly))return {};return QImage::fromData(artifact.readAll(),"PNG");
}
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
  QVERIFY2(!first.error,qPrintable(first.error.code));
  QVERIFY2(QStringList({"editor_finalizing","sensitive_decision_unavailable"}).contains(second.error.code),qPrintable(second.error.code));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString(keep?"completed":"cancelled"),20000);
  QCOMPARE(f.provider->uploads.load(),0);
  if(keep){const auto items=f.inspect(id)["items"].toArray();QCOMPARE(items.size(),1);QCOMPARE(items.first().toObject()["uploadStatus"].toString(),QString("local_only"));}
 }
 // A file from a file manager that changes after its check is never uploaded unchecked.
 void aFileChangedAfterItsCheckIsNotUploaded(){
  Fixture f(page(card));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto path=f.dir.filePath("receipt.png");QVERIFY(page(card).save(path,"PNG"));
  const auto started=call([&](JsonReply r){f.operations->uploadFiles({path},{},{},r,"report");});QVERIFY2(!started.error,qPrintable(started.error.code));
  const auto id=started.result["operationId"].toString();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  QVERIFY(page({"A different receipt 4111 1111 1111 1111"}).save(path,"PNG"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"upload",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("failed"),20000);
  QCOMPARE(f.inspect(id)["error"].toObject()["details"].toObject()["cause"].toString(),QString("file_changed"));
  QCOMPARE(f.provider->uploads.load(),0);
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
 // #1820: every kind on Redact automatically: the card is covered and the capture uploads with no decision.
 // With Keep a local copy on, the original is kept locally too, beside the covered copy.
 void everyKindOnRedactUploadsCoveredWithoutAsking(){
  Fixture f(page(card),true,true,everyKind);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();QVERIFY(!id.isEmpty());
  bool asked=false;
  const auto settled=[&]{const auto op=f.inspect(id);asked=asked||op["phase"]=="sensitive_text_found";return !Operation::activeState(op["state"].toString());};
  QVERIFY(putgd::test::waitFor(settled,40000));QCOMPARE(f.inspect(id)["state"].toString(),QString("completed"));
  QVERIFY(!asked);QCOMPARE(f.provider->uploads.load(),1);
  const auto sensitive=f.inspect(id)["sensitive"].toObject();
  QCOMPARE(sensitive["covered"].toObject()["card"].toInt(),1);QVERIFY(sensitive["complete"].toBool());QVERIFY(sensitive["found"].toObject().isEmpty());
  const auto image=uploaded(*f.operations,id);QVERIFY(!image.isNull());QVERIFY2(blackShare(image,digits(0))>0.7,qPrintable(QString::number(blackShare(image,digits(0)))));
  // The covered copy and the original, both kept locally.
  const auto items=f.inspect(id)["items"].toArray();QCOMPARE(items.first().toObject()["local"].toObject()["status"].toString(),QString("saved"));
  QVERIFY(blackShare(QImage(items.first().toObject()["local"].toObject()["path"].toString()),digits(0))>0.7);
  const auto original=sensitive["original"].toObject();QCOMPARE(original["status"].toString(),QString("saved"));QCOMPARE(original["paths"].toArray().size(),1);
  const QImage kept(original["paths"].toArray().first().toString());QVERIFY(!kept.isNull());QVERIFY(blackShare(kept,digits(0))<0.4);
  QVERIFY(!QJsonDocument(f.inspect(id)).toJson().contains("4111"));
 }
 // #1820: card on Redact automatically, key on Warn: the prompt asks about the key only, and Upload anyway keeps the card covered.
 void aMixedSettingAsksForTheWarnKindOnly(){
  Fixture f(page({card.first(),key.first()}),true,false,kinds({{"card","redact"},{"key","warn"}}));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  const auto sensitive=f.inspect(id)["sensitive"].toObject();
  QCOMPARE(sensitive["found"].toObject(),(QJsonObject{{"key",1}}));QCOMPARE(sensitive["covered"].toObject(),(QJsonObject{{"card",1}}));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"upload",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  QCOMPARE(f.provider->uploads.load(),1);
  const auto image=uploaded(*f.operations,id);
  QVERIFY2(blackShare(image,digits(0))>0.7,"the card stays covered");QVERIFY2(blackShare(image,digits(1))<0.4,"the key, on Warn, was uploaded as it is");
  QCOMPARE(f.inspect(id)["sensitive"].toObject()["covered"].toObject()["card"].toInt(),1);
 }
 // #1820: a word two kinds match goes to the kind covered automatically too: My redaction list on Redact still covers
 // "4111" though Card numbers, on Warn, claims the whole number when the kinds are matched together.
 void aRedactKindKeepsWordsAWarnKindAlsoMatches(){
  Fixture f(page(card),true,false,kinds({{"card","warn"},{"list","redact"}}),QJsonArray{"eng"},QJsonArray{"4111"});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  const auto sensitive=f.inspect(id)["sensitive"].toObject();
  QCOMPARE(sensitive["found"].toObject()["card"].toInt(),1);QCOMPARE(sensitive["covered"].toObject()["list"].toInt(),1);
 }
 // #1820: a Warn match inside text already covered automatically leaves nothing to ask about.
 void aWarnMatchInsideACoverDoesNotAsk(){
  Fixture f(page(card),true,false,kinds({{"card","redact"},{"list","warn"}}),QJsonArray{"eng"},QJsonArray{"4111"});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  bool asked=false;
  const auto settled=[&]{const auto op=f.inspect(id);asked=asked||op["phase"]=="sensitive_text_found";return !Operation::activeState(op["state"].toString());};
  QVERIFY(putgd::test::waitFor(settled,40000));QVERIFY(!asked);QCOMPARE(f.inspect(id)["state"].toString(),QString("completed"));
  QCOMPARE(f.inspect(id)["sensitive"].toObject()["covered"].toObject()["card"].toInt(),1);
 }
 // #1820: Warn kinds are matched one by one, so an IP match the covers take entirely can't hide the phone
 // number beside it, which the IP match would otherwise have claimed a word of first.
 void aCoveredWarnMatchHidesNoOtherWarnMatch(){
  Fixture f(page({"Host 192.0.2.1:202 555 0123"}),true,false,kinds({{"ip","warn"},{"phone","warn"},{"list","redact"}}),QJsonArray{"eng"},QJsonArray{"192.0.2.1:202"});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  const auto sensitive=f.inspect(id)["sensitive"].toObject();
  QVERIFY2(sensitive["found"].toObject()["phone"].toInt()>=1,QJsonDocument(sensitive).toJson(QJsonDocument::Compact).constData());
  QCOMPARE(sensitive["covered"].toObject()["list"].toInt(),1);
 }
 // #1820: a check that can't finish asks, even with every kind on Redact automatically; what was found is covered first.
 void anUnfinishedCheckAsksEvenWithEveryKindOnRedact(){
  // A second language with no data installed: the check can't read everything.
  Fixture f(page(card),true,false,everyKind,QJsonArray{"eng","zzz"});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  const auto sensitive=f.inspect(id)["sensitive"].toObject();
  QVERIFY(!sensitive["complete"].toBool());QCOMPARE(sensitive["reason"].toString(),QString("language_missing"));QCOMPARE(sensitive["covered"].toObject()["card"].toInt(),1);
  QCOMPARE(f.provider->uploads.load(),0);
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"upload",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  QVERIFY(blackShare(uploaded(*f.operations,id),digits(0))>0.7);
  // The notification never says what was covered after this.
  QVERIFY(!f.inspect(id)["sensitive"].toObject()["complete"].toBool());
 }
 // #1820: a file-manager selection covers each file on its own and uploads the covered copies in their places.
 void aSelectionUploadsEachFileCovered(){
  Fixture f(page({"nothing"}),true,false,everyKind);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto first=f.dir.filePath("receipt.png"),second=f.dir.filePath("notes.jpg");QVERIFY(page(card).save(first,"PNG"));QVERIFY(page(card).save(second,"JPG",95));
  const auto started=call([&](JsonReply r){f.operations->uploadFiles({first,second},{},{},r,"report");});QVERIFY2(!started.error,qPrintable(started.error.code));
  const auto id=started.result["operationId"].toString();
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),60000);
  QCOMPARE(f.provider->uploads.load(),2);QCOMPARE(f.inspect(id)["sensitive"].toObject()["covered"].toObject()["card"].toInt(),2);
  HistoryStore journal(f.operations->databasePath());const auto items=journal.operation(id).items;QCOMPARE(items.size(),2);
  for(const auto& item:items){QFile artifact(item.artifact.path);QVERIFY(artifact.open(QIODevice::ReadOnly));QVERIFY(blackShare(QImage::fromData(artifact.readAll()),digits(0))>0.7);}
  // The user's own files are untouched, and no covered copy is left behind.
  QVERIFY(blackShare(QImage(first),digits(0))<0.4);
  QVERIFY(QDir(f.dir.filePath("data/covered")).entryList(QDir::NoDotAndDotDot|QDir::AllEntries).isEmpty());
 }
};
QTEST_MAIN(SensitiveCheckTest)
#include "test_sensitive_check.moc"
