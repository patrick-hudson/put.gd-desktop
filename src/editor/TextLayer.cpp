#include "TextLayer.h"
#include <QDataStream>
#include <QIODevice>
#include <QPolygonF>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace putgd::editor {
QByteArray TextLayer::inputSignature(const EditDocument& doc) {
 QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);
 out<<doc.crop();
 for(const auto& shape:doc.shapes())if(shape.kind==Shape::Kind::Redact)out<<shape.id<<EditDocument::clampRect(shape.rect,doc.imageRect());
 return bytes;
}
bool TextLayer::touchesRedaction(const QRect& box,const QList<QRect>& redactions) {
 // Grown by a pixel, so a box that only shares an edge with a mark is dropped too.
 const auto grown=box.adjusted(-1,-1,1,1);
 return std::any_of(redactions.cbegin(),redactions.cend(),[&](const QRect& mark){return !mark.isEmpty()&&grown.intersects(mark);});
}
bool TextLayer::touchesRedaction(const QPolygon& quad,const QList<QRect>& redactions) {
 if(quad.isEmpty())return false;
 const QPolygonF shape(quad);
 for(const auto& mark:redactions){
  if(mark.isEmpty())continue;
  const QPolygonF area(QRectF(mark).adjusted(-1,-1,1,1));
  // Crossing, containing or contained, as QPolygonF::intersects counts it.
  if(shape.intersects(area))return true;
 }
 return false;
}
void TextLayer::exclude(QList<RecognizedWord>& words,QList<RecognizedCode>& codes,const QList<QRect>& redactions) {
 words.erase(std::remove_if(words.begin(),words.end(),[&](const RecognizedWord& word){return word.rect.isEmpty()||touchesRedaction(word.rect,redactions);}),words.end());
 codes.erase(std::remove_if(codes.begin(),codes.end(),[&](const RecognizedCode& code){return touchesRedaction(code.quad,redactions);}),codes.end());
 // A word starts a new line when it is not beside the line so far: its centre
 // is outside the height of that line's words, or it begins left of the word
 // before it. The whole line counts, not only the word before: recognition
 // gives a dash or a dot a box a few pixels high.
 int line=0;QRect current;
 for(qsizetype i=0;i<words.size();++i){
  const auto& now=words.at(i).rect;
  if(i>0){const int middle=now.center().y();
   if(middle<current.top()||middle>current.bottom()||now.left()<words.at(i-1).rect.left()){++line;current={};}}
  current=current.united(now);words[i].line=line;
 }
}
QString TextLayer::join(const QList<RecognizedWord>& words,int from,int to) {
 if(words.isEmpty())return {};
 const int first=std::clamp(qMin(from,to),0,int(words.size())-1),last=std::clamp(qMax(from,to),0,int(words.size())-1);
 QString text;
 for(int i=first;i<=last;++i){
  if(i>first)text+=words.at(i).line!=words.at(i-1).line?QChar('\n'):QChar(' ');
  text+=words.at(i).text;
 }
 return text;
}
void TextLayer::set(quint64 identity,QList<RecognizedWord> words,QList<RecognizedCode> codes) {
 identity_=identity;words_=std::move(words);codes_=std::move(codes);anchor_=focus_=-1;
}
void TextLayer::clear() {identity_=0;words_.clear();codes_.clear();anchor_=focus_=-1;}
int TextLayer::wordAt(const QPointF& image) const {
 const QPoint pixel(int(std::floor(image.x())),int(std::floor(image.y())));
 for(qsizetype i=0;i<words_.size();++i)if(words_.at(i).rect.contains(pixel))return int(i);
 return -1;
}
void TextLayer::select(int anchor,int focus) {
 const int count=int(words_.size());
 if(anchor<0||focus<0||anchor>=count||focus>=count){clearSelection();return;}
 anchor_=anchor;focus_=focus;
}
void TextLayer::extendTo(int focus) {if(anchor_<0)select(focus,focus);else select(anchor_,focus);}
void TextLayer::selectLine(int index) {
 if(index<0||index>=words_.size()){clearSelection();return;}
 const int line=words_.at(index).line;int first=index,last=index;
 while(first>0&&words_.at(first-1).line==line)--first;
 while(last+1<words_.size()&&words_.at(last+1).line==line)++last;
 select(first,last);
}
void TextLayer::selectAll() {if(words_.isEmpty())clearSelection();else select(0,int(words_.size())-1);}
void TextLayer::clearSelection() {anchor_=focus_=-1;}
}
