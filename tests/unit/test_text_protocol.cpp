// The text-job protocol library (#1428): the two pure filters between what a
// screen or a caller supplies and what put.gd copies, shows or passes on.
// Links Qt Core and the library only, as the putgd-ocr helper does.
#include "core/TextJobProtocol.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
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
  // #1748: the flags of England, Scotland and Wales are tag sequences, and only
  // they keep their tags; any other tag sequence loses them.
  row("England's flag",QString::fromUcs4(U"a\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007Fb"),
      QString::fromUcs4(U"a\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007Fb"),QString::fromUcs4(U"a\U0001F3F4b"));
  row("another tag sequence",QString::fromUcs4(U"a\U0001F3F4\U000E0075\U000E0073\U000E0063\U000E0061\U000E007Fb"),QString::fromUcs4(U"a\U0001F3F4b"));
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
 // #1911: a candidate from the second read of an uncertain token's line says
 // so, and Quick redact matches it apart from the first read; the mark is
 // carried only when set, and only as true.
 void secondReadCandidatesKeepTheirMark() {
  Options options;options.candidates=true;options.languages={"eng"};
  const InputHeader input{200,100,200,600};
  TextResult result;result.languagesUsed={"eng"};
  result.candidates={{"first",QRect(10,12,60,14),41,false},{"second",QRect(10,12,60,14),77,true}};
  const auto json=encodeResult(result,options);
  const auto object=QJsonDocument::fromJson(json).object();
  QVERIFY(!object.value("candidates").toArray().at(0).toObject().contains("second"));
  QCOMPARE(object.value("candidates").toArray().at(1).toObject().value("second"),QJsonValue(true));
  TextResult back;QVERIFY(decodeResult(json,input,options,back));
  QCOMPARE(back.candidates.size(),qsizetype(2));QVERIFY(!back.candidates.at(0).second);QVERIFY(back.candidates.at(1).second);
  auto falseMark=object;auto list=falseMark.value("candidates").toArray();auto word=list.at(0).toObject();word["second"]=false;list[0]=word;falseMark["candidates"]=list;
  TextResult refused;QVERIFY(!decodeResult(QJsonDocument(falseMark).toJson(QJsonDocument::Compact),input,options,refused));
  // Only candidates carry it: a filtered word marked second is refused.
  Options both=options;both.words=true;
  auto inWords=object;auto marked=object.value("candidates").toArray().at(1).toObject();marked["text"]=QStringLiteral("w");
  inWords["words"]=QJsonArray{marked};
  TextResult wrongList;QVERIFY(!decodeResult(QJsonDocument(inWords).toJson(QJsonDocument::Compact),input,both,wrongList));
  marked.remove("second");inWords["words"]=QJsonArray{marked};
  TextResult plainWord;QVERIFY(decodeResult(QJsonDocument(inWords).toJson(QJsonDocument::Compact),input,both,plainWord));
 }
 void languageNamesAreIdentifiersOnly() {
  for(const auto* valid:{"eng","deu","chi_sim","chi_tra_vert","frak"})QVERIFY2(validLanguage(valid),valid);
  for(const auto* bad:{"","en","ENG","eng.traineddata","../eng","/usr/share/eng","eng deu","eng+deu","osd","equ","eng_","eng_a_b_c","-c","eng\n"})QVERIFY2(!validLanguage(bad),bad);
  QVERIFY(validLanguages({"eng","deu","fra"}));
  QVERIFY(!validLanguages({}));QVERIFY(!validLanguages({"eng","deu","fra","spa"}));QVERIFY(!validLanguages({"eng","eng"}));QVERIFY(!validLanguages({"eng","../deu"}));
 }
 // #1618, round 2 of #1631 (U8): the shared table the MCP server reads too,
 // one row per rule, with ordinary patterns that must stay allowed.
 void redactValidationTable_data() {
  QTest::addColumn<bool>("pattern");QTest::addColumn<QString>("value");QTest::addColumn<QString>("error");QTest::addColumn<QString>("prefix");
  QFile file(QStringLiteral(PUTGD_SOURCE_DIR "/tests/fixtures/text/redact-validation.json"));
  QVERIFY(file.open(QIODevice::ReadOnly));
  const auto table=QJsonDocument::fromJson(file.readAll()).object();
  int index=0;
  for(const auto* list:{"terms","patterns"})for(const auto& value:table[list].toArray()){
   const auto row=value.toObject();
   QTest::addRow("%s %d %s",list,index++,qPrintable(row["rule"].toString()))<<(QString(list)=="patterns")<<row["value"].toString()<<row["error"].toString()<<row["errorPrefix"].toString();
  }
  QVERIFY(index>50);
 }
 void redactValidationTable() {
  QFETCH(bool,pattern);QFETCH(QString,value);QFETCH(QString,error);QFETCH(QString,prefix);
  const auto got=pattern?redactPatternError(value):redactTermError(value);
  if(!prefix.isEmpty())QVERIFY2(got.startsWith(prefix),qPrintable(got));
  else QCOMPARE(got,error);
 }
 // #1618: what the table cannot show. A message never repeats the value, and
 // a compile error gives Qt's message with the offset in the pattern.
 void redactErrorsNeverRepeatTheValue() {
  const QString canary=QStringLiteral("zq1618canary");
  for(const auto& bad:{canary+"\n"+canary+"x",canary+"\r"+canary,canary+QChar(0x2028)+canary,canary+QString(maxRedactTermLength,'a'),QStringLiteral("..")}){
   const auto error=redactTermError(bad);QVERIFY(!error.isEmpty());QVERIFY2(!error.contains(canary),qPrintable(error));
  }
  for(const auto& bad:{canary+"(abc",canary+"[",canary+"(?<",canary+"\n",canary+"|",canary+"("}){
   const auto error=redactPatternError(bad);QVERIFY(!error.isEmpty());QVERIFY2(!error.contains(canary),qPrintable(error));
  }
  const auto unclosed=redactPatternError(canary+"(abc");
  QVERIFY2(unclosed.startsWith("This pattern does not compile: ")&&unclosed.endsWith(" at offset 16"),qPrintable(unclosed));
  // 500 characters outside the BMP are 1000 UTF-16 units and still fit.
  const QString astral=QString::fromUcs4(U"\U0001D538");
  QCOMPARE(redactPatternError(astral.repeated(maxRedactPatternLength)),QString());
  QCOMPARE(redactPatternError(astral.repeated(maxRedactPatternLength+1)),QString("A pattern is longer than 500 characters"));
 }
};
QTEST_APPLESS_MAIN(TextProtocolTest)
#include "test_text_protocol.moc"
