#include "Words.h"
#include "core/TextJobProtocol.h"
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace putgd::ocr {
namespace {
QRect toSource(const QRect& r,const Band& band,double scale,int border,int width,int height){
 const int left=int(std::floor((r.left()-border)/scale)),top=int(std::floor((r.top()-border)/scale))+band.top;
 const int right=int(std::ceil((r.left()+r.width()-border)/scale)),bottom=int(std::ceil((r.top()+r.height()-border)/scale))+band.top;
 return QRect(QPoint(std::clamp(left,0,width-1),std::clamp(top,0,height-1)),QPoint(std::clamp(right-1,0,width-1),std::clamp(bottom-1,0,height-1)));
}
double overlap(const QRect& a,const QRect& b){
 const auto i=a.intersected(b);if(i.isEmpty())return 0;
 const double smaller=std::min(double(a.width())*a.height(),double(b.width())*b.height());
 return smaller>0?double(i.width())*i.height()/smaller:0;
}
qint64 area(const QRect& r){return r.isEmpty()?0:qint64(r.width())*r.height();}
bool codeSymbol(char32_t c){
 static const char32_t symbols[]=U"{}()[]<>;:=+-*/\\|&!?#$%^~@,.'\"`_";
 for(const auto s:symbols)if(s&&s==c)return true;
 return false;
}
}
int mergeBand(Recognition& job,const BandText& band,const Band& placement,const Band* previous,int previousFirstWord,
               double scale,int border,int bandIndex,int imageWidth,int imageHeight){
 const int lineBase=int(job.lines.size());
 // A band's first block continues the block the previous band ended in: a
 // band boundary is not a paragraph break.
 const int blockBase=job.lines.isEmpty()?0:job.lines.last().block;
 for(const auto& line:band.lines)job.lines.append({toSource(line.rect,placement,scale,border,imageWidth,imageHeight),blockBase+line.block,bandIndex,line.rightToLeft});
 const bool seam=placement.seamAbove&&previous;
 if(seam){
  // The previous band was cut at its limit: its words on the cut edge are partial.
  for(int i=previousFirstWord;i<job.words.size();++i)if(job.words[i].rect.bottom()>=previous->bottom-1)job.words[i].text.clear();
 }
 const int firstNew=int(job.words.size());
 // First the duplicates: a word this band shares with the previous one stays
 // once, in the earlier band's place, and the line it sits on in this band
 // becomes that earlier line, so the rest of the physical line joins it.
 // Each word of a joined line keeps its place in Tesseract's logical order
 // (seamOrder: its index in this band's sequence), never its x, so a
 // right-to-left or mixed line is not reversed.
 QList<Word> incoming;QList<int> bandLines;QHash<int,int> sameLine;
 for(int k=0;k<band.words.size();++k){
  const auto& item=band.words[k];
  Word word;word.text=textjob::sanitizeText(item.text).trimmed();
  if(word.text.isEmpty())continue;
  word.rect=toSource(item.rect,placement,scale,border,imageWidth,imageHeight);
  // Symbols only while their cleaned text spells the cleaned word: a direction
  // mark Tesseract puts in a word's text but in no character, or one the filter
  // removes, changes no length that counts.
  if(item.symbols.size()==item.characters.size()&&!item.symbols.isEmpty()){
   QString spelled;QList<Symbol> symbols;
   for(int c=0;c<item.symbols.size();++c){
    auto symbol=item.symbols[c];const auto clean=textjob::sanitizeText(item.characters[c]);
    symbol.length=int(clean.size());spelled+=clean;symbol.rect=toSource(symbol.rect,placement,scale,border,imageWidth,imageHeight);
    if(symbol.length>0)symbols.append(symbol);
   }
   if(spelled==word.text)word.symbols=std::move(symbols);
  }
  word.confidence=item.confidence;word.line=lineBase+item.line;word.seamOrder=k;
  if(seam){
   if(word.rect.top()<=placement.top+1)continue;
   bool duplicate=false;
   if(word.rect.top()<previous->bottom){
    for(int i=previousFirstWord;i<firstNew;++i){
     auto& earlier=job.words[i];if(earlier.text.isEmpty()||overlap(earlier.rect,word.rect)<=0.5)continue;
     duplicate=true;
     if(!sameLine.contains(item.line))sameLine.insert(item.line,earlier.line);
     earlier.seamOrder=k;
     if(word.confidence>earlier.confidence){earlier.text=word.text;earlier.rect=word.rect;earlier.confidence=word.confidence;earlier.symbols=word.symbols;}
     break;
    }
   }
   if(duplicate)continue;
  }
  incoming.append(std::move(word));bandLines.append(item.line);
 }
 for(int i=0;i<incoming.size();++i){
  auto& word=incoming[i];
  if(sameLine.contains(bandLines[i]))word.line=sameLine.value(bandLines[i]);
  word.lineHeight=job.lines.value(word.line).rect.height();
  job.words.append(std::move(word));
 }
 int removed=0;for(int i=previousFirstWord;i<firstNew;++i)if(job.words[i].text.isEmpty())++removed;
 job.words.erase(std::remove_if(job.words.begin()+std::min<qsizetype>(previousFirstWord,job.words.size()),job.words.end(),[](const Word& w){return w.text.isEmpty();}),job.words.end());
 if(!sameLine.isEmpty()){
  QSet<int> joined;for(const auto line:sameLine)joined.insert(line);
  // An earlier word with no partner in this band sits beside its neighbours:
  // just before the next earlier word that has one, or after the last.
  const int start=std::min<int>(previousFirstWord,int(job.words.size()));
  for(int i=start;i<job.words.size();++i){
   auto& w=job.words[i];
   if(!joined.contains(w.line)||w.seamOrder>=0||i>=firstNew-removed)continue;
   double order=-1;bool found=false;
   for(int j=i+1;j<firstNew-removed&&!found;++j)if(job.words[j].line==w.line&&job.words[j].seamOrder>=0){order=job.words[j].seamOrder-0.5;found=true;}
   for(int j=i-1;j>=start&&!found;--j)if(job.words[j].line==w.line&&job.words[j].seamOrder>=0){order=job.words[j].seamOrder+0.5;found=true;}
   w.seamOrder=order;
  }
  // Each line's words together, lines in the order they first appear, and
  // the words of a joined line in the engine's logical order.
  QHash<int,int> firstSeen;
  for(int i=start;i<job.words.size();++i)if(!firstSeen.contains(job.words[i].line))firstSeen.insert(job.words[i].line,i);
  std::stable_sort(job.words.begin()+start,job.words.end(),[&](const Word& a,const Word& b){
   if(a.line!=b.line)return firstSeen.value(a.line)<firstSeen.value(b.line);
   return joined.contains(a.line)&&a.seamOrder<b.seamOrder;
  });
 }
 for(int i=std::min<int>(previousFirstWord,int(job.words.size()));i<job.words.size();++i)job.words[i].seamOrder=-1;
 return firstNew-removed;
}
void joinUrls(Recognition& job){
 static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]{1,15}:$")),url(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]{1,15}://"));
 // An address split at a dot of its domain: "corpus@example. invalid".
 static const QRegularExpression address(QStringLiteral("^[\\w.+%-]+@[\\w-]+(\\.[\\w-]+)*\\.$"));
 static const QString joints=QStringLiteral("./?=&#-_");
 QList<Word> out;out.reserve(job.words.size());
 for(auto& word:job.words){
  if(!out.isEmpty()){
   auto& last=out.last();
   const int gap=word.rect.left()-(last.rect.right()+1);
   const bool sameLine=last.line==word.line&&gap>=0&&gap<std::max(2,last.lineHeight/2);
   const bool start=scheme.match(last.text).hasMatch()&&word.text.startsWith(QLatin1String("//"));
   const QChar next=word.text.isEmpty()?QChar():word.text.front();
   const bool continues=url.match(last.text).hasMatch()&&!last.text.isEmpty();
   const bool more=continues&&((joints.contains(last.text.back())&&(next.isDigit()||next.isLower()))||QStringLiteral("/?#&=").contains(next));
   const bool domain=address.match(last.text).hasMatch()&&(next.isDigit()||next.isLower());
   if(sameLine&&(start||more||domain)){
    // Spelled by both words' symbols, or by none when either has none.
    if(!last.symbols.isEmpty()&&!word.symbols.isEmpty())last.symbols+=word.symbols;else last.symbols.clear();
    last.text+=word.text;last.rect=last.rect.united(word.rect);last.confidence=std::min(last.confidence,word.confidence);
    continue;
   }
  }
  out.append(std::move(word));
 }
 job.words=std::move(out);
}
void placeEmoji(Recognition& job,const QList<EmojiHit>& hits){
 for(const auto& hit:hits){
  const QRect glyph=hit.rect;
  // Lines that hold words: a band seam leaves a second, empty box for a line
  // whose words joined the band above, and an emoji does not go there.
  QSet<int> worded;for(const auto& w:std::as_const(job.words))worded.insert(w.line);
  // The characters the glyph covers leave their words.
  for(int i=0;i<job.words.size();){
   const auto word=job.words[i];
   if(word.emoji||!word.rect.intersects(glyph)){++i;continue;}
   if(word.symbols.isEmpty()){
    if(area(word.rect.intersected(glyph))*2>=area(word.rect))job.words.removeAt(i);else ++i;
    continue;
   }
   QList<Word> pieces;Word piece;int at=0;bool covered=false;
   const auto flush=[&]{if(!piece.symbols.isEmpty()){pieces.append(piece);}piece=Word{};};
   for(const auto& symbol:word.symbols){
    const auto text=word.text.mid(at,symbol.length);at+=symbol.length;
    if(area(symbol.rect.intersected(glyph))*5>=area(symbol.rect)*2){covered=true;flush();continue;}
    // A piece is as sure as its own characters: the word's confidence counted
    // the junk the emoji was read as.
    if(piece.symbols.isEmpty()){piece=word;piece.text.clear();piece.symbols.clear();piece.rect=symbol.rect;piece.confidence=symbol.confidence;piece.attach=word.attach&&pieces.isEmpty();}
    piece.text+=text;piece.symbols.append(symbol);piece.rect=piece.rect.united(symbol.rect);piece.confidence=std::min(piece.confidence,symbol.confidence);
   }
   flush();
   if(!covered){++i;continue;}
   job.words.removeAt(i);
   for(int k=0;k<pieces.size();++k)job.words.insert(i+k,pieces[k]);
   i+=int(pieces.size());
  }
  // The line it belongs to: one it shares at least 40% of its height with,
  // the nearest across, within three glyph widths.
  int line=-1;qint64 nearest=std::numeric_limits<qint64>::max();int bestOverlap=0;
  for(int l=0;l<job.lines.size();++l){
   if(!worded.contains(l))continue;
   const auto& r=job.lines[l].rect;
   const int overlap=std::min(r.bottom(),glyph.bottom())-std::max(r.top(),glyph.top())+1;
   if(overlap*5<2*std::min(r.height(),glyph.height()))continue;
   const qint64 across=std::max({0,r.left()-glyph.right()-1,glyph.left()-r.right()-1});
   if(across>3*qint64(glyph.width()))continue;
   if(across<nearest||(across==nearest&&overlap>bestOverlap)){line=l;nearest=across;bestOverlap=overlap;}
  }
  if(line<0){
   // A line of its own, after the last line above it in its column (or the
   // last above it at all), in that line's block.
   int after=-1;
   for(int pass=0;pass<2&&after<0;++pass)for(int l=0;l<job.lines.size();++l){
    const auto& r=job.lines[l].rect;
    const bool column=r.left()<=glyph.right()&&glyph.left()<=r.right();
    if((pass==0&&!column)||r.center().y()>glyph.center().y())continue;
    if(after<0||r.bottom()>=job.lines[after].rect.bottom())after=l;
   }
   line=after+1;
   const int block=after>=0?job.lines[after].block:(job.lines.isEmpty()?0:job.lines.first().block);
   job.lines.insert(line,Line{glyph,block,after>=0?job.lines[after].band:0,false});
   for(auto& w:job.words)if(w.line>=line)++w.line;
  }else job.lines[line].rect=job.lines[line].rect.united(glyph);
  // Its place among the line's words, in the line's own direction.
  int first=0;while(first<job.words.size()&&job.words[first].line<line)++first;
  int end=first;while(end<job.words.size()&&job.words[end].line==line)++end;
  // Its place in Tesseract's order. Between two words that are next to each
  // other in that order, it goes between them. Otherwise it is at the edge of
  // a run of the other direction (a left-to-right run inside a right-to-left
  // paragraph, or the reverse) and reads in the paragraph's direction: after
  // the whole run of the word it follows in that direction, or first on the
  // line when no word comes before it.
  const bool rightToLeft=job.lines[line].rightToLeft;int at=first;
  // The words beside it on the page that decide the joins with the word before
  // it and the word after it in that order: at a run's edge those are the run's
  // nearest word and the word on the other side, not the run's far end.
  int touchBefore=-1,touchAfter=-1;
  {
   const int middle=glyph.center().x();int left=-1,right=-1;
   for(int i=first;i<end;++i){
    const int x=job.words[i].rect.center().x();
    if(x<middle&&(left<0||x>job.words[left].rect.center().x()))left=i;
    if(x>=middle&&(right<0||x<job.words[right].rect.center().x()))right=i;
   }
   if(left>=0&&right>=0&&std::abs(left-right)==1){at=std::max(left,right);touchBefore=std::min(left,right);touchAfter=at;}
   else if(const int before=rightToLeft?right:left;before>=0){
    // The run of the other direction it ends: words onward in Tesseract's
    // order that step across the page against the paragraph.
    int last=before;
    const auto against=[&](int a,int b){return rightToLeft?job.words[b].rect.center().x()>job.words[a].rect.center().x():job.words[b].rect.center().x()<job.words[a].rect.center().x();};
    while(last+1<end&&against(last,last+1))++last;
    at=last+1;touchBefore=before;touchAfter=rightToLeft?left:right;
   }else touchAfter=rightToLeft?left:right;
  }
  Word emoji;emoji.text=hit.text;emoji.rect=glyph;emoji.line=line;emoji.lineHeight=job.lines[line].rect.height();emoji.emoji=true;
  emoji.confidence=float(std::clamp(100*(1-5*hit.distance),1.0,99.0));
  // Almost no gap to a neighbour, under a quarter of the glyph's height: no space
  // between them. Measured on rendered lines: a glued emoji's gap was a median
  // 0.15 of its height, and one after a space 0.25 or more but for 1 in 248.
  const auto gap=[&](const QRect& word){return std::max(word.left()-glyph.right()-1,glyph.left()-word.right()-1);};
  const int tight=std::max(1,glyph.height()/4);
  if(at>first&&touchBefore>=0&&gap(job.words[touchBefore].rect)<tight)emoji.attach=true;
  if(at<end&&touchAfter>=0&&gap(job.words[touchAfter].rect)<tight)job.words[at].attach=true;
  job.words.insert(at,emoji);
 }
}
void applyCopyFilter(Recognition& job,const GreyView& source,const CopyFilter& filter){
 for(auto& word:job.words){
  word.kept=true;
  // An emoji the colour plane named is copied whatever Tesseract would make of its pixels.
  if(word.emoji)continue;
  if(word.lineHeight<filter.minimumLineHeight){word.kept=false;continue;}
  const auto points=word.text.toUcs4();
  if(points.size()<=2&&word.confidence<filter.confidence){word.kept=false;continue;}
  if(points.size()==1&&!QChar::isLetterOrNumber(points[0])&&(!codeSymbol(points[0])||word.confidence<filter.symbolConfidence)){word.kept=false;continue;}
  bool alphanumeric=false,foreign=false;
  for(const auto c:points){if(QChar::isLetterOrNumber(c))alphanumeric=true;else if(!codeSymbol(c))foreign=true;}
  if(!alphanumeric&&foreign){word.kept=false;continue;}
  // Contrast against the word's own background: the median of its box.
  const QRect r=word.rect.intersected(QRect(0,0,source.width,source.height));
  if(r.isEmpty()){word.kept=false;continue;}
  std::array<int,256> histogram{};
  for(int y=r.top();y<=r.bottom();++y){const auto* row=source.row(y);for(int x=r.left();x<=r.right();++x)++histogram[row[x]];}
  const qint64 area=qint64(r.width())*r.height();qint64 seen=0;int background=0;
  for(int i=0;i<256;++i){seen+=histogram[i];if(seen*2>=area){background=i;break;}}
  qint64 strong=0;for(int i=0;i<256;++i)if(std::abs(i-background)>=filter.contrast)strong+=histogram[i];
  if(strong<std::max<qint64>(3,area/200))word.kept=false;
 }
}
QString copiedText(const Recognition& job,QList<int>* lineTops){
 QString text;int line=-1,block=-1;
 for(qsizetype i=0;i<job.words.size();++i){
  const auto& word=job.words[i];
  if(!word.kept)continue;
  if(word.line!=line){
   const int wordBlock=job.lines.value(word.line).block,top=job.lines.value(word.line).rect.top();
   if(line>=0){text+=wordBlock!=block?QStringLiteral("\n\n"):QStringLiteral("\n");if(wordBlock!=block&&lineTops)lineTops->append(top);}
   if(lineTops)lineTops->append(top);
   line=word.line;block=wordBlock;
  }else if(!(word.attach&&i>0&&job.words[i-1].kept&&job.words[i-1].line==word.line))text+=QLatin1Char(' ');
  // A join counts only with the word it was made with: when that one was
  // dropped, the space stays.
  text+=word.text;
 }
 return text;
}
}
