// #1804: Check captures for sensitive text before uploading, through the
// resident's operation service, the real editor helper and the real putgd-ocr.
// Only test values: Visa's documented test number and AWS's documented example key.
#include "fixtures/U2Fixtures.h"
#include "core/SanitizedPng.h"
#include "core/SensitiveText.h"
#include "core/AllocatorPolicy.h"
#include "core/QrImage.h"
#include "core/TextJob.h"
#include <QDirIterator>
#include "fixtures/Waits.h"
#include <QCoreApplication>
#include <algorithm>
#include <QFileInfo>
#include <QFont>
#include <QPainter>
#include <QRegularExpression>
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
 Fixture(QImage image,bool check=true,bool keepLocal=false,QJsonObject checkObject={},QJsonArray languages={"eng"},QJsonArray terms={},QJsonObject style={}){
  registry.add("opaque",provider);
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=keepLocal;doc["archive"]=archive;
  auto preferences=doc["preferences"].toObject();preferences["sensitiveTextCheck"]=checkObject.isEmpty()?QJsonObject{{"enabled",check},{"categories",QJsonArray{"card","key"}}}:checkObject;preferences["textRecognitionLanguages"]=languages;if(!terms.isEmpty())preferences["quickRedactTerms"]=terms;if(!style.isEmpty())preferences["captureStyle"]=style;doc["preferences"]=preferences;
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
// The kept original as the operation should name it: one PNG saved in the fixture's archive, named after the
// capture with -original, and nothing else.
QJsonObject keptOriginal(const Fixture& f,const QJsonObject& sensitive){
 const auto path=sensitive["original"].toObject()["paths"].toArray().first().toString();
 const QFileInfo file(path);
 if(!path.startsWith(f.dir.filePath("archive/images/"))||!file.isFile()||!file.fileName().endsWith("-original.png"))return {};
 return {{"paths",QJsonArray{path}},{"status","saved"}};
}
// The operation's JSON without the values put.gd generates for it: the fixture's folder, and each id, revision,
// hash and token, taken from the field that holds it and only when it has that value's shape, so recognized text
// can't hide in one; and the PNG's measured size. They are random hex or numbers and hold 4111 by chance (#1889).
QString withoutGeneratedValues(const QJsonObject& operation,const QString& root){
 static const QRegularExpression uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"),sha("^[0-9a-f]{64}$");
 QStringList values{root};
 const auto take=[&](const QJsonValue& value,const QRegularExpression& shape,const QString& prefix=QString()){
  const auto text=value.toString();if(text.startsWith(prefix)&&shape.match(text.mid(prefix.size())).hasMatch())values.append(text);};
 const auto destination=[&](const QJsonValue& value){take(value.toObject()["profileRevision"],sha);};
 const auto reference=[&](const QJsonValue& value){const auto o=value.toObject();take(o["fileId"],sha,QStringLiteral("opaque:α/%?#:"));take(o["revisionId"],uuid,"opaque-version!");};
 auto copy=operation;take(copy["id"],uuid);take(copy["settingsRevision"],sha);destination(copy["destination"]);
 auto items=copy["items"].toArray();
 for(qsizetype i=0;i<items.size();++i){
  auto item=items[i].toObject();auto artifact=item["artifact"].toObject();const auto receipt=item["receipt"].toObject();
  take(artifact["id"],uuid);take(artifact["sha256"],sha);take(item["publicationId"],uuid);take(item["concurrencyToken"],uuid,"opaque-token!");reference(item["objectReference"]);
  take(receipt["publicationId"],uuid);take(receipt["concurrencyToken"],uuid,"opaque-token!");reference(receipt["objectReference"]);destination(receipt["destination"]);
  artifact.remove("bytes");item["artifact"]=artifact;items[i]=item;
 }
 copy["items"]=items;
 auto json=QString::fromUtf8(QJsonDocument(copy).toJson(QJsonDocument::Compact));
 // Longest first: an id inside a token goes with the token.
 std::sort(values.begin(),values.end(),[](const QString& a,const QString& b){return a.size()>b.size();});
 for(const auto& value:std::as_const(values))if(!value.isEmpty())json.replace(value,QString());
 return json;
}
// The first piece of the fixture's text left in the operation once the generated values are out: either digit group
// of the card, alone or in any form, the line's word, or a piece of the example key. Empty when there is none.
QString fixtureTextIn(const QJsonObject& operation,const QString& root){
 const auto json=withoutGeneratedValues(operation,root);
 for(const auto* piece:{"4111","1111","Card","AKIA","IOSFODNN","EXAMPLE"})if(json.contains(QLatin1String(piece)))return QString::fromLatin1(piece)+QStringLiteral(" in ")+json;
 return {};
}
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
  // Only the kinds and counts: nothing recognized reaches the operation, in a field of its own or in any other.
  QCOMPARE(f.inspect(id)["sensitive"].toObject(),(QJsonObject{{"found",QJsonObject{{"card",1}}},{"complete",true},{"prompt",1}}));
  {const auto leak=fixtureTextIn(f.inspect(id),f.dir.path());QVERIFY2(leak.isEmpty(),qPrintable(leak));}
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
 // Open in editor opens the editor with the suggestions placed; its PNG is what uploads.
 void openInEditorPlacesTheSuggestions(){
  Fixture f(page(card));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();
  QVERIFY(f.reaches(id,"sensitive_text_found"));
  QCOMPARE(call([&](JsonReply r){f.operations->editorCommand(id,"inspect",{},r);}).error.code,QString("editor_unavailable"));
  // #1898: the choice that used to open the editor has a name of its own now.
  QCOMPARE(call([&](JsonReply r){f.operations->decideSensitive(id,"redact",r);}).error.code,QString("invalid_request"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"edit",r);}).error);
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
 // #1893 with #1898: Redact and upload with Capture style on covers first, then styles: the covers sit on the digits inside
 // the frame. Open in editor starts styled, and its export is not styled a second time.
 void redactAndUploadWithCaptureStyleCoversInsideTheFrame(){
  const auto style=CaptureStyle::preset("paper");const auto image=page({card.first(),key.first()});
  Fixture f(image,true,true,kinds({{"card","warn"},{"key","warn"}}),{"eng"},{},style.toJson());PUTGD_TRY_VERIFY(f.operations->ready());
  const auto geometry=style.geometry(image.size());
  const auto id=f.start();QVERIFY(f.reaches(id,"sensitive_text_found"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"redact_upload",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  const auto styled=uploaded(*f.operations,id);QCOMPARE(styled.size(),geometry.canvas);QVERIFY(f.inspect(id)["style"].toObject()["applied"].toBool());
  for(int row=0;row<2;++row)QVERIFY2(blackShare(styled,digits(row).translated(geometry.padding,geometry.padding))>0.7,"covered inside the frame");
  // The kept original is the capture before covering and before styling.
  const QImage kept(f.inspect(id)["sensitive"].toObject()["original"].toObject()["paths"].toArray().first().toString());QCOMPARE(kept.size(),image.size());
  const auto edited=f.start();QVERIFY(f.reaches(edited,"sensitive_text_found"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(edited,"edit",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(edited)["phase"].toString(),QString("editing"),20000);
  QVERIFY(!call([&](JsonReply r){f.operations->editorCommand(edited,"submit",{},r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(edited)["state"].toString(),QString("completed"),20000);
  QCOMPARE(uploaded(*f.operations,edited).size(),geometry.canvas);QVERIFY(!f.inspect(edited).contains("style"));
 }
 // #1898: Redact and upload covers exactly what the prompt's preview shows, the suggestions Open in editor places,
 // and uploads with no editor step; the notification's counts name what was covered.
 void redactAndUploadSendsThePreviewedCoversInOneStep(){
  Fixture f(page({card.first(),key.first()}),true,true,kinds({{"card","redact"},{"key","warn"}}));PUTGD_TRY_VERIFY(f.operations->ready());
  // The reference: the same capture through Open in editor, submitted untouched.
  const auto edited=f.start();QVERIFY(f.reaches(edited,"sensitive_text_found"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(edited,"edit",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(edited)["phase"].toString(),QString("editing"),20000);
  QVERIFY(!call([&](JsonReply r){f.operations->editorCommand(edited,"submit",{},r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(edited)["state"].toString(),QString("completed"),20000);
  const auto id=f.start();QVERIFY(f.reaches(id,"sensitive_text_found"));
  QCOMPARE(f.inspect(id)["sensitive"].toObject()["found"].toObject(),(QJsonObject{{"key",1}}));
  bool editing=false;
  const auto decided=call([&](JsonReply r){f.operations->decideSensitive(id,"redact_upload",r);});QVERIFY2(!decided.error,qPrintable(decided.error.code));
  QCOMPARE(decided.result["decided"].toString(),QString("redact_upload"));
  QVERIFY(putgd::test::waitFor([&]{const auto op=f.inspect(id);editing=editing||op["phase"]=="editing";return !Operation::activeState(op["state"].toString());},20000));
  QCOMPARE(f.inspect(id)["state"].toString(),QString("completed"));QVERIFY2(!editing,"no editor step");QCOMPARE(f.provider->uploads.load(),2);
  const auto image=uploaded(*f.operations,id);
  QVERIFY2(blackShare(image,digits(0))>0.7,"the card, on Redact automatically, is covered");QVERIFY2(blackShare(image,digits(1))>0.7,"the key, on Warn, is covered too");
  QCOMPARE(image.convertToFormat(QImage::Format_RGB32),uploaded(*f.operations,edited).convertToFormat(QImage::Format_RGB32));
  const auto sensitive=f.inspect(id)["sensitive"].toObject();
  QCOMPARE(sensitive["covered"].toObject(),(QJsonObject{{"card",1},{"key",1}}));QVERIFY(sensitive["complete"].toBool());
  QCOMPARE(coveredSentence(sensitive["covered"].toObject()),QString("Covered 1 card number and 1 key or token."));
  // With Keep a local copy on, the uncovered original is kept locally, as automatic covers keep it.
  const auto original=sensitive["original"].toObject();QCOMPARE(original["status"].toString(),QString("saved"));QCOMPARE(original["paths"].toArray().size(),1);
  const QImage kept(original["paths"].toArray().first().toString());QVERIFY(blackShare(kept,digits(0))<0.4);QVERIFY(blackShare(kept,digits(1))<0.4);
  QCOMPARE(f.inspect(id)["sensitive"].toObject(),(QJsonObject{{"found",QJsonObject{{"key",1}}},{"covered",QJsonObject{{"card",1},{"key",1}}},{"complete",true},{"original",keptOriginal(f,sensitive)}}));
  {const auto leak=fixtureTextIn(f.inspect(id),f.dir.path());QVERIFY2(leak.isEmpty(),qPrintable(leak));}
 }
 // #1898: Redact and upload covers each file of a selection that asks; the user's own files stay as they are.
 void redactAndUploadCoversEachFileOfASelection(){
  Fixture f(page({"nothing"}));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto first=f.dir.filePath("receipt.png"),second=f.dir.filePath("notes.png");QVERIFY(page(card).save(first,"PNG"));QVERIFY(page(key).save(second,"PNG"));
  const auto started=call([&](JsonReply r){f.operations->uploadFiles({first,second},{},{},r,"report");});QVERIFY2(!started.error,qPrintable(started.error.code));
  const auto id=started.result["operationId"].toString();
  for(int prompt=1;prompt<=2;++prompt){
   QVERIFY(putgd::test::waitFor([&]{return f.inspect(id)["sensitive"].toObject()["prompt"].toInt()==prompt;},40000));
   // A selection never opens the editor.
   QCOMPARE(call([&](JsonReply r){f.operations->decideSensitive(id,"edit",r,prompt);}).error.code,QString("sensitive_choice_unavailable"));
   QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"redact_upload",r,prompt);}).error);
  }
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),40000);
  QCOMPARE(f.provider->uploads.load(),2);QCOMPARE(f.inspect(id)["sensitive"].toObject()["covered"].toObject(),(QJsonObject{{"card",1},{"key",1}}));
  HistoryStore journal(f.operations->databasePath());for(const auto& item:journal.operation(id).items){QFile artifact(item.artifact.path);QVERIFY(artifact.open(QIODevice::ReadOnly));QVERIFY(blackShare(QImage::fromData(artifact.readAll()),digits(0))>0.7);}
  QVERIFY(blackShare(QImage(first),digits(0))<0.4);QVERIFY(blackShare(QImage(second),digits(0))<0.4);
 }
 // #1898: a file chosen twice is answered at each place: Redact and upload at the first never becomes the second's
 // Upload anyway, which keeps only the automatic cover, nor the reverse.
 void aFileChosenTwiceKeepsEachPlacesAnswer(){
  Fixture f(page({"nothing"}),true,false,kinds({{"card","redact"},{"key","warn"}}));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto path=f.dir.filePath("receipt.png");QVERIFY(page({card.first(),key.first()}).save(path,"PNG"));
  const auto started=call([&](JsonReply r){f.operations->uploadFiles({path,path},{},{},r,"report");});QVERIFY2(!started.error,qPrintable(started.error.code));
  const auto id=started.result["operationId"].toString();
  const QStringList choices{"redact_upload","upload"};
  for(int prompt=1;prompt<=2;++prompt){
   QVERIFY(putgd::test::waitFor([&]{return f.inspect(id)["sensitive"].toObject()["prompt"].toInt()==prompt;},40000));
   QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,choices[prompt-1],r,prompt);}).error);
  }
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),40000);
  HistoryStore journal(f.operations->databasePath());const auto items=journal.operation(id).items;QCOMPARE(items.size(),2);
  QList<double> keys;for(const auto& item:items){QFile artifact(item.artifact.path);QVERIFY(artifact.open(QIODevice::ReadOnly));const auto image=QImage::fromData(artifact.readAll());QVERIFY(blackShare(image,digits(0))>0.7);keys.append(blackShare(image,digits(1)));}
  std::sort(keys.begin(),keys.end());QVERIFY2(keys[0]<0.4&&keys[1]>0.7,"one place covered the key, the other left it as chosen");
 }
 // #1898: a file chosen twice that changes between its two checks never uploads unchecked at the place left uncovered.
 void aFileChosenTwiceThatChangesBetweenChecksIsNotUploaded(){
  Fixture f(page({"nothing"}));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto path=f.dir.filePath("receipt.png");QVERIFY(page({card.first(),key.first()}).save(path,"PNG"));
  const auto started=call([&](JsonReply r){f.operations->uploadFiles({path,path},{},{},r,"report");});QVERIFY2(!started.error,qPrintable(started.error.code));
  const auto id=started.result["operationId"].toString();
  QVERIFY(putgd::test::waitFor([&]{return f.inspect(id)["sensitive"].toObject()["prompt"].toInt()==1;},40000));
  QVERIFY(page({card.first(),key.first(),"changed"}).save(path,"PNG"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"upload",r,1);}).error);
  QVERIFY(putgd::test::waitFor([&]{return f.inspect(id)["sensitive"].toObject()["prompt"].toInt()==2;},40000));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"redact_upload",r,2);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("failed"),40000);
  QCOMPARE(f.inspect(id)["error"].toObject()["details"].toObject()["cause"].toString(),QString("file_changed"));
  QCOMPARE(f.provider->uploads.load(),0);
 }
 // #1898: a check that couldn't finish and found nothing has nothing to cover: Redact and upload is refused.
 void redactAndUploadNeedsSomethingFound(){
  Fixture f(page({"Meeting notes for Tuesday"}),true,false,{},QJsonArray{"eng","zzz"});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();QVERIFY(f.reaches(id,"sensitive_text_found"));
  QVERIFY(!f.inspect(id)["sensitive"].toObject()["complete"].toBool());
  QCOMPARE(call([&](JsonReply r){f.operations->decideSensitive(id,"redact_upload",r);}).error.code,QString("sensitive_choice_unavailable"));
  QVERIFY(!call([&](JsonReply r){f.operations->decideSensitive(id,"cancel",r);}).error);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("cancelled"),20000);QCOMPARE(f.provider->uploads.load(),0);
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
  QCOMPARE(f.inspect(id)["sensitive"].toObject(),(QJsonObject{{"found",QJsonObject{}},{"covered",QJsonObject{{"card",1}}},{"complete",true},{"original",keptOriginal(f,sensitive)}}));
  {const auto leak=fixtureTextIn(f.inspect(id),f.dir.path());QVERIFY2(leak.isEmpty(),qPrintable(leak));}
 }
 // #1941: a QR code holding a 2FA secret or a Wi-Fi password is covered with Keys and tokens before the upload: one
 // solid cover over the whole symbol, so the real putgd-ocr finds no code in what was uploaded (it reads the one in
 // the original). The code's content is used only to decide: it reaches no operation record, History, file or
 // notification line, and only its kind and count are reported.
 void aSecretQrCodeIsCoveredBeforeUpload_data(){
  QTest::addColumn<QByteArray>("content");QTest::addColumn<QByteArray>("secret");QTest::addColumn<bool>("warn");
  const QByteArray seed=QByteArray("JBSWY3DP")+"EHPK3PXP"+"KZ4TQ7MA";
  const QByteArray otp="otpauth://totp/Example:alice@example.com?secret="+seed+"&issuer=Example";
  QTest::newRow("2FA enrolment, covered automatically")<<otp<<seed<<false;
  const QByteArray password=QByteArray("orchard")+"-Lantern-"+"4271";
  QTest::newRow("Wi-Fi network, covered automatically")<<("WIFI:T:WPA;S:HomeNet;P:"+password+";;")<<password<<false;
  // Keys and tokens on Warn beside a card on Redact automatically: Redact and upload covers both (Sol on #1952).
  QTest::newRow("2FA enrolment, Redact and upload")<<otp<<seed<<true;
 }
 void aSecretQrCodeIsCoveredBeforeUpload(){
  QFETCH(QByteArray,content);QFETCH(QByteArray,secret);QFETCH(bool,warn);
  qr::Matrix matrix;QVERIFY(!qr::encode(content,matrix));
  const int scale=6;const auto code=qr::image(matrix,scale);
  // page()'s text, then the code below it.
  auto capture=page({warn?card.first():QStringLiteral("Scan to connect")});
  capture=capture.copy(0,0,capture.width(),capture.height()+code.height()+40);
  {QPainter painter(&capture);painter.fillRect(0,150,capture.width(),capture.height()-150,Qt::white);painter.drawImage(40,150,code);}
  const QRect symbol(40+qr::quietZone*scale,150+qr::quietZone*scale,matrix.size*scale,matrix.size*scale);
  Fixture f(capture,true,true,warn?kinds({{"key","warn"},{"card","redact"}}):kinds({{"key","redact"}}));PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();QVERIFY(!id.isEmpty());
  if(warn){
   QVERIFY(f.reaches(id,"sensitive_text_found",60000));
   const auto found=f.inspect(id)["sensitive"].toObject();
   QCOMPARE(found["found"].toObject(),(QJsonObject{{"key",1}}));QCOMPARE(found["covered"].toObject(),(QJsonObject{{"card",1}}));
   const auto decided=call([&](JsonReply r){f.operations->decideSensitive(id,"redact_upload",r);});QVERIFY2(!decided.error,qPrintable(decided.error.code));
   QVERIFY(putgd::test::waitFor([&]{return !Operation::activeState(f.inspect(id)["state"].toString());},30000));
  }else{
   bool asked=false;
   QVERIFY(putgd::test::waitFor([&]{const auto op=f.inspect(id);asked=asked||op["phase"]=="sensitive_text_found";return !Operation::activeState(op["state"].toString());},60000));
   QVERIFY(!asked);
  }
  QCOMPARE(f.inspect(id)["state"].toString(),QString("completed"));QCOMPARE(f.provider->uploads.load(),1);
  const auto sensitive=f.inspect(id)["sensitive"].toObject();
  QCOMPARE(sensitive["covered"].toObject(),warn?(QJsonObject{{"card",1},{"key",1}}):(QJsonObject{{"key",1}}));
  QCOMPARE(coveredSentence(sensitive["covered"].toObject()),warn?QString("Covered 1 card number and 1 key or token."):QString("Covered 1 key or token."));
  // Solid over the whole symbol, finder patterns and all, and past it.
  const auto image=uploaded(*f.operations,id);QVERIFY(!image.isNull());
  QCOMPARE(blackShare(image,symbol.adjusted(-scale,-scale,scale,scale)),1.0);
  // Read back with the real decoder: the original holds the code, the upload holds none.
  TextJob job(QCoreApplication::applicationDirPath()+"/putgd-ocr");
  QStringList data;for(const auto& d:{QStringLiteral("/usr/share/tesseract-ocr/5/tessdata"),QStringLiteral("/usr/share/tesseract/tessdata"),QStringLiteral("/usr/share/tessdata")})if(QFileInfo::exists(d+"/eng.traineddata"))data.append(d);
  if(data.isEmpty())QSKIP("English language data is not installed");
  job.setDataDirectories(data);TextJob::Options options;options.languages={"eng"};
  Error error;const auto before=job.recognize(capture,options,error);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(before.codes.size(),1);QCOMPARE(before.codes.first().text.toUtf8(),content);
  const auto after=job.recognize(image,options,error);QVERIFY2(!error,qPrintable(error.code));QVERIFY(after.codes.isEmpty());
  // Nothing of the content anywhere the resident writes or answers: the operation, and every file under its data
  // and configuration (History's database and journal, logs, the local copies' names and metadata).
  const auto record=QJsonDocument(f.inspect(id)).toJson();
  for(const QByteArray& part:{secret,QByteArray("otpauth"),QByteArray("WIFI:")})QVERIFY2(!record.contains(part),record.constData());
  for(QDirIterator it(f.dir.path(),QDir::Files|QDir::Hidden,QDirIterator::Subdirectories);it.hasNext();){
   QFile file(it.next());if(!file.open(QIODevice::ReadOnly))continue;
   const auto bytes=file.readAll();QVERIFY2(!bytes.contains(secret),qPrintable(it.filePath()));QVERIFY2(!it.fileName().contains(QString::fromLatin1(secret)),qPrintable(it.filePath()));
  }
 }
 // #1893: with Capture style on too, the check reads the capture as it was and the style goes around the covered image:
 // the cover sits on the digits, offset by the padding, and the original kept is the one from before covering.
 void aCoveredCaptureIsStyledAfterCovering(){
  const auto style=CaptureStyle::preset("paper");const auto image=page(card);
  Fixture f(image,true,true,everyKind,{"eng"},{},style.toJson());PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.start();QVERIFY(!id.isEmpty());
  QVERIFY(putgd::test::waitFor([&]{return !Operation::activeState(f.inspect(id)["state"].toString());},40000));QCOMPARE(f.inspect(id)["state"].toString(),QString("completed"));
  const auto op=f.inspect(id);QCOMPARE(op["sensitive"].toObject()["covered"].toObject()["card"].toInt(),1);QVERIFY(op["style"].toObject()["applied"].toBool());
  const auto styled=uploaded(*f.operations,id);const auto geometry=style.geometry(image.size());
  QCOMPARE(styled.size(),geometry.canvas);
  QVERIFY2(blackShare(styled,digits(0).translated(geometry.padding,geometry.padding))>0.7,"the card is covered under the style");
  const auto original=op["sensitive"].toObject()["original"].toObject();QCOMPARE(original["status"].toString(),QString("saved"));
  const QImage kept(original["paths"].toArray().first().toString());QCOMPARE(kept.size(),image.size());
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
  // No zero in the shared word "…:212": where DejaVu Sans Mono is missing (Fedora's toolbox), the fallback
  // Noto Sans Mono draws a zero recognition can read as "@", and "202" came back "2@2".
  Fixture f(page({"Host 192.0.2.1:212 555 1234"}),true,false,kinds({{"ip","warn"},{"phone","warn"},{"list","redact"}}),QJsonArray{"eng"},QJsonArray{"192.0.2.1:212"});PUTGD_TRY_VERIFY(f.operations->ready());
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
