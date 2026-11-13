// The editor's Copy text (#1428): what recognition returns can never carry a
// word or code that a Redact mark covers, and a result for an old recognition
// revision is never accepted. EditorTextRecognition runs a fake in place of
// the putgd-ocr helper, on its real worker thread.
#include "fixtures/Waits.h"
#include "ui/EditorWindow.h"
#include <QJsonDocument>
#include <QtTest>
#include <atomic>
using namespace putgd;
namespace {
using editor::Shape;
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
bool done(const EditorTextRecognition& text) {return text.state()=="done";}
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
  QCOMPARE(text.identity(),quint64(0));QVERIFY(text.state()!="done");QVERIFY(text.candidates().isEmpty());
 }
};
QTEST_GUILESS_MAIN(EditorTextTest)
#include "test_editor_text.moc"
