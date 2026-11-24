#include "Script.h"
#include "Layout.h"
#include <algorithm>
#include <cstring>

namespace putgd::ocr {
namespace {
bool lettered(const QString& text){
 return std::any_of(text.cbegin(),text.cend(),[](QChar c){return (c>=QLatin1Char('a')&&c<=QLatin1Char('z'))||(c>=QLatin1Char('A')&&c<=QLatin1Char('Z'));});
}
// Written without spaces between words.
bool unspaced(char32_t c){
 if((c>=0x3000&&c<=0x303F)||(c>=0xFF00&&c<=0xFF60)||(c>=0xFF61&&c<=0xFF9F))return true;  // CJK punctuation, full and half width forms
 switch(QChar::script(c)){
  case QChar::Script_Han:case QChar::Script_Hiragana:case QChar::Script_Katakana:case QChar::Script_Thai:case QChar::Script_Lao:case QChar::Script_Khmer:case QChar::Script_Myanmar:return true;
  default:return false;
 }
}
char32_t firstCharacter(const QString& text){
 if(text.isEmpty())return 0;
 if(text.size()>1&&text.at(0).isHighSurrogate()&&text.at(1).isLowSurrogate())return QChar::surrogateToUcs4(text.at(0),text.at(1));
 return text.at(0).unicode();
}
char32_t lastCharacter(const QString& text){
 if(text.isEmpty())return 0;
 const auto n=text.size();
 if(n>1&&text.at(n-1).isLowSurrogate()&&text.at(n-2).isHighSurrogate())return QChar::surrogateToUcs4(text.at(n-2),text.at(n-1));
 return text.at(n-1).unicode();
}
}
QList<Segment> lineSegments(const Recognition& job){
 QList<Segment> out;
 const auto close=[&](Segment& s){
  if(s.last<=s.first)return;
  double sum=0;int letters=0;QRect box;
  for(int i=s.first;i<s.last;++i){const auto& w=job.words.at(i);sum+=w.confidence;box=box.isNull()?w.rect:box.united(w.rect);if(lettered(w.text))++letters;}
  s.rect=box;s.confidence=float(sum/(s.last-s.first));
  s.garbled=letters>=garbledLetteredWords&&s.confidence<garbledConfidence;
  out.append(s);
 };
 Segment current;current.first=current.last=-1;
 for(int i=0;i<job.words.size();++i){
  const auto& w=job.words.at(i);
  // Emoji are not Tesseract's: they take no part.
  if(w.emoji||w.reread){if(current.first>=0){close(current);current.first=current.last=-1;}continue;}
  if(current.first>=0){
   const auto& previous=job.words.at(current.last-1);
   const int height=std::max({1,w.rect.height(),previous.rect.height()});
   // The gap either way: a right-to-left line runs leftwards.
   const int gap=std::max(w.rect.left()-previous.rect.right(),previous.rect.left()-w.rect.right());
   if(w.line==current.line&&gap<3*height){current.last=i+1;continue;}
   close(current);
  }
  current.line=w.line;current.first=i;current.last=i+1;
 }
 if(current.first>=0)close(current);
 return out;
}
QList<Segment> garbledSegments(const Recognition& job){
 int words=0;for(const auto& w:job.words)if(!w.emoji&&!w.reread)++words;
 if(words==0)return {};
 QList<Segment> garbled;int held=0;
 for(const auto& s:lineSegments(job))if(s.garbled){garbled.append(s);held+=s.last-s.first;}
 if(garbled.size()<garbledSegmentsNeeded||double(held)<garbledShare*words)return {};
 return garbled;
}
QString languageForScript(const QString& script){
 struct Entry{const char* script;const char* language;};
 // OSD names Japanese text Japanese, Hiragana or Katakana by its kana, and
 // Chinese Han. Korean is Hangul.
 static constexpr Entry table[]={{"Han","chi_sim"},{"Japanese","jpn"},{"Hiragana","jpn"},{"Katakana","jpn"},{"Hangul","kor"},{"Korean","kor"},
                                 {"Cyrillic","rus"},{"Arabic","ara"},{"Hebrew","heb"},{"Devanagari","hin"},{"Thai","tha"},{"Greek","ell"}};
 const auto name=script.toLatin1();
 for(const auto& e:table)if(name==e.script)return QString::fromLatin1(e.language);
 return {};
}
QStringList detectionLanguages(){return {"chi_sim","jpn","kor","rus","ara","heb","hin","tha","ell"};}
bool keepReread(double before,double after){return after-before>=double(keepRise);}
QRect paddedPiece(const QRect& rect,const QRect& bounds){
 const int pad=std::max(4,rect.height()/3);
 return rect.adjusted(-pad,-pad,pad,pad).intersected(bounds);
}
Composite composite(const QList<PieceSource>& pieces,int gap){
 Composite out;QList<GreyImage> copies;int width=1,height=0;
 for(const auto& p:pieces){
  const QRect r=p.rect.intersected(QRect(0,0,p.view.width,p.view.height));
  GreyImage copy;
  if(!r.isEmpty()){
   const GreyView columns{p.view.row(0)+r.left(),r.width(),p.view.height,p.view.stride};
   copy=bandImage(columns,Band{r.top(),r.bottom()+1,false},p.scale,0,{},p.background,p.invert);
  }
  if(!out.tops.isEmpty())height+=gap;
  out.tops.append(height);out.heights.append(copy.height);out.scales.append(p.scale);
  height+=copy.height;width=std::max(width,copy.width);copies.append(std::move(copy));
 }
 out.image.width=width;out.image.height=std::max(1,height);
 out.image.pixels=QByteArray(qsizetype(out.image.width)*out.image.height,char(255));
 for(int i=0;i<copies.size();++i){
  const auto& c=copies.at(i);
  // A light page's own background, not white, beside a narrower piece: each
  // line keeps its own background to its right edge only.
  for(int y=0;y<c.height;++y)std::memcpy(out.image.row(out.tops.at(i)+y),c.pixels.constData()+qsizetype(y)*c.width,size_t(c.width));
 }
 return out;
}
int pieceAt(const Composite& composite,const QRect& box){
 const int y=box.center().y();
 for(int i=0;i<composite.tops.size();++i)if(y>=composite.tops.at(i)&&y<composite.tops.at(i)+composite.heights.at(i))return i;
 return -1;
}
QRect pieceToSource(const Composite& composite,int index,const PieceSource& piece,const QRect& box){
 const QRect r=piece.rect.intersected(QRect(0,0,piece.view.width,piece.view.height));
 const double s=composite.scales.value(index,1);const int top=composite.tops.value(index);
 const int left=int(std::floor(box.left()/s)),right=int(std::ceil((box.left()+box.width())/s));
 const int y0=int(std::floor((box.top()-top)/s)),y1=int(std::ceil((box.top()+box.height()-top)/s));
 const int l=std::clamp(left,0,r.width()-1),t=std::clamp(y0,0,r.height()-1);
 const int rr=std::clamp(right-1,l,r.width()-1),b=std::clamp(y1-1,t,r.height()-1);
 return QRect(QPoint(r.left()+l,r.top()+t),QPoint(r.left()+rr,r.top()+b));
}
void joinUnspaced(Recognition& job){
 for(int i=1;i<job.words.size();++i){
  auto& w=job.words[i];const auto& before=job.words.at(i-1);
  if(w.line!=before.line||w.attach)continue;
  if(unspaced(lastCharacter(before.text))&&unspaced(firstCharacter(w.text)))w.attach=true;
 }
}
}
