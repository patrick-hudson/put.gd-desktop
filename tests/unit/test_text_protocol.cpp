// The text-job protocol library (#1428): the two pure filters between what a
// screen or a caller supplies and what put.gd copies, shows or passes on.
// Links Qt Core and the library only, as the putgd-ocr helper does.
#include "core/TextJobProtocol.h"
#include <QtTest>

using namespace putgd;
using namespace putgd::textjob;

class TextProtocolTest:public QObject {
 Q_OBJECT
private slots:
 // Decision 4a. Recognized text and code content come from the screen: a
 // control, bidirectional or invisible character that got through could steer
 // a terminal it is pasted into, or hide what a link really says.
 void controlAndBidirectionalCharactersAreRemoved_data() {
  QTest::addColumn<QString>("input");QTest::addColumn<QString>("text");QTest::addColumn<QString>("code");
  const auto row=[](const char* name,const QString& input,const QString& text,const QString& code={}){QTest::newRow(name)<<input<<text<<(code.isNull()?text:code);};
  row("escape sequence and carriage return",QString::fromLatin1("a\x1b[31mb\rc"),"a[31mbc");
  row("tab and newline stay",QString("a\tb\nc"),"a\tb\nc");
  row("delete and C1",QString("a")+QChar(0x7F)+QChar(0x85)+QChar(0x9B)+"b","ab");
  row("right-to-left override",QString("gpj.")+QChar(0x202E)+"exe","gpj.exe");
  row("embeddings and isolates",QString("a")+QChar(0x202A)+QChar(0x202B)+QChar(0x202C)+QChar(0x2066)+QChar(0x2067)+QChar(0x2068)+QChar(0x2069)+"b","ab");
  row("marks and zero-width space",QString("a")+QChar(0x200E)+QChar(0x200F)+QChar(0x200B)+QChar(0x2060)+QChar(0xFEFF)+QChar(0x00AD)+"b","ab");
  row("tag characters",QString("a")+QString::fromUcs4(U"\U000E0001\U000E0041\U000E007F")+"b","ab");
  row("line and paragraph separators",QString("a")+QChar(0x2028)+"b"+QChar(0x2029)+"c","a\nb\nc");
  row("decomposed becomes composed",QString("e")+QChar(0x0301),QString(QChar(0x00E9)));
  // Persian and Indic text needs the joiners; a code's content keeps no format character.
  row("joiners",QString("a")+QChar(0x200C)+QChar(0x200D)+"b",QString("a")+QChar(0x200C)+QChar(0x200D)+"b","ab");
 }
 void controlAndBidirectionalCharactersAreRemoved() {
  QFETCH(QString,input);QFETCH(QString,text);QFETCH(QString,code);
  QCOMPARE(sanitizeText(input),text);QCOMPARE(sanitizeCodeContent(input),code);
  // The parent's decoder applies it: nothing a job returns skips the filter.
  TextResult result;result.text=input;result.lineTops={0};TextCode qr;qr.format="QRCode";qr.text=input;qr.quad={{0,0},{8,0},{8,8},{0,8}};result.codes={qr};
  Options options;options.languages={"eng"};TextResult decoded;
  QVERIFY(decodeResult(encodeResult(result,options),{64,64,64},options,decoded));
  QCOMPARE(decoded.text,text);QCOMPARE(decoded.codes.first().text,code);
 }
 // Decision 6. A language name becomes a file the helper opens: one that is a
 // path, an engine file or an option would let a caller choose what it reads.
 void languageNamesAreIdentifiersOnly() {
  for(const auto* valid:{"eng","deu","chi_sim","chi_tra_vert","frak"})QVERIFY2(validLanguage(valid),valid);
  for(const auto* bad:{"","en","ENG","eng.traineddata","../eng","/usr/share/eng","eng deu","eng+deu","osd","equ","eng_","eng_a_b_c","-c","eng\n"})QVERIFY2(!validLanguage(bad),bad);
  QVERIFY(validLanguages({"eng","deu","fra"}));
  QVERIFY(!validLanguages({}));QVERIFY(!validLanguages({"eng","deu","fra","spa"}));QVERIFY(!validLanguages({"eng","eng"}));QVERIFY(!validLanguages({"eng","../deu"}));
 }
};
QTEST_APPLESS_MAIN(TextProtocolTest)
#include "test_text_protocol.moc"
