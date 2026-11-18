// The editor's Copy text (#1428): what recognition returns can never carry a
// word or code that a Redact mark covers, and a result for an old recognition
// revision is never accepted. EditorTextRecognition runs a fake in place of
// the putgd-ocr helper, on its real worker thread.
#include "fixtures/Waits.h"
#include "editor/EditorCommands.h"
#include "ui/AccountName.h"
#include "ui/EditorWindow.h"
#include <QJsonDocument>
#include <QtTest>
#include <atomic>
#include <QSemaphore>
#include <QScopeGuard>
using namespace putgd;
namespace {
using editor::Shape;
// A rendered terminal page, as a helper would read it: an email address (every
// build matches it), the account's own login in a prompt and a home path, and a
// hostname from a saved list.
QList<TextWord> terminal(const QString& login) {
 return {{"jane@example.test",QRect(10,10,150,20),95},{login+"@d69:~$",QRect(10,40,140,20),95},{"/home/"+login+"/project",QRect(10,70,200,20),95},{"build-host-7",QRect(10,100,110,20),95},{"JIRA-4821",QRect(10,130,90,20),95}};
}
// What a helper would read in a 400 x 200 capture: a public word, a secret
// word and a QR code. The fake returns them whatever the pixels say, as a code
// that still decodes through its error correction would.
TextResult found() {
 TextResult result;result.text="public secret";result.lineTops={10};
 result.words={{"public",QRect(10,10,60,20),95},{"secret",QRect(200,10,60,20),95}};result.candidates=result.words;
 TextCode code;code.format="QRCode";code.text="https://example.com/secret";code.length=code.text.size();code.quad={QPoint(300,100),QPoint(340,100),QPoint(340,140),QPoint(300,140)};
 result.codes={code};return result;
}
QImage capture() {QImage image(400,200,QImage::Format_ARGB32);image.fill(Qt::white);return image;}
Shape redaction(const QRect& rect) {Shape shape;shape.kind=Shape::Kind::Redact;shape.rect=rect;return shape;}
bool done(const EditorTextRecognition& text) {return text.state()=="completed";}
// Nothing the result may show, copy or return mentions the covered word or code.
void verifyCovered(EditorTextRecognition& text) {
 text.layer().selectAll();
 const auto inspected=QString::fromUtf8(QJsonDocument(text.inspect()).toJson());
 for(const auto& visible:{text.layer().selectedText(),text.allText(),inspected})QVERIFY2(!visible.contains("secret"),qPrintable(visible));
 for(const auto& word:text.candidates())QVERIFY(word.text!="secret");
 QVERIFY(text.layer().codes().isEmpty());
 QVERIFY(text.layer().selectedText().contains("public"));
}
}
class EditorTextTest final:public QObject {
 Q_OBJECT
private slots:
 void quickRedactCannotChangeSubmittedDocument() {
  QSemaphore gate;std::atomic_bool started=false;
  editor::EditDocument doc(capture());EditorWindow window(doc);
  window.text().setRunner([&](const QImage&,const textjob::Options&,Error&,TextJob::Cancel){
   started=true;gate.acquire();auto result=found();result.candidates={{"jane@example.test",QRect(10,10,100,20),95}};return result;
  });
  const auto release=qScopeGuard([&]{gate.release();});
  Error error;bool answered=false;
  window.quickRedact({"email"},[&](Error e,QJsonObject){error=e;answered=true;});
  PUTGD_TRY_VERIFY(started.load());window.setBusy("Preparing upload…");
  gate.release();PUTGD_TRY_VERIFY(answered);
  QCOMPARE(error.code,QString("editor_finalizing"));QVERIFY(doc.shapes().isEmpty());
 }
 // Runs Quick redact on the terminal page in `window` and returns its answer.
 // #1618: name and list get the account, the saved list and the agent's terms and patterns.
 static QJsonObject run(EditorWindow& window,const QStringList& categories,QStringList terms={},QStringList patterns={}) {
  Error error;QJsonObject result;bool answered=false;
  window.quickRedact(categories,[&](Error e,QJsonObject r){error=e;result=r;answered=true;},terms,patterns);
  // A failed wait or a refusal fails the test and answers with nothing.
  if(!QTest::qVerify(putgd::test::waitFor([&]{return answered;}),"answered","Quick redact did not answer",__FILE__,__LINE__))return {};
  if(!QTest::qVerify(!error,"!error",qPrintable(error.code+": "+error.message),__FILE__,__LINE__))return {};
  return result;
 }
 static void useTerminal(EditorWindow& window) {
  const auto login=ui::currentAccount().login;
  window.text().setRunner([login](const QImage&,const textjob::Options&,Error&,TextJob::Cancel){auto result=found();result.candidates=terminal(login.isEmpty()?QStringLiteral("someone"):login);return result;});
 }
 // Each mark Quick redact adds names the category that suggested it, never the
 // text; moving or resizing, and an agent's update, keep it; a mark drawn by
 // hand has none; and an agent's add cannot set it.
 void suggestedMarkCarriesItsCategory() {
  editor::EditDocument doc(capture());EditorWindow window(doc);useTerminal(window);
  const auto hand=doc.add(redaction(QRect(300,150,30,30)));
  const auto result=run(window,{"email"});
  QCOMPARE(result["created"].toArray().size(),1);QCOMPARE(result["counts"].toObject()["email"].toInt(),1);
  const auto id=quint64(result["created"].toArray().first().toDouble());
  QCOMPARE(doc.find(id)->suggested,QString("email"));QVERIFY(doc.find(hand)->suggested.isEmpty());
  // Nothing of the matched text is kept on the shape.
  const auto kept=QString::fromUtf8(QJsonDocument(editor::Commands::state(doc)).toJson());QVERIFY(!kept.contains("jane"));QVERIFY(!kept.contains("example.test"));
  // editor.inspect: the category on the suggested mark, no field on the hand-drawn one.
  for(const auto& value:editor::Commands::state(doc)["shapes"].toArray()){const auto shape=value.toObject();
   if(quint64(shape["id"].toDouble())==id)QCOMPARE(shape["suggested"].toString(),QString("email"));else QVERIFY(!shape.contains("suggested"));}
  // Moved and resized by hand: the tag stays, so does the box it was suggested at.
  auto moved=*doc.find(id);const auto at=moved.suggestedAt;moved.rect=QRect(40,120,90,30);QVERIFY(doc.replace(moved));
  QCOMPARE(doc.find(id)->suggested,QString("email"));QCOMPARE(doc.find(id)->suggestedAt,at);
  // An agent's update keeps it; an add cannot set it, and neither can an update.
  QJsonArray created;QString code,message;
  QVERIFY(editor::Commands::apply(doc,QJsonArray{QJsonObject{{"op","update"},{"shapeId",double(id)},{"shape",QJsonObject{{"rect",QJsonArray{50,130,80,25}}}}}},created,code,message));
  QCOMPARE(doc.find(id)->suggested,QString("email"));QCOMPARE(doc.find(id)->rect,QRect(50,130,80,25));
  QVERIFY(!editor::Commands::apply(doc,QJsonArray{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{0,0,10,10}},{"suggested","email"}}}}},created,code,message));
  QCOMPARE(code,QString("edit_command_invalid"));
  QVERIFY(!editor::Commands::apply(doc,QJsonArray{QJsonObject{{"op","update"},{"shapeId",double(hand)},{"shape",QJsonObject{{"suggested","name"}}}}},created,code,message));
  QVERIFY(doc.find(hand)->suggested.isEmpty());
  // An agent's add draws a plain mark.
  QVERIFY(editor::Commands::apply(doc,QJsonArray{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{0,0,10,10}}}}}},created,code,message));
  QVERIFY(doc.find(quint64(created.first().toDouble()))->suggested.isEmpty());
 }
 // Deleting a suggested mark and running Quick redact again does not add it
 // back; undoing the delete brings the mark back; undoing the whole run and
 // running again adds it as new.
 void deletedSuggestionIsNotAddedBack() {
  editor::EditDocument doc(capture());EditorWindow window(doc);useTerminal(window);
  const auto first=run(window,{"email"});QCOMPARE(doc.shapes().size(),1);
  const auto id=quint64(first["created"].toArray().first().toDouble());const auto at=doc.find(id)->suggestedAt;
  // Even after it was moved: the memory is by where it was suggested.
  auto moved=*doc.find(id);moved.rect=moved.rect.translated(0,60);QVERIFY(doc.replace(moved));
  QVERIFY(doc.remove(id));QVERIFY(doc.shapes().isEmpty());QVERIFY(doc.dismissed(at,"email"));
  const auto again=run(window,{"email"});
  QCOMPARE(again["created"].toArray().size(),0);QCOMPARE(again["counts"].toObject()["email"].toInt(),0);QVERIFY(doc.shapes().isEmpty());
  // Undoing the delete: the mark is back and is no longer remembered as deleted.
  QVERIFY(doc.undo());QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().suggested,QString("email"));QVERIFY(!doc.dismissed(at,"email"));
  // Undoing the whole run is not a delete: the next run adds it again.
  QVERIFY(doc.undo());QVERIFY(doc.undo());QVERIFY(doc.shapes().isEmpty());QVERIFY(!doc.dismissed(at,"email"));
  const auto third=run(window,{"email"});QCOMPARE(third["created"].toArray().size(),1);
 }
 // Lab finding F1: a later run reads the page with the other marks blacked out, so the
 // recognized boxes move a few pixels and shrink a little. A deleted suggestion is still
 // skipped then, and it is the same only for the same category.
 void deletedSuggestionIsSkippedWhenTheNextRunsBoxMoves_data() {
  QTest::addColumn<QRect>("second");
  QTest::newRow("shifted 1 px")<<QRect(11,11,150,20);
  QTest::newRow("shifted 2 px and 1 px, a little shorter")<<QRect(12,11,148,17);
  QTest::newRow("shifted 5 px, shorter and narrower")<<QRect(15,15,144,16);
  QTest::newRow("shifted up and left")<<QRect(6,7,150,20);
 }
 void deletedSuggestionIsSkippedWhenTheNextRunsBoxMoves() {
  QFETCH(QRect,second);
  editor::EditDocument doc(capture());EditorWindow window(doc);
  std::atomic_int reads{0};
  window.text().setRunner([&](const QImage&,const textjob::Options&,Error&,TextJob::Cancel){
   auto result=found();result.candidates={{"jane@example.test",reads.fetch_add(1)==0?QRect(10,10,150,20):second,95}};return result;
  });
  const auto first=run(window,{"email"});QCOMPARE(doc.shapes().size(),1);
  const auto id=quint64(first["created"].toArray().first().toDouble());const auto firstBox=doc.find(id)->rect,at=doc.find(id)->suggestedAt;
  QVERIFY(doc.remove(id));
  const auto again=run(window,{"email"});
  QVERIFY2(reads.load()>=2,"the second run read the page again");
  QCOMPARE(again["created"].toArray().size(),0);QCOMPARE(again["counts"].toObject()["email"].toInt(),0);QVERIFY(doc.shapes().isEmpty());
  // Undo of the delete brings the deleted mark itself back, not a new one.
  QVERIFY(doc.undo());QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().id,id);QCOMPARE(doc.shapes().first().rect,firstBox);
  QVERIFY(doc.redo());QVERIFY(doc.shapes().isEmpty());QVERIFY(doc.dismissed(at,"email"));
 }
 // What counts as the same suggestion: the same category, and boxes that overlap by at least half their union.
 void dismissalMatchesByCategoryAndOverlap() {
  editor::EditDocument doc(capture());
  Shape mark=redaction(QRect(100,100,200,40));mark.suggested="email";mark.suggestedAt=mark.rect;
  const auto id=doc.add(mark);QVERIFY(doc.remove(id));
  QVERIFY(doc.dismissed(QRect(100,100,200,40),"email"));
  QVERIFY(doc.dismissed(QRect(103,101,196,34),"email"));
  QVERIFY(!doc.dismissed(QRect(103,101,196,34),"name")); // another category is another suggestion
  QVERIFY(!doc.dismissed(QRect(100,100,90,40),"email")); // a different, shorter match inside the line
  QVERIFY(!doc.dismissed(QRect(100,150,200,40),"email")); // the next line
  QVERIFY(!doc.dismissed(QRect(400,100,200,40),"email"));
 }
 // "Remove suggestions" takes back every suggestion still there as one undo
 // step, leaves marks drawn by hand alone, and does not count as deleting.
 void removeSuggestionsKeepsHandMarks() {
  editor::EditDocument doc(capture());EditorWindow window(doc);useTerminal(window);
  const auto hand=doc.add(redaction(QRect(300,150,30,30)));
  const auto first=run(window,{"email","name","list"},{"build-host-7"});
  const auto added=first["created"].toArray().size();QVERIFY(added>=1);QCOMPARE(doc.shapes().size(),1+added);
  QCOMPARE(window.removeSuggestions(),added);
  QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().id,hand);
  QCOMPARE(window.removeSuggestions(),0);
  // One undo step brings every suggestion back.
  QVERIFY(doc.undo());QCOMPARE(doc.shapes().size(),1+added);
  QVERIFY(doc.redo());QCOMPARE(doc.shapes().size(),1);
  // Not remembered as deleted: running again suggests them again.
  QCOMPARE(run(window,{"email"})["created"].toArray().size(),1);
 }
 // The two new categories reach the matcher with the account, the saved list
 // and an agent's terms and patterns, and answer with a count for each.
 void nameAndListAnswerWithCounts() {
  editor::EditDocument doc(capture());EditorWindow window(doc);useTerminal(window);
  window.setRedactList({"build-host-7"},{});
  // The runner reads the account on its worker (round 1, M9): the login is
  // found in the prompt and in the home path. A login under 3 characters
  // builds no form of its own (decision 3), so it finds nothing.
  const auto login=ui::currentAccount().login;
  const auto result=run(window,{"name","list"});
  const auto counts=result["counts"].toObject();
  QCOMPARE(counts["name"].toInt(),login.size()>=3?2:0);
  QCOMPARE(counts["list"].toInt(),1); // build-host-7, from the saved list
  for(const auto& shape:doc.shapes())QVERIFY(shape.suggested=="name"||shape.suggested=="list");
  // Words under a mark Quick redact already added are not read again, so a second run finds nothing new.
  QCOMPARE(run(window,{"name","list"})["created"].toArray().size(),0);
  // An agent's terms and patterns add to the saved list for that run only (a fresh edit, with the saved list in).
  editor::EditDocument other(capture());EditorWindow second(other);useTerminal(second);second.setRedactList({"build-host-7"},{});
  const auto agent=run(second,{"list"},{"extra-term","jane"},{"\\bJIRA-\\d+\\b"});
  QCOMPARE(agent["counts"].toObject()["list"].toInt(),3); // build-host-7, jane (in jane@example.test), JIRA-4821
  QVERIFY(!agent.contains("skippedPatterns"));
  QCOMPARE(second.removeSuggestions(),3);
  // Nothing agent-given stays: the next run sees only the saved list.
  QCOMPARE(run(second,{"list"})["counts"].toObject()["list"].toInt(),1);
  // list without anything saved or given finds nothing and is not an error.
  editor::EditDocument bare(capture());EditorWindow third(bare);useTerminal(third);
  QCOMPARE(run(third,{"list"})["created"].toArray().size(),0);
 }
 // redactText's own checks (#1618): 1 to 7 categories, optional terms and
 // patterns that follow Settings' rules and need list, errors by position.
 void redactTextChecksCategoriesTermsAndPatterns() {
  const auto check=[](const QJsonObject& entry,QStringList* categories=nullptr,QStringList* terms=nullptr,QStringList* patterns=nullptr){
   QStringList c,t,p;QString code,message;
   const bool ok=EditorWindow::parseRedactText(entry,c,t,p,code,message);
   if(categories)*categories=c;if(terms)*terms=t;if(patterns)*patterns=p;
   return ok?QString("ok"):code+": "+message;
  };
  const QJsonObject op{{"op","redactText"}};
  const auto with=[&](const QJsonObject& extra){auto entry=op;for(auto it=extra.begin();it!=extra.end();++it)entry[it.key()]=it.value();return entry;};
  QStringList categories,terms,patterns;
  QCOMPARE(check(with({{"categories",QJsonArray{"email","phone","ip","card","key","name","list"}}}),&categories),QString("ok"));QCOMPARE(categories.size(),7);
  QCOMPARE(check(with({{"categories",QJsonArray{"name"}}})),QString("ok"));
  QCOMPARE(check(with({{"categories",QJsonArray{"list"}},{"terms",QJsonArray{"Project Aurora","acme"}},{"patterns",QJsonArray{"\\bJIRA-\\d+\\b"}}}),&categories,&terms,&patterns),QString("ok"));
  QCOMPARE(terms,(QStringList{"Project Aurora","acme"}));QCOMPARE(patterns,(QStringList{"\\bJIRA-\\d+\\b"}));
  // Refused: the old limit's text is gone, and each says what to send.
  const auto refused=[&](const QJsonObject& entry,const QString& needle){const auto text=check(entry);QTest::qVerify(text.startsWith("edit_command_invalid: ")&&text.contains(needle),"refused",qPrintable(text),__FILE__,__LINE__);return text;};
  refused(with({{"categories",QJsonArray{}}}),"1 to 7");
  refused(with({{"categories",QJsonArray{"email","phone","ip","card","key","name","list","email"}}}),"1 to 7");
  refused(with({{"categories",QJsonArray{"username"}}}),"name and list");
  refused(with({{"categories",QJsonArray{"email","email"}}}),"1 to 7");
  refused(with({{"categories",QJsonArray{"list"}},{"extra",1}}),"terms and patterns");
  refused(with({{"categories",QJsonArray{"email"}},{"terms",QJsonArray{"x"}}}),"need the list category");
  refused(with({{"categories",QJsonArray{"email"}},{"patterns",QJsonArray{"x"}}}),"need the list category");
  refused(with({{"categories",QJsonArray{"list"}},{"terms",QJsonArray{}}}),"terms are 1 to 200");
  refused(with({{"categories",QJsonArray{"list"}},{"terms","word"}}),"terms are 1 to 200");
  refused(with({{"categories",QJsonArray{"list"}},{"patterns",QJsonArray{}}}),"patterns are 1 to 50");
  QJsonArray many;for(int i=0;i<201;++i)many.append("w"+QString::number(i));
  refused(with({{"categories",QJsonArray{"list"}},{"terms",many}}),"terms are 1 to 200");
  QJsonArray patternsMany;for(int i=0;i<51;++i)patternsMany.append("p"+QString::number(i));
  refused(with({{"categories",QJsonArray{"list"}},{"patterns",patternsMany}}),"patterns are 1 to 50");
  refused(with({{"categories",QJsonArray{"list"}},{"terms",QJsonArray{3}}}),"entry 0");
  // A term with a line break is refused by position, and its text is in no message.
  const auto term=refused(with({{"categories",QJsonArray{"list"}},{"terms",QJsonArray{"fine","Synthetic-Term-Not-Real\nsecond"}}}),"entry 1");
  QVERIFY(!term.contains("Synthetic-Term"));
  // A pattern that does not compile, or matches empty text, fails the whole command
  // naming its index and never its text.
  for(const auto& bad:{QString("(Synthetic-Pattern-Not-Real"),QString("["),QString("a*")}){
   const auto text=refused(with({{"categories",QJsonArray{"list"}},{"patterns",QJsonArray{"\\bok\\b",bad}}}),"entry 1");
   QVERIFY2(!text.contains("Synthetic-Pattern"),qPrintable(text));
  }
 }
 // Round 1 (U2): the list alone with nothing saved and nothing given answers at once with
 // created [] and a list count of 0, and starts no recognition, so it cannot fail without OCR.
 void emptyListAloneNeedsNoRecognition() {
  editor::EditDocument doc(capture());EditorWindow window(doc);
  std::atomic_bool started=false;
  window.text().setRunner([&](const QImage&,const textjob::Options&,Error& error,TextJob::Cancel){started=true;error={"engine_unavailable","No OCR here",{}};return TextResult{};});
  Error error{"pending","",{}};QJsonObject result;
  window.quickRedact({"list"},[&](Error e,QJsonObject r){error=e;result=r;});
  QVERIFY(!error);QCOMPARE(result["created"].toArray().size(),0);QCOMPARE(result["counts"].toObject(),(QJsonObject{{"list",0}}));
  PUTGD_VERIFY_STEADY(!started.load(),150);QVERIFY(doc.shapes().isEmpty());
  // With something to find, or another category, recognition is needed and its failure is reported.
  error={"pending","",{}};window.quickRedact({"list"},[&](Error e,QJsonObject){error=e;},{"build-host-7"});
  PUTGD_TRY_VERIFY(error.code!="pending");QCOMPARE(error.code,QString("engine_unavailable"));
 }
 // Round 1 (U5): a Redact mark added and then undone while matching runs is a new revision of
 // the image, even though the pixels read are the same again, so the result is not applied.
 void changeThenUndoWhileMatchingMakesTheResultStale() {
  editor::EditDocument doc(capture());EditorWindow window(doc);useTerminal(window);
  Error error{"pending","",{}};QJsonObject result;
  window.quickRedact({"email"},[&](Error e,QJsonObject r){error=e;result=r;});
  // Right after the recognition callback that starts the matching, in the same turn: the change and its undo.
  window.text().whenRecognized([&](Error){doc.add(redaction(QRect(300,150,30,30)));window.documentEdited();QVERIFY(doc.undo());window.documentEdited();});
  PUTGD_TRY_VERIFY(error.code!="pending");
  QCOMPARE(error.code,QString("edit_stale"));QVERIFY(doc.shapes().isEmpty());
 }
 // Round 1 (U1): a result that arrives while a mark is being dragged waits for the drag to end;
 // it is then its own undo step, so one Undo removes the suggestions and a second the drag.
 void resultDuringADragIsItsOwnUndoStep() {
  editor::EditDocument doc(capture());EditorWindow window(doc);useTerminal(window);
  Shape arrow;arrow.kind=Shape::Kind::Arrow;arrow.from=QPoint(300,100);arrow.to=QPoint(350,150);const auto id=doc.add(arrow);
  doc.beginGesture();auto moved=*doc.find(id);moved.from=QPoint(310,110);moved.to=QPoint(360,160);doc.preview(moved);
  Error error{"pending","",{}};QJsonObject result;
  window.quickRedact({"email"},[&](Error e,QJsonObject r){error=e;result=r;});
  PUTGD_COMPARE_STEADY(error.code,QString("pending"),400);
  QCOMPARE(doc.shapes().size(),1); // nothing joined the drag
  doc.commitGesture();
  PUTGD_TRY_VERIFY(error.code!="pending");QVERIFY(!error);QCOMPARE(doc.shapes().size(),2);
  QVERIFY(doc.undo());QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.find(id)->from,QPoint(310,110)); // the suggestions only
  QVERIFY(doc.undo());QCOMPARE(doc.find(id)->from,QPoint(300,100)); // then the drag
 }
 // Round 1 (U9): an agent removes the suggestions through editor.view, as one undo step, and
 // is told when there are none.
 void agentRemovesSuggestionsThroughTheView() {
  editor::EditDocument doc(capture());EditorWindow window(doc);useTerminal(window);
  const auto hand=doc.add(redaction(QRect(300,150,30,30)));
  QString code,message;
  window.viewCommand({{"action","removeSuggestions"}},code,message);
  QCOMPARE(code,QString("edit_shape_missing"));
  QCOMPARE(run(window,{"email"})["created"].toArray().size(),1);
  code.clear();window.viewCommand({{"action","removeSuggestions"}},code,message);
  QVERIFY2(code.isEmpty(),qPrintable(code+": "+message));QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().id,hand);
  QVERIFY(doc.undo());QCOMPARE(doc.shapes().size(),2);
  window.viewCommand({{"action","removeSuggestions"},{"shapeId",1}},code,message);QCOMPARE(code,QString("invalid_request"));
 }
 void wordUnderRedactMarkIsNeverReturned_data() {
  QTest::addColumn<QString>("change");
  QTest::newRow("mark added after recognition")<<"added";
  QTest::newRow("mark moved over the word")<<"moved";
  QTest::newRow("mark restored by redo")<<"redo";
 }
 // The covered word and the code under a mark are dropped from the result,
 // and a result from before the mark is never shown again.
 void wordUnderRedactMarkIsNeverReturned() {
  QFETCH(QString,change);
  editor::EditDocument doc(capture());EditorTextRecognition text(doc);QImage input;
  text.setRunner([&input](const QImage& grey,const textjob::Options&,Error&,TextJob::Cancel){input=grey;return found();});
  quint64 mark=0;
  if(change=="moved")mark=doc.add(redaction(QRect(100,150,20,20)));
  if(change=="redo"){mark=doc.add(redaction(QRect(195,5,140,140)));QVERIFY(doc.undo());}
  text.documentChanged();text.start();PUTGD_TRY_VERIFY(done(text));
  QVERIFY(text.layer().words().size()==2);
  const auto before=text.identity();
  if(change=="added")mark=doc.add(redaction(QRect(195,5,140,140)));
  if(change=="moved"){auto moved=*doc.find(mark);moved.rect=QRect(195,5,140,140);QVERIFY(doc.replace(moved));}
  if(change=="redo")QVERIFY(doc.redo());
  text.documentChanged();
  // The old result is gone at once: nothing to select, copy or inspect.
  QCOMPARE(text.identity(),quint64(0));QVERIFY(text.layer().words().isEmpty());QVERIFY(text.layer().codes().isEmpty());
  text.start();PUTGD_TRY_VERIFY(done(text));
  QVERIFY(text.identity()!=before);
  verifyCovered(text);
  // And the helper never saw the covered pixels: they are black in its input.
  QCOMPARE(qGray(input.pixel(230,20)),0);QCOMPARE(qGray(input.pixel(20,20)),255);
 }
 // A job that ends after a Redact mark changed is discarded with everything it
 // found, and a caller waiting on it hears edit_stale.
 void lateResultForOldRevisionIsRejected() {
  editor::EditDocument doc(capture());EditorTextRecognition text(doc);
  std::atomic_bool release{false},started{false},returned{false};
  text.setRunner([&](const QImage&,const textjob::Options&,Error&,TextJob::Cancel){
   started=true;while(!release.load())QThread::msleep(2);
   returned=true;return found(); // ignores the cancel, as a helper that already finished would
  });
  Error waited{"pending","",{}};text.whenRecognized([&waited](Error error){waited=error;});
  PUTGD_TRY_VERIFY(started.load());
  doc.add(redaction(QRect(300,180,10,10)));text.documentChanged();
  QCOMPARE(waited.code,QString("edit_stale"));
  release=true;PUTGD_TRY_VERIFY(returned.load());
  // The late result arrives and is rejected: nothing is shown or kept.
  PUTGD_COMPARE_STEADY(text.layer().words().size(),qsizetype(0),200);
  QCOMPARE(text.identity(),quint64(0));QVERIFY(text.state()!="completed");QVERIFY(text.candidates().isEmpty());
 }
};
QTEST_MAIN(EditorTextTest)
#include "test_editor_text.moc"
