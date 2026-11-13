// putgd-ocr's engine-free rules (#1428) whose failure would be silent: code
// areas blanked before recognition, band seams that lose or double no line,
// strips cut between lines and joined in page order, and the input limit. The
// contract's own tests cover the character filter.
// Synthetic data only.
#include "ocr/Layout.h"
#include "ocr/Words.h"
#include <QTest>
#include <iterator>

using namespace putgd::ocr;
namespace {
Profile lines(int width,int height,int lineHeight,int gap){
 Profile p;p.lineHeight=lineHeight;p.rowInk.fill(0,height);
 for(int y=0;y<height;++y)if(gap==0||(y%(lineHeight+gap))<lineHeight)p.rowInk[y]=width/4;
 return p;
}
}
class OcrLogic final:public QObject {
 Q_OBJECT
private slots:
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
  const Band first{0,200,false},second{176,400,true};
  // The second band lists the seam line's words in the engine's logical
  // order: left to right, or right to left as for Arabic or Hebrew.
  const auto merged=[&](bool rightToLeft){
   Recognition job;
   BandText a;a.lines={{QRect(0,100,1000,20),0},{QRect(0,185,1000,15),0},{QRect(0,178,1000,18),0}};
   a.words={{"kept",QRect(10,100,40,18),90,0},{"partial",QRect(10,185,40,15),40,1},{"twice",QRect(300,178,40,18),70,2}};
   // In the second band, row 4 is source row 180: the line the first band cut through.
   BandText b;b.lines={{QRect(0,2,1000,20),0},{QRect(0,40,1000,20),0}};
   const BandText::Item twice{"twice",QRect(300,2,40,18),95,0},whole{"whole",QRect(10,4,40,18),88,0};
   b.words=rightToLeft?QList<BandText::Item>{twice,whole}:QList<BandText::Item>{whole,twice};
   b.words.append({"next",QRect(10,40,40,18),91,1});
   const int firstWord=mergeBand(job,a,first,nullptr,0,1,0,0,1000,400);
   mergeBand(job,b,second,&first,firstWord,1,0,1,1000,400);
   return copiedText(job);
  };
  // The line the seam crossed is one line, in the engine's order, and the
  // band boundary is not a paragraph break.
  QCOMPARE(merged(false),QStringLiteral("kept\nwhole twice\nnext"));
  QCOMPARE(merged(true),QStringLiteral("kept\ntwice whole\nnext"));
 }
 // Parallel recognition: a single pass is cut into strips at blank row gaps,
 // each recognized on its own and merged by its index, whatever order the
 // strips finished in. Every line comes out once, in page order, and a strip
 // boundary is not a paragraph break.
 // A dense block whose rows are all inked (a border down its side, and
 // descenders and ascenders meeting between lines) is cut between lines too.
 void stripsKeepEveryLineOnceInOrder_data(){
  QTest::addColumn<bool>("blankGaps");
  QTest::newRow("blank-gaps")<<true;
  QTest::newRow("no-blank-row")<<false;
 }
 void stripsKeepEveryLineOnceInOrder(){
  QFETCH(bool,blankGaps);
  const int width=1000,height=600;auto profile=lines(width,height,12,8);
  if(!blankGaps){
   profile.lineHeight=0;  // no blank row, so no line height either
   for(int y=0;y<height;++y){const int r=y%20;profile.rowInk[y]=r<12?width/4:r==12||r==19?40:8;}
   for(int y=0;y<height;++y)QVERIFY(!profile.blank(y,width));
  }
  const auto cut=strips(profile,width,height,4);
  QVERIFY(cut.size()>1);QCOMPARE(cut.first().top,0);QCOMPARE(cut.last().bottom,height);
  for(int i=0;i<cut.size();++i){
   QVERIFY(!cut[i].seamAbove);if(i)QCOMPARE(cut[i].top,cut[i-1].bottom);
   if(i)QVERIFY2(cut[i].top%20>=12,qPrintable(QString::number(cut[i].top)));  // never through a line
  }
  // One word per text line, in each strip's own pixels.
  QList<BandText> parts(cut.size());QStringList expected;
  for(int y=0,n=0;y<height;y+=20,++n){
   int s=0;while(s+1<cut.size()&&y>=cut[s].bottom)++s;
   const int local=y-cut[s].top;
   parts[s].lines.append({QRect(0,local,width,12),0});
   parts[s].words.append({QStringLiteral("w%1").arg(n),QRect(10,local,40,12),90,int(parts[s].lines.size()-1)});
   expected.append(QStringLiteral("w%1").arg(n));
  }
  Recognition job;int first=0;
  for(int i=0;i<cut.size();++i)first=mergeBand(job,parts[i],cut[i],i?&cut[i-1]:nullptr,first,1,0,i,width,height);
  QCOMPARE(copiedText(job),expected.join(QLatin1Char('\n')));
  QCOMPARE(job.lines.size(),qsizetype(expected.size()));
 }
 // Whole screens: two windows side by side are two panels, read left then
 // right, each holding every one of its lines once; a single column of the
 // same text is one panel, the whole image, read as before.
 void panelsSplitWindowsNotColumnsOfText(){
  const int width=1200,height=400;
  // Fake glyphs: 6 px wide marks with 3 px gaps, 12 px lines, 8 px apart.
  const auto page=[&](const QList<QRect>& columns){
   QByteArray grey(width*height,char(255));
   for(const auto& c:columns)for(int y=c.top();y+12<=c.bottom();y+=20)for(int x=c.left();x+6<=c.right();x+=9)
    for(int dy=0;dy<12;++dy)for(int dx=0;dx<6;++dx)grey[(y+dy)*width+x+dx]=char(20);
   return grey;
  };
  const QRect left(20,20,500,360),right(680,20,500,360);
  const auto two=page({left,right});
  const auto found=panels({reinterpret_cast<const uint8_t*>(two.constData()),width,height,width},{},12);
  QCOMPARE(found.size(),2);
  QVERIFY(found[0].contains(left.adjusted(0,0,-6,-6)));QVERIFY(found[1].contains(right.adjusted(0,0,-6,-6)));
  QVERIFY(found[0].right()<found[1].left());
  const auto one=page({QRect(20,20,1160,360)});
  QCOMPARE(panels({reinterpret_cast<const uint8_t*>(one.constData()),width,height,width},{},12),QList<QRect>{QRect(0,0,width,height)});
 }
 void refusesInputOverTheLimit(){
  QCOMPARE(plan(4900,4900,lines(4900,4900,12,4),{}).error,QStringLiteral("input_excessive"));
  QVERIFY(plan(4898,4898,lines(4898,4898,12,4),{}).error.isEmpty());
 }
};
QTEST_APPLESS_MAIN(OcrLogic)
#include "test_ocr_logic.moc"
