// putgd-ocr's engine-free rules (#1428) whose failure would be silent: code
// areas blanked before recognition, band seams that lose or double no line,
// strips cut between lines and joined in page order, faint lines read again (#1894) in each
// window of a screen grown past its trimmed text (#1912), uncertain tokens' lines read again (#1911), the input limit, and
// emoji (#1748) put in place of what Tesseract made of them but never over
// confident letters. The contract's own tests cover the character filter.
// Synthetic data only: the emoji are drawn from the built-in templates.
#include "ocr/Layout.h"
#include "ocr/Words.h"
#include <QTest>
#include <cmath>
#include <iterator>

using namespace putgd::ocr;
namespace {
// A colour image of `width` x `height` on `background`, with templates drawn
// into it (nearest pixel from the template's side x side ink box).
struct Rgb { int r=255,g=255,b=255; };
struct Canvas {
 int width=0,height=0;QByteArray rgb;
 Canvas(int w,int h,Rgb background):width(w),height(h),rgb(QByteArray(w*h*3,'\0')){
  for(int i=0;i<w*h;++i){rgb[3*i]=char(background.r);rgb[3*i+1]=char(background.g);rgb[3*i+2]=char(background.b);}
 }
 void draw(const EmojiTemplates& t,int index,const QRect& at){
  const int side=t.side();const uint8_t* p=t.pixels(index);
  for(int y=0;y<at.height();++y)for(int x=0;x<at.width();++x){
   const uint8_t* q=p+4*((y*side/at.height())*side+x*side/at.width());
   char* o=rgb.data()+3*((at.y()+y)*width+at.x()+x);
   for(int c=0;c<3;++c)o[c]=char(q[c]+(255-q[3])*uchar(o[c])/255);
  }
 }
 ColourView view()const{return {reinterpret_cast<const uint8_t*>(rgb.constData()),width,height,width*3,{}};}
};
int find(const EmojiTemplates& t,const QString& text,int style=0){for(int i=0;i<t.count();++i)if(t.text(i)==text&&t.style(i)==style)return i;return -1;}
// A word whose characters each have a box, as Tesseract gives them; the
// character at `unsure` (what it made of an emoji) has a low confidence.
Word word(const QString& text,const QList<QRect>& boxes,float confidence,int unsure=-1){
 Word w;w.text=text;w.confidence=confidence;w.lineHeight=24;w.rect=boxes.first();
 for(int i=0;i<boxes.size();++i){w.symbols.append({boxes[i],i==unsure?30.f:confidence,1});w.rect=w.rect.united(boxes[i]);}
 return w;
}
QString recognized(Recognition job,const ColourView& colour){
 placeEmoji(job,findEmoji(colour,EmojiTemplates::builtIn()).hits);
 return copiedText(job);
}
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
 // #1894: a line of faint ink alone (comment grey beside brighter text on a dark page), which
 // Otsu's threshold turns to background, is found as a band to read again; brighter lines and a
 // smooth gradient are not. Stretched, its darkest pixel is black and the background white.
 void faintLinesAreFoundForASecondRead(){
  const int width=400,height=160;
  const auto page=[&](int faintInk,bool gradient){
   QByteArray grey(width*height,char(35));
   for(int y=0;y<height;++y)for(int x=0;x<width;++x){
    if(gradient)grey[y*width+x]=char(35+x*40/width);
    const bool glyph=(x/2)%2==0&&x>=20&&x<380;
    if(glyph&&((y>=10&&y<22)||(y>=40&&y<52)||(y>=70&&y<82)))grey[y*width+x]=char(220);
    if(glyph&&faintInk&&y>=100&&y<112)grey[y*width+x]=char(faintInk);
   }
   return grey;
  };
  const auto lines=[&](const QByteArray& grey){
   const GreyView view{reinterpret_cast<const uint8_t*>(grey.constData()),width,height,width};
   return faintLines(measure(view,{}),width,height);
  };
  const auto found=lines(page(72,false));
  QCOMPARE(found.size(),qsizetype(1));
  QVERIFY(found.first().top<=100&&found.first().bottom>=112&&found.first().bottom-found.first().top<=20);
  QVERIFY(lines(page(0,false)).isEmpty());
  // A bright line two rows above stays out of the faint band (Sol on #1904): its ink would pull
  // the band's threshold above the faint line again.
  {auto tight=page(72,false);
   for(int y=86;y<97;++y)for(int x=20;x<380;++x)if((x/2)%2==0)tight[y*width+x]=char(220);
   // and two bright pixels in each of the rows just above it, which a padding of two would take in.
   for(const int y:{97,98})for(const int x:{30,31})tight[y*width+x]=char(220);
   const auto close=lines(tight);
   QCOMPARE(close.size(),qsizetype(1));QVERIFY2(close.first().top>=99,qPrintable(QString::number(close.first().top)));}
  QVERIFY(lines(page(0,true)).isEmpty());
  GreyImage band;band.width=4;band.height=1;band.pixels=QByteArray("\xb6\xc8\xdc\xdc",4);
  stretchContrast(band,220);
  QCOMPARE(uchar(band.pixels.at(0)),uchar(0));QCOMPARE(uchar(band.pixels.at(3)),uchar(255));
  QVERIFY(uchar(band.pixels.at(1))>0&&uchar(band.pixels.at(1))<255);
 }
 // #1912: a trimmed panel grows through its window's plain background to a
 // faint line below and beside its text, and stops at the window's edge on a
 // desktop of another shade; then that line is found as faint.
 void panelsGrowThroughTheirWindow(){
  const int width=900,height=500;
  QByteArray grey(width*height,char(21));                       // desktop
  for(int y=40;y<400;++y)for(int x=60;x<760;++x)grey[y*width+x]=char(44);   // window
  for(int line=0;line<5;++line)for(int y=80+line*30;y<92+line*30;++y)for(int x=100;x<300;++x)if((x/3)%2==0)grey[y*width+x]=char(190);
  for(int y=260;y<272;++y)for(int x=100;x<700;++x)if((x/3)%2==0)grey[y*width+x]=char(70);   // faint, longer
  const GreyView view{reinterpret_cast<const uint8_t*>(grey.constData()),width,height,width};
  const QRect trimmed(92,72,216,160);
  const auto grown=plainAround(view,trimmed,{},44);
  QCOMPARE(grown,QRect(QPoint(60,40),QPoint(759,399)));
  const GreyView window{view.row(grown.y())+grown.x(),grown.width(),grown.height(),width};
  const auto found=faintLines(measure(window,{}),grown.width(),grown.height());
  QCOMPARE(found.size(),qsizetype(1));QVERIFY(found.first().top<=260-40&&found.first().bottom>=272-40);
  // A long faint line below the read rows that Otsu takes for ink over the
  // whole window is still read again as text the first pass never saw.
  {auto heavy=grey;
   for(int y=320;y<338;++y)for(int x=80;x<740;++x)if(x%3!=0)heavy[y*width+x]=char(70);
   const GreyView heavyView{reinterpret_cast<const uint8_t*>(heavy.constData()),width,height,width};
   const auto g=plainAround(heavyView,trimmed,{},44);
   const GreyView w{heavyView.row(g.y())+g.x(),g.width(),g.height(),width};
   const auto prof=measure(w,{});
   const int readTop=trimmed.top()-g.top(),readBottom=trimmed.bottom()+1-g.top();
   const auto unread=unreadLines(prof,g.width(),g.height(),readTop,readBottom);
   // The faint line at row 260 and the heavy one, both below the read rows.
   QCOMPARE(unread.size(),qsizetype(2));
   QVERIFY(unread.first().top>=readBottom&&unread.first().top<=260-g.top());
   QVERIFY(unread.last().top<=320-g.top()&&unread.last().bottom>=338-g.top());}
  // Never into another panel: one beside it on the same plain background.
  QCOMPARE(plainAround(view,trimmed,{QRect(500,40,100,360)},44).right(),499);
  // A dense faint line, two thirds ink across the window, still lets it grow past (Sol on #1920).
  {auto dense=grey;
   for(int y=300;y<312;++y)for(int x=60;x<760;++x)if(x%3!=0)dense[y*width+x]=char(70);
   const GreyView denseView{reinterpret_cast<const uint8_t*>(dense.constData()),width,height,width};
   QCOMPARE(plainAround(denseView,trimmed,{},44),QRect(QPoint(60,40),QPoint(759,399)));}
  // A stripe of the window's shade across the desktop below does not carry it out.
  {auto striped=grey;
   for(int y=400;y<height;++y)for(int x=60;x<300;++x)striped[y*width+x]=char(44);
   const GreyView stripedView{reinterpret_cast<const uint8_t*>(striped.constData()),width,height,width};
   QCOMPARE(plainAround(stripedView,trimmed,{},44).bottom(),399);}
 }
 // #1911: only an uncertain word shaped like a key or token is read again;
 // its line's rows, joined with a neighbour's, and the least confident first
 // when there are too many. The copy read again is dark ink on white whatever
 // the page's polarity or contrast.
 void uncertainTokensAreReadAgain(){
  QVERIFY(uncertainToken(QStringLiteral("ghp_a1B2c3D4e5F6g7"),40));
  QVERIFY(uncertainToken(QStringLiteral("\"sk-ant-api03-x9Y8z7\","),69));
  QVERIFY(!uncertainToken(QStringLiteral("ghp_a1B2c3D4e5F6g7"),70));       // confident
  QVERIFY(!uncertainToken(QStringLiteral("internationalization"),20));    // no digit
  QVERIFY(!uncertainToken(QStringLiteral("2026-11-22.193401"),20));       // no letter
  QVERIFY(!uncertainToken(QStringLiteral("\"a1b2c3d4e5f\""),20));        // 11 once trimmed
  // Words of one row are one band, the least confident band first; a dense
  // run of rows is cut into bands of at most four word heights.
  const auto bands=tokenLines({{QRect(10,100,200,12),50},{QRect(300,104,100,12),20},{QRect(10,10,200,12),60}},200);
  QCOMPARE(bands.size(),qsizetype(2));
  QCOMPARE(bands[0].band.top,100-5);QCOMPARE(bands[0].band.bottom,116+5);QCOMPARE(bands[0].confidence,20.f);
  QCOMPARE(bands[1].band.top,10-5);QCOMPARE(bands[1].band.bottom,22+5);QCOMPARE(bands[1].confidence,60.f);
  QList<TokenWord> dense;for(int y=0;y<240;y+=16)dense.append({QRect(10,y,300,12),50});
  const auto cut=tokenLines(dense,400);
  QVERIFY(cut.size()>=4);
  for(const auto& b:cut)QVERIFY2(b.band.bottom-b.band.top<=48,qPrintable(QString::number(b.band.bottom-b.band.top)));
  const int width=320,height=80;
  for(const auto& [paper,ink]:{std::pair{30,200},std::pair{240,40},std::pair{30,62},std::pair{0x58,0x93}}){
   QByteArray grey(width*height,char(paper));
   for(int y=30;y<42;++y)for(int x=20;x<300;++x)if((x/3)%2==0)grey[y*width+x]=char(ink);
   const auto copy=contrastBand({reinterpret_cast<const uint8_t*>(grey.constData()),width,height,width},{26,46,false},2,4,{});
   QCOMPARE(copy.width,width*2+8);QCOMPARE(copy.height,20*2+8);
   int darkest=255,lightest=0;
   for(int y=0;y<copy.height;++y)for(int x=0;x<copy.width;++x){const int v=uchar(copy.pixels.at(y*copy.width+x));darkest=std::min(darkest,v);lightest=std::max(lightest,v);}
   QVERIFY2(darkest<=40,qPrintable(QStringLiteral("paper %1 ink %2: darkest %3").arg(paper).arg(ink).arg(darkest)));
   QCOMPARE(lightest,255);
   // The rows above the glyphs are paper: white.
   QCOMPARE(uchar(copy.pixels.at((4+2)*copy.width+copy.width/2)),uchar(255));
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
 // same text is one panel, the whole image, read as before, also when one of
 // its lines runs far past the others.
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
  // On a tall page, one long line (a token) past the others of its column
  // is not cut off into panels of its own.
  const int tallWidth=2400,tall=1500;
  QByteArray tallPage(tallWidth*tall,char(255));
  for(int y=20;y+12<=tall-20;y+=20){
   for(int x=20;x+6<=620;x+=9)for(int dy=0;dy<12;++dy)for(int dx=0;dx<6;++dx)tallPage[(y+dy)*tallWidth+x+dx]=char(20);
   // The long line's tail: dense thin strokes, too little activity a column
   // for the noise floor of a tall page, and two groups of tall glyphs
   // (brackets) that stand above it, as a token's tail had.
   if(y==700){
    for(int x=620;x<2300;x+=4)for(int dy=2;dy<10;++dy)tallPage[(y+dy)*tallWidth+x]=char(20);
    for(const int group:{1500,2200})for(int x=group;x<group+12;x+=4)for(int dy=-10;dy<20;++dy)tallPage[(y+dy)*tallWidth+x]=char(20);
   }
  }
  QCOMPARE(panels({reinterpret_cast<const uint8_t*>(tallPage.constData()),tallWidth,tall,tallWidth},{},12),QList<QRect>{QRect(0,0,tallWidth,tall)});
 }
 // An emoji takes the place of what Tesseract read for it, a stray letter
 // glued to a word or a symbol of its own, and joins a neighbour without a
 // space when its glyph touches it: "I ❤️ this" and "Hello👋", whatever the
 // background.
 void emojiReplaceWhatTesseractMadeOfThem(){
  const auto& t=EmojiTemplates::builtIn();QVERIFY(t.count()>6000);
  const int heart=find(t,QString::fromUtf8("\u2764\ufe0f")),wave=find(t,QString::fromUtf8("\U0001F44B"),1);
  QVERIFY(heart>=0&&wave>=0);
  // White, a dark theme, and a saturated chat bubble the emoji sit inside.
  for(const Rgb background:{Rgb{255,255,255},Rgb{49,51,56},Rgb{20,90,165}}){
   Canvas canvas(220,40,background);
   canvas.draw(t,heart,QRect(40,10,22,20));canvas.draw(t,wave,QRect(147,10,20,21));
   Recognition job;job.lines={{QRect(0,8,220,24),0,0}};
   // "IW this Hellow": the W is the heart, the last w the wave, glued to "Hello".
   job.words={word("IW",{QRect(20,12,6,16),QRect(40,10,22,20)},43,1),word("this",{QRect(70,12,6,16),QRect(77,12,6,16),QRect(84,12,6,16),QRect(91,12,6,16)},96),
              word("Hellow",{QRect(110,12,6,16),QRect(117,12,6,16),QRect(124,12,6,16),QRect(131,12,6,16),QRect(138,12,8,16),QRect(147,10,20,21)},88,5)};
   QCOMPARE(recognized(job,canvas.view()),QString::fromUtf8("I \u2764\ufe0f this Hello\U0001F44B"));
  }
 }
 // A plain shape such as a red dot is matched, so it is not taken for a stop
 // sign, but never named; a glyph that only roughly fits a template that stands
 // out little from the background (a faint letter against a dark disc) is not
 // named; and emoji on no line of text get a line of their own, a tall narrow
 // one included.
 void emojiNeverNameShapesOrFaintFits(){
  const auto& t=EmojiTemplates::builtIn();
  const int face=find(t,QString::fromUtf8("\U0001F602")),dot=find(t,QString::fromUtf8("\U0001F534")),moon=find(t,QString::fromUtf8("\U0001F311")),merperson=find(t,QString::fromUtf8("\U0001F9DC"));
  QVERIFY(face>=0&&dot>=0&&moon>=0&&merperson>=0&&t.shape(dot)&&!t.shape(face));
  Canvas canvas(200,100,Rgb{30,30,30});
  canvas.draw(t,dot,QRect(100,10,20,20));canvas.draw(t,face,QRect(60,50,20,20));
  canvas.draw(t,merperson,QRect(90,45,int(std::lround(40*t.aspect(merperson))),40));
  // A dim blue letter-sized blob where a dark disc would fit loosely.
  for(int y=12;y<26;++y)for(int x=20;x<32;++x)if((x-26)*(x-26)+(y-19)*(y-19)<=30){char* o=canvas.rgb.data()+3*(y*200+x);o[0]=char(40);o[1]=char(60);o[2]=char(110);}
  Recognition job;job.lines={{QRect(0,8,200,24),0,0}};
  job.words={word("o",{QRect(20,12,12,14)},95),word("@",{QRect(100,10,20,20)},60),word("end",{QRect(130,12,6,16),QRect(137,12,6,16),QRect(144,12,6,16)},96)};
  QCOMPARE(recognized(job,canvas.view()),QString::fromUtf8("o @ end\n\U0001F602 \U0001F9DC"));
 }
 // An emoji goes where the line reads past it: between two words of a
 // right-to-left line in their logical order, after the one word of a
 // right-to-left line when it stands left of it, inside a left-to-right run of
 // a right-to-left paragraph as that run reads, and at the run's edges as the
 // paragraph reads.
 void emojiFollowTheLineDirection(){
  const QString face=QString::fromUtf8("\U0001F602");
  const auto place=[&](bool rightToLeft,const QList<Word>& words,int x=60){
   Recognition job;job.lines={{QRect(0,10,260,20),0,0,rightToLeft}};job.words=words;
   placeEmoji(job,{{face,QRect(x,10,20,20),0.02,0}});
   return copiedText(job);
  };
  const auto hebrew=[](const char* text,int x){return word(QString::fromUtf8(text),{QRect(x,12,30,16)},95);};
  QCOMPARE(place(true,{hebrew("\u05D0\u05D1\u05D2",100),hebrew("\u05D3\u05D4\u05D5",20)}),QString::fromUtf8("\u05D0\u05D1\u05D2 ")+face+QString::fromUtf8(" \u05D3\u05D4\u05D5"));
  QCOMPARE(place(true,{hebrew("\u05D0\u05D1\u05D2",100)}),QString::fromUtf8("\u05D0\u05D1\u05D2 ")+face);
  QCOMPARE(place(true,{word("foo",{QRect(20,12,30,16)},95),word("bar",{QRect(100,12,30,16)},95)}),"foo "+face+" bar");
  // Hebrew A (right), "foo bar" (a left-to-right run), Hebrew B (left), in Tesseract's order A foo bar B.
  const auto a=QString::fromUtf8("\u05D0\u05D1\u05D2"),b=QString::fromUtf8("\u05D3\u05D4\u05D5");
  const QList<Word> mixed{hebrew("\u05D0\u05D1\u05D2",220),word("foo",{QRect(70,12,30,16)},95),word("bar",{QRect(140,12,30,16)},95),hebrew("\u05D3\u05D4\u05D5",0)};
  QCOMPARE(place(true,mixed,112),a+" foo "+face+" bar "+b);   // inside the run, nearer bar
  QCOMPARE(place(true,mixed,40),a+" foo bar "+face+" "+b);    // left of the run: after it
  QCOMPARE(place(true,mixed,190),a+" "+face+" foo bar "+b);   // right of the run: before it
  // Touching the run's edge on the page joins the word next to it in that order.
  QCOMPARE(place(true,mixed,50),a+" foo bar"+face+" "+b);
  QCOMPARE(place(true,mixed,170),a+" "+face+"foo bar "+b);
  // A row of emoji on a line of its own stays one line, however long.
  Recognition row;row.lines={{QRect(0,10,60,20),0,0,false}};row.words={word("Hi",{QRect(0,12,20,16)},95)};
  QList<EmojiHit> five;for(int i=0;i<5;++i)five.append({face,QRect(22*i,50,20,20),0.02,0});
  placeEmoji(row,five);
  QCOMPARE(copiedText(row),"Hi\n"+face.repeated(5));
  // ...also with a line of text beside the row.
  Recognition beside;beside.lines={{QRect(160,50,40,20),0,0,false}};beside.words={word("text",{QRect(160,52,40,16)},95)};
  QList<EmojiHit> fiveMore;for(int i=0;i<5;++i)fiveMore.append({face,QRect(22*i,50,20,20),0.02,0});
  placeEmoji(beside,fiveMore);
  QCOMPARE(copiedText(beside),"text\n"+face.repeated(5));
  // Between two columns, an emoji goes with the nearer one as recognition
  // left it, not with a line an earlier emoji drew towards it.
  Recognition columns;columns.lines={{QRect(0,10,40,20),0,0,false},{QRect(120,10,40,20),0,0,false}};
  auto right=word("right",{QRect(120,12,40,16)},95);right.line=1;
  columns.words={word("left",{QRect(0,12,40,16)},95),right};
  placeEmoji(columns,{{face,QRect(43,10,20,20),0.02,0},{face,QRect(79,10,20,20),0.02,0}});
  QCOMPARE(copiedText(columns),"left"+face+"\n"+face+" right");
 }
 void refusesInputOverTheLimit(){
  QCOMPARE(plan(4900,4900,lines(4900,4900,12,4),{}).error,QStringLiteral("input_excessive"));
  QVERIFY(plan(4898,4898,lines(4898,4898,12,4),{}).error.isEmpty());
 }
};
QTEST_APPLESS_MAIN(OcrLogic)
#include "test_ocr_logic.moc"
