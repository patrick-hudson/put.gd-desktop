// putgd-ocr's engine-free rules (#1428) whose failure would be silent: the
// untrusted-character filter, code areas blanked before recognition, band
// seams that lose or double no line, and the input limit. Synthetic data only.
#include "ocr/Layout.h"
#include "ocr/Sanitize.h"
#include "ocr/Words.h"
#include <QTest>
#include <iterator>

using namespace putgd::ocr;
namespace {
QString points(std::initializer_list<char32_t> list){return QString::fromUcs4(std::data(list),qsizetype(list.size()));}
Profile lines(int width,int height,int lineHeight,int gap){
 Profile p;p.lineHeight=lineHeight;p.rowInk.fill(0,height);
 for(int y=0;y<height;++y)if(gap==0||(y%(lineHeight+gap))<lineHeight)p.rowInk[y]=width/4;
 return p;
}
}
class OcrLogic final:public QObject {
 Q_OBJECT
private slots:
 // Decision 4a. Built from code points, so this source holds no control itself.
 void filtersUntrustedCharacters_data(){
  QTest::addColumn<QString>("input");QTest::addColumn<bool>("code");QTest::addColumn<QString>("expected");
  QTest::newRow("c0-and-del")<<points({'a',0x01,'b',0x1b,'c',0x7f,'d','\r'})<<false<<QStringLiteral("abcd");
  QTest::newRow("tab-newline-kept")<<points({'a','\t','b','\n','c'})<<true<<points({'a','\t','b','\n','c'});
  QTest::newRow("c1")<<points({'a',0x85,'b',0x9b,'c'})<<false<<QStringLiteral("abc");
  QTest::newRow("bidi-override")<<points({'s','a','f','e',0x202e,'l','i','v','e'})<<true<<QStringLiteral("safelive");
  QTest::newRow("bidi-embedding-isolate")<<points({'a',0x202a,'b',0x202c,'c',0x2066,'d',0x2069,'e',0x200f})<<false<<QStringLiteral("abcde");
  QTest::newRow("zero-width-and-bom")<<points({'a',0x200b,'b',0xfeff,'c',0x2060,'d'})<<false<<QStringLiteral("abcd");
  QTest::newRow("tag-characters")<<points({'a',0xe0041,0xe007f,'b'})<<true<<QStringLiteral("ab");
  QTest::newRow("line-separators")<<points({'a',0x2028,'b',0x2029,'c'})<<false<<QStringLiteral("a\nb\nc");
  QTest::newRow("joiners-kept-in-text")<<points({0x645,0x200c,0x6cc,0x200d})<<false<<points({0x645,0x200c,0x6cc,0x200d});
  QTest::newRow("joiners-removed-from-codes")<<points({0x645,0x200c,0x6cc,0x200d})<<true<<points({0x645,0x6cc});
  QTest::newRow("nfc")<<points({'e',0x301})<<true<<points({0xe9});
 }
 void filtersUntrustedCharacters(){
  QFETCH(QString,input);QFETCH(bool,code);QFETCH(QString,expected);
  QCOMPARE(sanitizeUntrusted(input,code?Untrusted::CodeContent:Untrusted::RecognizedText),expected);
 }
 // A code's area is filled with the background in every band copy, so no word
 // can come from its modules. Here the "code" is a checkerboard on white.
 void codeAreasAreBlank(){
  const int width=300,height=200;QByteArray grey(width*height,char(255));
  for(int y=50;y<150;++y)for(int x=100;x<200;++x)grey[y*width+x]=char((x+y)%2?0:255);
  const GreyView view{reinterpret_cast<const uint8_t*>(grey.constData()),width,height,width};
  const QList<Quad> codes{Quad{QPoint(100,50),QPoint(199,50),QPoint(199,149),QPoint(100,149)}};
  for(const double scale:{1.0,2.0}){
   const auto band=bandImage(view,{0,height,false},scale,4,codes,255,false);
   for(const char p:band.pixels)QCOMPARE(uchar(p),uchar(255));
  }
 }
 // Every row is in a band; bands cut at a gap meet, bands cut with no gap
 // overlap by two lines; a word cut by a seam comes from the band that holds
 // it whole, and a word seen in both bands is kept once.
 void seamsLoseAndDoubleNoLine(){
  for(const int gap:{4,0}){
   const int width=4000,height=4000;PlanLimits limits;limits.forcedScale=1;
   const auto p=plan(width,height,lines(width,height,12,gap),limits);
   QVERIFY(p.banded());QCOMPARE(p.bands.first().top,0);QCOMPARE(p.bands.last().bottom,height);
   for(int i=1;i<p.bands.size();++i){
    QVERIFY(p.bands[i].top>p.bands[i-1].top);
    QCOMPARE(p.bands[i-1].bottom-p.bands[i].top,gap?0:24);
    QCOMPARE(p.bands[i].seamAbove,gap==0);
   }
  }
  Recognition job;const Band first{0,200,false},second{176,400,true};
  BandText a;a.lines={{QRect(0,100,1000,20),0},{QRect(0,185,1000,15),0},{QRect(0,178,1000,18),0}};
  a.words={{"kept",QRect(10,100,40,18),90,0},{"partial",QRect(10,185,40,15),40,1},{"twice",QRect(300,178,40,18),70,2}};
  // In the second band, row 4 is source row 180: the line the first band cut through.
  BandText b;b.lines={{QRect(0,2,1000,20),0},{QRect(0,40,1000,20),0}};
  b.words={{"twice",QRect(300,2,40,18),95,0},{"whole",QRect(10,4,40,18),88,0},{"next",QRect(10,40,40,18),91,1}};
  const int firstWord=mergeBand(job,a,first,nullptr,0,1,0,0,1000,400);
  mergeBand(job,b,second,&first,firstWord,1,0,1,1000,400);
  QStringList texts;for(const auto& w:job.words)texts.append(w.text);
  QCOMPARE(texts,QStringList({"kept","twice","whole","next"}));
 }
 void refusesInputOverTheLimit(){
  QCOMPARE(plan(4900,4900,lines(4900,4900,12,4),{}).error,QStringLiteral("input_excessive"));
  QVERIFY(plan(4898,4898,lines(4898,4898,12,4),{}).error.isEmpty());
 }
};
QTEST_APPLESS_MAIN(OcrLogic)
#include "test_ocr_logic.moc"
