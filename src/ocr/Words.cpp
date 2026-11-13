#include "Words.h"
#include "core/TextJobProtocol.h"
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>

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
bool codeSymbol(char32_t c){
 static const char32_t symbols[]=U"{}()[]<>;:=+-*/\\|&!?#$%^~@,.'\"`_";
 for(const auto s:symbols)if(s&&s==c)return true;
 return false;
}
}
int mergeBand(Recognition& job,const BandText& band,const Band& placement,const Band* previous,int previousFirstWord,
               double scale,int border,int bandIndex,int imageWidth,int imageHeight){
 const int lineBase=int(job.lines.size());
 const int blockBase=job.lines.isEmpty()?0:job.lines.last().block+1;
 for(const auto& line:band.lines)job.lines.append({toSource(line.rect,placement,scale,border,imageWidth,imageHeight),blockBase+line.block,bandIndex});
 const bool seam=placement.seamAbove&&previous;
 if(seam){
  // The previous band was cut at its limit: its words on the cut edge are partial.
  for(int i=previousFirstWord;i<job.words.size();++i)if(job.words[i].rect.bottom()>=previous->bottom-1)job.words[i].text.clear();
 }
 const int firstNew=int(job.words.size());
 for(const auto& item:band.words){
  Word word;word.text=textjob::sanitizeText(item.text).trimmed();
  if(word.text.isEmpty())continue;
  word.rect=toSource(item.rect,placement,scale,border,imageWidth,imageHeight);
  word.confidence=item.confidence;word.line=lineBase+item.line;
  word.lineHeight=job.lines.value(word.line).rect.height();
  if(seam){
   if(word.rect.top()<=placement.top+1)continue;
   bool duplicate=false;
   if(word.rect.top()<previous->bottom){
    for(int i=previousFirstWord;i<firstNew;++i){
     auto& earlier=job.words[i];if(earlier.text.isEmpty()||overlap(earlier.rect,word.rect)<=0.5)continue;
     duplicate=true;
     if(word.confidence>earlier.confidence){earlier.text=word.text;earlier.rect=word.rect;earlier.confidence=word.confidence;}
     break;
    }
   }
   if(duplicate)continue;
  }
  job.words.append(std::move(word));
 }
 int removed=0;for(int i=previousFirstWord;i<firstNew;++i)if(job.words[i].text.isEmpty())++removed;
 job.words.erase(std::remove_if(job.words.begin()+std::min<qsizetype>(previousFirstWord,job.words.size()),job.words.end(),[](const Word& w){return w.text.isEmpty();}),job.words.end());
 return firstNew-removed;
}
void joinUrls(Recognition& job){
 static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]{1,15}:$")),url(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]{1,15}://"));
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
   if(sameLine&&(start||more)){
    last.text+=word.text;last.rect=last.rect.united(word.rect);last.confidence=std::min(last.confidence,word.confidence);
    continue;
   }
  }
  out.append(std::move(word));
 }
 job.words=std::move(out);
}
void applyCopyFilter(Recognition& job,const GreyView& source,const CopyFilter& filter){
 for(auto& word:job.words){
  word.kept=true;
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
 for(const auto& word:job.words){
  if(!word.kept)continue;
  if(word.line!=line){
   const int wordBlock=job.lines.value(word.line).block,top=job.lines.value(word.line).rect.top();
   if(line>=0){text+=wordBlock!=block?QStringLiteral("\n\n"):QStringLiteral("\n");if(wordBlock!=block&&lineTops)lineTops->append(top);}
   if(lineTops)lineTops->append(top);
   line=word.line;block=wordBlock;
  }else text+=QLatin1Char(' ');
  text+=word.text;
 }
 return text;
}
BandText joinParts(const QList<BandText>& parts){
 BandText out;int blockBase=0;
 for(const auto& part:parts){
  const int lineBase=int(out.lines.size());int blocks=0;
  for(const auto& line:part.lines){out.lines.append({line.rect,blockBase+line.block});blocks=std::max(blocks,line.block+1);}
  for(const auto& word:part.words){auto copy=word;copy.line+=lineBase;out.words.append(std::move(copy));}
  blockBase+=blocks;
 }
 return out;
}
}
