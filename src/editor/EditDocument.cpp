#include "EditDocument.h"
#include <QBuffer>
#include <QColorSpace>
#include <QFont>
#include <QFontMetrics>
#include <QImageWriter>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <algorithm>
#include <cstring>
#include <vector>

namespace putgd::editor {
namespace {
QRect normalizedBox(const QPoint& a,const QPoint& b){return QRect(QPoint(std::min(a.x(),b.x()),std::min(a.y(),b.y())),QPoint(std::max(a.x(),b.x()),std::max(a.y(),b.y())));}
// Sliding-window box blur of one channel-interleaved line with clamped edges.
void boxLine(QRgb* line,int count,qsizetype step,int radius,std::vector<QRgb>& scratch){
 if(count<2||radius<1)return;
 scratch.resize(size_t(count));
 for(int i=0;i<count;++i)scratch[size_t(i)]=line[qsizetype(i)*step];
 const int window=2*radius+1;
 qint64 a=0,r=0,g=0,b=0;
 auto at=[&](int i)->QRgb{return scratch[size_t(std::clamp(i,0,count-1))];};
 for(int i=-radius;i<=radius;++i){const auto p=at(i);a+=qAlpha(p);r+=qRed(p);g+=qGreen(p);b+=qBlue(p);}
 for(int i=0;i<count;++i){
  line[qsizetype(i)*step]=qRgba(int(r/window),int(g/window),int(b/window),int(a/window));
  const auto out=at(i-radius),in=at(i+radius+1);
  a+=qAlpha(in)-qAlpha(out);r+=qRed(in)-qRed(out);g+=qGreen(in)-qGreen(out);b+=qBlue(in)-qBlue(out);
 }
}
QColor contrast(const QColor& color){return color.lightnessF()>0.6?QColor(20,20,20):QColor(255,255,255);}
}

QRect Shape::bounds() const {
 if(kind==Kind::Arrow){const int pad=std::max(12,width*5);return normalizedBox(from,to).adjusted(-pad,-pad,pad,pad);}
 if(kind==Kind::Rectangle){const int pad=width;return rect.adjusted(-pad,-pad,pad,pad);}
 if(kind==Kind::Text){const int pad=std::max(2,fontPixels/6);return rect.adjusted(-pad,-pad,pad,pad);}
 return rect;
}

EditDocument::EditDocument(const QImage& source){
 QImage image=source;
 if(image.colorSpace().isValid()&&image.colorSpace()!=QColorSpace(QColorSpace::SRgb))image.convertToColorSpace(QColorSpace::SRgb);
 base_=image.convertToFormat(image.hasAlphaChannel()?QImage::Format_ARGB32:QImage::Format_RGB32);
 state_.crop=base_.rect();
}

const Shape* EditDocument::find(quint64 id) const {for(const auto& shape:state_.shapes)if(shape.id==id)return &shape;return nullptr;}

QRect EditDocument::clampRect(const QRect& rect,const QRect& bounds){return rect.normalized().intersected(bounds);}

qint64 EditDocument::bytesOf(const State& state){
 qint64 total=qint64(sizeof(State))+qint64(state.shapes.size())*qint64(sizeof(Shape));
 for(const auto& shape:state.shapes)total+=qint64(shape.text.size())*qint64(sizeof(QChar));
 return total;
}

void EditDocument::push(State before){
 if(before==state_)return;
 for(const auto& state:redo_)historyBytes_-=bytesOf(state);
 redo_.clear();
 historyBytes_+=bytesOf(before);undo_.append(std::move(before));
 // Evicting the oldest step only shortens undo; the visible state never changes.
 while(!undo_.isEmpty()&&(undo_.size()>maximumHistory||historyBytes_>maximumHistoryBytes)){historyBytes_-=bytesOf(undo_.first());undo_.removeFirst();truncated_=true;}
}

quint64 EditDocument::add(Shape shape){
 shape.id=nextId_++;
 shape.text.truncate(maximumTextCharacters);
 shape.strength=std::clamp(shape.strength,minimumBlur,maximumBlur);
 auto before=state_;state_.shapes.append(shape);if(!gesture_)push(std::move(before));
 return shape.id;
}
bool EditDocument::replace(const Shape& changed){
 for(auto& shape:state_.shapes)if(shape.id==changed.id){
  auto next=changed;next.text.truncate(maximumTextCharacters);next.strength=std::clamp(next.strength,minimumBlur,maximumBlur);
  if(shape==next)return false;
  auto before=state_;shape=next;if(!gesture_)push(std::move(before));return true;
 }
 return false;
}
void EditDocument::preview(const Shape& shape){const bool live=gesture_;gesture_=true;replace(shape);gesture_=live;}
bool EditDocument::remove(quint64 id){
 for(qsizetype i=0;i<state_.shapes.size();++i)if(state_.shapes[i].id==id){auto before=state_;state_.shapes.removeAt(i);if(!gesture_)push(std::move(before));return true;}
 return false;
}
bool EditDocument::setCrop(const QRect& requested){
 const auto crop=clampRect(requested,base_.rect());
 if(crop.width()<1||crop.height()<1||crop==state_.crop)return false;
 auto before=state_;state_.crop=crop;if(!gesture_)push(std::move(before));return true;
}
void EditDocument::beginGesture(){if(!gesture_){gestureStart_=state_;gesture_=true;}}
void EditDocument::commitGesture(){if(!gesture_)return;gesture_=false;push(std::exchange(gestureStart_,{}));}
void EditDocument::cancelGesture(){if(!gesture_)return;gesture_=false;state_=std::exchange(gestureStart_,{});}
bool EditDocument::undo(){
 if(gesture_||undo_.isEmpty())return false;
 historyBytes_-=bytesOf(undo_.last());auto previous=undo_.takeLast();
 historyBytes_+=bytesOf(state_);redo_.append(std::exchange(state_,std::move(previous)));return true;
}
bool EditDocument::redo(){
 if(gesture_||redo_.isEmpty())return false;
 historyBytes_-=bytesOf(redo_.last());auto next=redo_.takeLast();
 historyBytes_+=bytesOf(state_);undo_.append(std::exchange(state_,std::move(next)));return true;
}

void EditDocument::blurInPlace(QImage& image,const QRect& requested,int radius){
 const auto area=clampRect(requested,image.rect());
 if(area.isEmpty()||radius<1||image.depth()!=32)return;
 // Three box passes approximate a Gaussian; each pass reads only this area.
 const int box=std::max(1,radius/2);
 std::vector<QRgb> scratch;
 const auto stride=image.bytesPerLine()/qsizetype(sizeof(QRgb));
 for(int pass=0;pass<3;++pass){
  for(int y=area.top();y<=area.bottom();++y)boxLine(reinterpret_cast<QRgb*>(image.scanLine(y))+area.left(),area.width(),1,box,scratch);
  for(int x=area.left();x<=area.right();++x)boxLine(reinterpret_cast<QRgb*>(image.scanLine(area.top()))+x,area.height(),stride,box,scratch);
 }
}

void EditDocument::applyRedactions(QImage& image,const QRect& within) const {
 // Integer source-pixel rectangles, painted with Source composition and no
 // antialiasing, replace every covered value with opaque black.
 QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_Source);painter.setRenderHint(QPainter::Antialiasing,false);
 for(const auto& shape:state_.shapes)if(shape.kind==Shape::Kind::Redact){
  const auto area=clampRect(shape.rect,within);if(area.isEmpty())continue;
  painter.fillRect(area.translated(-within.topLeft()),QColor(0,0,0,255));
 }
}

void EditDocument::paintAnnotation(QPainter& painter,const Shape& shape){
 painter.save();painter.setRenderHint(QPainter::Antialiasing,true);painter.setRenderHint(QPainter::TextAntialiasing,true);
 switch(shape.kind){
 case Shape::Kind::Rectangle:{
  QPen pen(shape.color,shape.width);pen.setJoinStyle(Qt::MiterJoin);painter.setPen(pen);painter.setBrush(Qt::NoBrush);
  painter.drawRect(QRectF(shape.rect).adjusted(shape.width/2.0,shape.width/2.0,-shape.width/2.0,-shape.width/2.0));break;
 }
 case Shape::Kind::Highlight:
  // A translucent marker stays visible on dark and light captures and never hides text.
  painter.fillRect(shape.rect,QColor(shape.color.red(),shape.color.green(),shape.color.blue(),105));break;
 case Shape::Kind::Arrow:{
  const QLineF line(shape.from,shape.to);if(line.length()<1)break;
  const double head=std::max(12.0,shape.width*4.0),angle=std::atan2(-line.dy(),line.dx());
  const QPointF tip=shape.to;
  const QPointF left=tip-QPointF(std::sin(angle+M_PI/3)*head,std::cos(angle+M_PI/3)*head);
  const QPointF right=tip-QPointF(std::sin(angle+M_PI-M_PI/3)*head,std::cos(angle+M_PI-M_PI/3)*head);
  const QPointF base=(left+right)/2;
  QPen pen(shape.color,shape.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);painter.setPen(pen);
  if(line.length()>head)painter.drawLine(QLineF(shape.from,base));
  painter.setPen(Qt::NoPen);painter.setBrush(shape.color);painter.drawPolygon(QPolygonF{tip,left,right});break;
 }
 case Shape::Kind::Text:{
  if(shape.text.isEmpty())break;
  QFont font;font.setPixelSize(std::max(4,shape.fontPixels));font.setBold(true);
  QPainterPath path;path.addText(QPointF(shape.rect.left(),shape.rect.top()+QFontMetrics(font).ascent()),font,shape.text);
  painter.setPen(QPen(contrast(shape.color),std::max(2.0,shape.fontPixels/7.0),Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.setBrush(Qt::NoBrush);painter.drawPath(path);
  painter.setPen(Qt::NoPen);painter.setBrush(shape.color);painter.drawPath(path);break;
 }
 case Shape::Kind::Redact:case Shape::Kind::Blur:break;
 }
 painter.restore();
}

QImage EditDocument::render() const {
 const auto crop=state_.crop;
 // A fresh raster carries no reader text, color space or source container data.
 QImage out(crop.size(),base_.format());if(out.isNull())return {};
 const auto rowBytes=size_t(crop.width())*sizeof(QRgb);
 for(int y=0;y<crop.height();++y)std::memcpy(out.scanLine(y),base_.constScanLine(crop.top()+y)+qsizetype(crop.left())*qsizetype(sizeof(QRgb)),rowBytes);
 applyRedactions(out,crop);
 for(const auto& shape:state_.shapes)if(shape.kind==Shape::Kind::Blur)blurInPlace(out,shape.rect.translated(-crop.topLeft()),shape.strength);
 {
  QPainter painter(&out);painter.translate(-crop.topLeft());
  for(const auto& shape:state_.shapes)if(shape.kind!=Shape::Kind::Redact&&shape.kind!=Shape::Kind::Blur)paintAnnotation(painter,shape);
 }
 if(out.hasAlphaChannel())for(int y=0;y<out.height();++y){auto* line=reinterpret_cast<QRgb*>(out.scanLine(y));for(int x=0;x<out.width();++x)if(qAlpha(line[x])==0)line[x]=0;}
 return out;
}

QByteArray EditDocument::exportPng() const {
 QByteArray encoded;
 {
  auto image=render();if(image.isNull())return {};
  QBuffer buffer(&encoded);if(!buffer.open(QIODevice::WriteOnly))return {};
  QImageWriter writer(&buffer,"png");if(!writer.write(image))return {};
 }
 return encoded;
}

QImage EditDocument::blurPreview(const Shape& shape,double scale) const {
 const auto area=clampRect(shape.rect,state_.crop);if(area.isEmpty())return {};
 // Bound preview work to about 1.5 megapixels regardless of the selected area.
 constexpr double previewPixels=1.5e6;
 scale=std::min({1.0,scale,std::sqrt(previewPixels/(double(area.width())*area.height()))});
 const QSize target(std::max(1,int(std::lround(area.width()*scale))),std::max(1,int(std::lround(area.height()*scale))));
 const QImage view(base_.constScanLine(area.top())+qsizetype(area.left())*qsizetype(sizeof(QRgb)),area.width(),area.height(),base_.bytesPerLine(),base_.format());
 auto preview=target==area.size()?view.copy():view.scaled(target,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
 preview=preview.convertToFormat(base_.format());
 const double sx=double(target.width())/area.width(),sy=double(target.height())/area.height();
 {
  QPainter painter(&preview);painter.setCompositionMode(QPainter::CompositionMode_Source);
  for(const auto& other:state_.shapes)if(other.kind==Shape::Kind::Redact){
   const auto overlap=other.rect.intersected(area);if(overlap.isEmpty())continue;
   const QRectF mapped((overlap.left()-area.left())*sx,(overlap.top()-area.top())*sy,overlap.width()*sx,overlap.height()*sy);
   painter.fillRect(mapped.toAlignedRect(),Qt::black);
  }
 }
 blurInPlace(preview,preview.rect(),std::max(1,int(std::lround(shape.strength*std::min(sx,sy)))));
 return preview;
}
}
