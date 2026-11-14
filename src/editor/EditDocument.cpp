#include "EditDocument.h"
#include "HighlightFilter.h"
#include <QBuffer>
#include <QColorSpace>
#include <QFont>
#include <QFontMetrics>
#include <QImageWriter>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
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
// A step's label and rim: white while it keeps the 3:1 large-text contrast on
// the disc (red, blue, black), else near-black (yellow, orange, green, white).
QColor stepInk(const QColor& color){
 auto channel=[](double c){return c<=0.04045?c/12.92:std::pow((c+0.055)/1.055,2.4);};
 const double luminance=0.2126*channel(color.redF())+0.7152*channel(color.greenF())+0.0722*channel(color.blueF());
 return 1.05/(luminance+0.05)>=3.0?QColor(255,255,255):QColor(20,20,20);
}
QPainterPath textPath(const Shape& shape){
 const auto font=textFont(shape.fontFamily,shape.fontPixels);QPainterPath path;
 path.addText(QPointF(shape.rect.left(),shape.rect.top()+QFontMetrics(font).ascent()),font,shape.text);return path;
}
}

QRect Shape::bounds() const {
 if(kind==Kind::Arrow){const int pad=std::max(12,width*5);return normalizedBox(from,to).adjusted(-pad,-pad,pad,pad);}
 if(kind==Kind::Line){const int pad=width;return normalizedBox(from,to).adjusted(-pad,-pad,pad,pad);}
 if(kind==Kind::Pen){const int pad=width;return box().adjusted(-pad,-pad,pad,pad);}
 if(kind==Kind::Rectangle||kind==Kind::Ellipse){const int pad=width;return rect.adjusted(-pad,-pad,pad,pad);}
 if(kind==Kind::Step){const int pad=std::max(2,rect.width()/12);return rect.adjusted(-pad,-pad,pad,pad);}
 if(kind==Kind::Text){const int pad=std::max(2,fontPixels/6);return rect.adjusted(-pad,-pad,pad,pad);}
 return rect;
}
QRect Shape::box() const {
 if(twoPoint())return normalizedBox(from,to);
 if(kind!=Kind::Pen)return rect;
 if(points.isEmpty())return {};
 int left=points.first().x(),right=left,top=points.first().y(),bottom=top;
 for(const auto& p:points){left=std::min(left,p.x());right=std::max(right,p.x());top=std::min(top,p.y());bottom=std::max(bottom,p.y());}
 return QRect(QPoint(left,top),QPoint(right,bottom));
}

QPainterPath stepLabelPath(const Shape& shape){
 const auto label=QString::number(shape.number);const int d=shape.rect.width();
 // Three digits get a smaller face so they stay inside the circle.
 const auto font=textFont({},std::max(4,int(std::lround(d*(label.size()>2?0.40:label.size()>1?0.48:0.56)))));
 QPainterPath path;path.addText(QPointF(0,0),font,label);
 const auto ink=path.boundingRect();
 return path.translated(QRectF(shape.rect).center()-ink.center());
}
QPainterPath strokePath(const QList<QPoint>& points){
 QPainterPath path;if(points.isEmpty())return path;
 path.moveTo(points.first());
 if(points.size()==1){path.lineTo(QPointF(points.first())+QPointF(0.01,0));return path;}
 // Quadratic segments through the midpoints keep corners soft without
 // overshooting the points the user drew.
 for(qsizetype i=1;i+1<points.size();++i)path.quadTo(points[i],(QPointF(points[i])+QPointF(points[i+1]))/2);
 path.lineTo(points.last());return path;
}
namespace {
double segmentDistance(const QPointF& p,const QPointF& a,const QPointF& b){
 const QPointF ab=b-a;const double length=QPointF::dotProduct(ab,ab);
 if(length<=0)return QLineF(p,a).length();
 const double t=std::clamp(QPointF::dotProduct(p-a,ab)/length,0.0,1.0);
 return QLineF(p,a+t*ab).length();
}
void simplify(const std::vector<QPointF>& points,size_t first,size_t last,double tolerance,std::vector<bool>& keep){
 double worst=0;size_t index=first;
 for(size_t i=first+1;i<last;++i){const double d=segmentDistance(points[i],points[first],points[last]);if(d>worst){worst=d;index=i;}}
 if(worst>tolerance){keep[index]=true;simplify(points,first,index,tolerance,keep);simplify(points,index,last,tolerance,keep);}
}
}
QList<QPoint> limitStroke(QList<QPoint> points){
 // Thin the interior evenly rather than cutting the end: a long live drag
 // keeps following the pointer, and both ends stay where they were drawn.
 while(points.size()>maximumPenPoints){QList<QPoint> fewer;fewer.reserve(points.size()/2+2);for(qsizetype i=0;i<points.size();++i)if(i%2==0||i==points.size()-1)fewer.append(points[i]);points=std::move(fewer);}
 return points;
}
QList<QPoint> smoothStroke(const QList<QPoint>& raw){
 if(raw.size()<3)return raw;
 // A five-point moving average (shorter at the ends) evens out hand jitter;
 // the first and last points stay where the stroke began and ended.
 std::vector<QPointF> smooth(size_t(raw.size()));
 for(qsizetype i=0;i<raw.size();++i){
  const qsizetype reach=std::min<qsizetype>({2,i,raw.size()-1-i});QPointF sum;
  for(qsizetype j=i-reach;j<=i+reach;++j)sum+=QPointF(raw[j]);
  smooth[size_t(i)]=sum/double(2*reach+1);
 }
 std::vector<bool> keep(smooth.size(),false);keep.front()=keep.back()=true;
 simplify(smooth,0,smooth.size()-1,0.5,keep);
 QList<QPoint> result;
 for(size_t i=0;i<smooth.size();++i)if(keep[i]){const QPoint p(int(std::lround(smooth[i].x())),int(std::lround(smooth[i].y())));if(result.isEmpty()||result.last()!=p)result.append(p);}
 return limitStroke(std::move(result));
}
QList<QPoint> fitStroke(const QList<QPoint>& points,const QRect& from,const QRect& box){
 QList<QPoint> result;result.reserve(points.size());
 const double sx=from.width()>1?double(box.width()-1)/(from.width()-1):1,sy=from.height()>1?double(box.height()-1)/(from.height()-1):1;
 for(const auto& p:points)result.append(QPoint(box.left()+int(std::lround((p.x()-from.left())*sx)),box.top()+int(std::lround((p.y()-from.top())*sy))));
 return result;
}

EditDocument::EditDocument(const QImage& source){
 QImage image=source;
 if(image.colorSpace().isValid()&&image.colorSpace()!=QColorSpace(QColorSpace::SRgb))image.convertToColorSpace(QColorSpace::SRgb);
 base_=image.convertToFormat(image.hasAlphaChannel()?QImage::Format_ARGB32:QImage::Format_RGB32);
 state_.crop=base_.rect();
}

const Shape* EditDocument::find(quint64 id) const {for(const auto& shape:state_.shapes)if(shape.id==id)return &shape;return nullptr;}

QRect EditDocument::clampRect(const QRect& rect,const QRect& bounds){return rect.normalized().intersected(bounds);}

QFont textFont(const QString& family,int pixels){
 QFont font;if(!family.isEmpty())font.setFamily(family);
 font.setPixelSize(std::max(4,pixels));font.setBold(true);return font;
}
QRectF textOutputBounds(const Shape& shape){
 const auto path=textPath(shape);if(path.isEmpty())return {};
 QPainterPathStroker stroke;stroke.setWidth(std::max(2.0,shape.fontPixels/7.0));stroke.setCapStyle(Qt::RoundCap);stroke.setJoinStyle(Qt::RoundJoin);
 return path.boundingRect().united(stroke.createStroke(path).boundingRect());
}
qint64 EditDocument::bytesOf(const State& state){
 qint64 total=qint64(sizeof(State))+qint64(state.shapes.size())*qint64(sizeof(Shape));
 for(const auto& shape:state.shapes)total+=qint64(shape.text.size()+shape.fontFamily.size())*qint64(sizeof(QChar))+qint64(shape.points.size())*qint64(sizeof(QPoint));
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
 shape.number=std::clamp(shape.number,1,maximumStepNumber);
 shape.points=limitStroke(std::move(shape.points));
 auto before=state_;state_.shapes.append(shape);++generation_;if(!gesture_)push(std::move(before));
 return shape.id;
}
bool EditDocument::replace(const Shape& changed){
 for(qsizetype i=0;i<state_.shapes.size();++i)if(state_.shapes.at(i).id==changed.id){
  auto next=changed;next.text.truncate(maximumTextCharacters);next.strength=std::clamp(next.strength,minimumBlur,maximumBlur);next.number=std::clamp(next.number,1,maximumStepNumber);next.points=limitStroke(std::move(next.points));
  if(state_.shapes.at(i)==next)return false;
  // Copy first, then write through the list: it detaches from the snapshot.
  // (A reference taken before the copy wrote into both, losing the step.)
  auto before=state_;state_.shapes[i]=next;++generation_;if(!gesture_)push(std::move(before));return true;
 }
 return false;
}
void EditDocument::preview(const Shape& shape){const bool live=gesture_;gesture_=true;replace(shape);gesture_=live;}
bool EditDocument::remove(quint64 id){
 for(qsizetype i=0;i<state_.shapes.size();++i)if(state_.shapes[i].id==id){auto before=state_;state_.shapes.removeAt(i);++generation_;if(!gesture_)push(std::move(before));return true;}
 return false;
}
bool EditDocument::setCrop(const QRect& requested){
 const auto crop=clampRect(requested,base_.rect());
 if(crop.width()<1||crop.height()<1||crop==state_.crop)return false;
 auto before=state_;state_.crop=crop;++generation_;if(!gesture_)push(std::move(before));return true;
}
void EditDocument::beginGesture(){if(!gesture_){gestureStart_=state_;gesture_=true;}}
void EditDocument::commitGesture(){if(!gesture_)return;gesture_=false;push(std::exchange(gestureStart_,{}));}
void EditDocument::cancelGesture(){if(!gesture_)return;gesture_=false;state_=std::exchange(gestureStart_,{});++generation_;}
bool EditDocument::undo(){
 if(gesture_||undo_.isEmpty())return false;
 historyBytes_-=bytesOf(undo_.last());auto previous=undo_.takeLast();
 historyBytes_+=bytesOf(state_);redo_.append(std::exchange(state_,std::move(previous)));++generation_;return true;
}
bool EditDocument::redo(){
 if(gesture_||redo_.isEmpty())return false;
 historyBytes_-=bytesOf(redo_.last());auto next=redo_.takeLast();
 historyBytes_+=bytesOf(state_);undo_.append(std::exchange(state_,std::move(next)));++generation_;return true;
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

void EditDocument::pixelateInPlace(QImage& image,const QRect& requested,const QPoint& origin,int block){
 const auto area=clampRect(requested,image.rect());
 if(area.isEmpty()||block<1||image.depth()!=32)return;
 // Blocks sit on the shape's own grid, so a crop never shifts them; each
 // averages only the pixels it covers inside the area, as blur reads.
 auto floorDiv=[](int a,int b){return a>=0?a/b:-((-a+b-1)/b);};
 const int firstColumn=floorDiv(area.left()-origin.x(),block),lastColumn=floorDiv(area.right()-origin.x(),block);
 const int columns=lastColumn-firstColumn+1;
 std::vector<qint64> sums(size_t(columns)*4);std::vector<QRgb> colors(static_cast<size_t>(columns));
 for(int rowTop=origin.y()+floorDiv(area.top()-origin.y(),block)*block;rowTop<=area.bottom();rowTop+=block){
  const int top=std::max(rowTop,area.top()),bottom=std::min(rowTop+block-1,area.bottom());
  std::fill(sums.begin(),sums.end(),0);
  for(int y=top;y<=bottom;++y){const auto* line=reinterpret_cast<const QRgb*>(image.constScanLine(y));for(int x=area.left();x<=area.right();++x){const auto c=size_t(floorDiv(x-origin.x(),block)-firstColumn)*4;const auto p=line[x];sums[c]+=qAlpha(p);sums[c+1]+=qRed(p);sums[c+2]+=qGreen(p);sums[c+3]+=qBlue(p);}}
  for(int column=0;column<columns;++column){
   const int left=std::max(origin.x()+(firstColumn+column)*block,area.left()),right=std::min(origin.x()+(firstColumn+column+1)*block-1,area.right());
   const qint64 count=qint64(right-left+1)*(bottom-top+1);const auto c=size_t(column)*4;
   colors[size_t(column)]=count>0?qRgba(int(sums[c+1]/count),int(sums[c+2]/count),int(sums[c+3]/count),int(sums[c]/count)):0;
  }
  for(int y=top;y<=bottom;++y){auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));for(int x=area.left();x<=area.right();++x)line[x]=colors[size_t(floorDiv(x-origin.x(),block)-firstColumn)];}
 }
}

void EditDocument::pixelatePreview(QImage& target,const QRect& window,const Shape& shape) const {
 const auto full=clampRect(shape.rect,state_.crop),area=full.intersected(window);if(area.isEmpty()||target.isNull())return;
 // Block colours come from full-resolution pixels (the redacted base): every
 // whole block the window touches, so the preview shows the export's blocks
 // at any zoom, even where the window cuts a block.
 const int b=std::max(1,shape.strength);const auto origin=shape.rect.topLeft();
 auto down=[b](int v,int o){const int d=v-o;return o+(d>=0?d/b:-((-d+b-1)/b))*b;};
 const QRect grown=QRect(QPoint(down(area.left(),origin.x()),down(area.top(),origin.y())),QPoint(down(area.right(),origin.x())+b-1,down(area.bottom(),origin.y())+b-1)).intersected(full);
 QImage blocks(grown.size(),base_.format());if(blocks.isNull())return;
 for(int y=0;y<grown.height();++y)std::memcpy(blocks.scanLine(y),base_.constScanLine(grown.top()+y)+qsizetype(grown.left())*qsizetype(sizeof(QRgb)),size_t(grown.width())*sizeof(QRgb));
 {QPainter painter(&blocks);painter.setCompositionMode(QPainter::CompositionMode_Source);for(const auto& other:state_.shapes)if(other.kind==Shape::Kind::Redact){const auto overlap=other.rect.intersected(grown);if(!overlap.isEmpty())painter.fillRect(overlap.translated(-grown.topLeft()),Qt::black);}}
 pixelateInPlace(blocks,blocks.rect(),origin-grown.topLeft(),b);
 // Sample the block raster onto the preview's pixels.
 const double sx=double(target.width())/window.width(),sy=double(target.height())/window.height();
 const QRect mapped=QRectF((area.left()-window.left())*sx,(area.top()-window.top())*sy,area.width()*sx,area.height()*sy).toAlignedRect().intersected(target.rect());
 for(int ty=mapped.top();ty<=mapped.bottom();++ty){
  const int iy=std::clamp(int(std::floor(window.top()+(ty+0.5)/sy)),area.top(),area.bottom())-grown.top();
  auto* out=reinterpret_cast<QRgb*>(target.scanLine(ty));const auto* in=reinterpret_cast<const QRgb*>(blocks.constScanLine(iy));
  for(int tx=mapped.left();tx<=mapped.right();++tx)out[tx]=in[std::clamp(int(std::floor(window.left()+(tx+0.5)/sx)),area.left(),area.right())-grown.left()];
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

void EditDocument::paintAnnotation(QPainter& painter,const Shape& shape,const QPainterPath* frozenText){
 painter.save();painter.setRenderHint(QPainter::Antialiasing,true);painter.setRenderHint(QPainter::TextAntialiasing,true);
 switch(shape.kind){
 case Shape::Kind::Rectangle:{
  QPen pen(shape.color,shape.width);pen.setJoinStyle(Qt::MiterJoin);painter.setPen(pen);painter.setBrush(Qt::NoBrush);
  painter.drawRect(QRectF(shape.rect).adjusted(shape.width/2.0,shape.width/2.0,-shape.width/2.0,-shape.width/2.0));break;
 }
 case Shape::Kind::Highlight:
  // A pixel filter, applied by render() and the canvas tile, not a paint.
  break;
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
 case Shape::Kind::Ellipse:{
  QPen pen(shape.color,shape.width);painter.setPen(pen);painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(QRectF(shape.rect).adjusted(shape.width/2.0,shape.width/2.0,-shape.width/2.0,-shape.width/2.0));break;
 }
 case Shape::Kind::Line:{
  if(shape.from==shape.to)break;
  painter.setPen(QPen(shape.color,shape.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.drawLine(QLineF(shape.from,shape.to));break;
 }
 case Shape::Kind::Pen:{
  if(shape.points.isEmpty())break;
  painter.setPen(QPen(shape.color,shape.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.setBrush(Qt::NoBrush);painter.drawPath(strokePath(shape.points));break;
 }
 case Shape::Kind::Step:{
  // A filled disc in the colour with a contrasting rim and label, so it
  // reads on light and dark content alike.
  const QRectF disc(shape.rect);if(disc.width()<1)break;
  const double rim=std::max(1.5,disc.width()/16.0);
  const auto ink=stepInk(shape.color);
  painter.setPen(QPen(ink,rim));painter.setBrush(shape.color);painter.drawEllipse(disc.adjusted(rim/2,rim/2,-rim/2,-rim/2));
  const auto path=frozenText?*frozenText:stepLabelPath(shape);
  painter.setPen(Qt::NoPen);painter.setBrush(ink);painter.drawPath(path);break;
 }
 case Shape::Kind::Text:{
  if(shape.text.isEmpty())break;
  const auto path=frozenText?*frozenText:textPath(shape);
  painter.setPen(QPen(contrast(shape.color),std::max(2.0,shape.fontPixels/7.0),Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.setBrush(Qt::NoBrush);painter.drawPath(path);
  painter.setPen(Qt::NoPen);painter.setBrush(shape.color);painter.drawPath(path);break;
 }
 case Shape::Kind::Redact:case Shape::Kind::Blur:break;
 }
 painter.restore();
}

EditDocument EditDocument::snapshotForExport() const {
 // Qt's FreeType outlines can change after a canvas repaint, even with the
 // same font and draft. Reuse one set for every copy and Upload of that draft.
 const QFont defaultFont;
 if(textPathsGeneration_!=generation_||textPathsFont_!=defaultFont){
  textPaths_.clear();textPathsGeneration_=generation_;textPathsFont_=defaultFont;
  for(const auto& shape:state_.shapes){if(shape.kind==Shape::Kind::Text)textPaths_.insert(shape.id,textPath(shape));else if(shape.kind==Shape::Kind::Step)textPaths_.insert(shape.id,stepLabelPath(shape));}
 }
 // A worker keeps the GUI's resolved outlines regardless of its default font.
 auto snapshot=*this;snapshot.textPathsFrozen_=true;return snapshot;
}

QImage EditDocument::render() const {
 const auto crop=state_.crop;
 // A fresh raster carries no reader text, color space or source container data.
 QImage out(crop.size(),base_.format());if(out.isNull())return {};
 const auto rowBytes=size_t(crop.width())*sizeof(QRgb);
 for(int y=0;y<crop.height();++y)std::memcpy(out.scanLine(y),base_.constScanLine(crop.top()+y)+qsizetype(crop.left())*qsizetype(sizeof(QRgb)),rowBytes);
 applyRedactions(out,crop);
 // Highlights decide their colours on the redacted base, before blur, the
 // same pixels the canvas preview reads; they tint the blurred result.
 QList<std::pair<QRect,highlight::Plan>> highlights;
 for(const auto& shape:state_.shapes)if(shape.kind==Shape::Kind::Highlight){const auto area=clampRect(shape.rect,crop).translated(-crop.topLeft());if(!area.isEmpty())highlights.append({area,highlight::plan(out,area,{},shape.color)});}
 for(const auto& shape:state_.shapes)if(shape.kind==Shape::Kind::Blur){
  if(shape.pixelate)pixelateInPlace(out,shape.rect.translated(-crop.topLeft()),shape.rect.topLeft()-crop.topLeft(),shape.strength);
  else blurInPlace(out,shape.rect.translated(-crop.topLeft()),shape.strength);
 }
 if(highlights.size()==1)highlight::apply(out,highlights.first().first,highlights.first().second);
 else if(!highlights.isEmpty()){
  // Each highlight tints the pre-highlight pixels, and the later one wins
  // where they overlap, as the canvas paints its tiles.
  const QImage before=out.copy();
  for(const auto& [area,plan]:highlights){QImage tile=before.copy(area);highlight::apply(tile,tile.rect(),plan);QPainter painter(&out);painter.setCompositionMode(QPainter::CompositionMode_Source);painter.drawImage(area.topLeft(),tile);}
 }
 {
  QPainter painter(&out);painter.translate(-crop.topLeft());
  for(const auto& shape:state_.shapes)if(shape.kind!=Shape::Kind::Redact&&shape.kind!=Shape::Kind::Blur){const auto path=textPaths_.constFind(shape.id);paintAnnotation(painter,shape,textPathsGeneration_==generation_&&(textPathsFrozen_||textPathsFont_==QFont())&&path!=textPaths_.cend()?&path.value():nullptr);}
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

int EditDocument::nextStepNumber() const {
 int highest=0;for(const auto& shape:state_.shapes)if(shape.kind==Shape::Kind::Step)highest=std::max(highest,shape.number);
 return std::min(highest+1,maximumStepNumber);
}

QList<QRect> EditDocument::redactionRects() const {
 QList<QRect> rects;for(const auto& shape:state_.shapes)if(shape.kind==Shape::Kind::Redact)rects.append(clampRect(shape.rect,base_.rect()));return rects;
}

QRect EditDocument::highlightWindow(const Shape& shape) const {
 // The box plus the reach, inside the box's crop, of every blur crossing its
 // edge (three box passes of radius/2, plus one): what the preview must hold.
 const auto area=clampRect(shape.rect,state_.crop);QRect window=area;
 if(area.isEmpty())return window;
 for(const auto& other:state_.shapes)if(other.kind==Shape::Kind::Blur){
  // A pixelation reads its blocks from the base, not from this window.
  if(other.pixelate)continue;
  const int reach=3*std::max(1,other.strength/2)+1;
  const auto overlap=clampRect(other.rect,state_.crop).intersected(area.adjusted(-reach,-reach,reach,reach));
  if(!overlap.isEmpty()&&other.rect.intersects(area))window|=overlap;
 }
 return window;
}

void EditDocument::paintPreviewSource(QPainter& painter,const QRectF& target,const QRectF& source,
 const std::function<void(QPainter&,const QRectF&)>& backdrop,const QRect& samplingBounds) const {
 if(target.isEmpty()||source.isEmpty())return;
 const QRect bounds=samplingBounds.isEmpty()?base_.rect():samplingBounds.intersected(base_.rect());if(bounds.isEmpty())return;
 const QImage view(base_.constScanLine(bounds.top())+qsizetype(bounds.left())*sizeof(QRgb),bounds.width(),bounds.height(),base_.bytesPerLine(),base_.format());
 const auto origin=bounds.topLeft();const auto localSource=source.translated(-origin);
 const double sx=target.width()/source.width(),sy=target.height()/source.height();
 const QTransform logical(sx,0,0,sy,target.x()-localSource.x()*sx,target.y()-localSource.y()*sy);
 // Share one source-to-target transform across the base and every patch.
 // Re-deriving a scale from each patch's rectangle changes fixed-point
 // sampling phase (especially at fractional zoom on a high-DPI backing store).
 painter.save();painter.setClipRect(target,Qt::IntersectClip);painter.setWorldTransform(logical,true);
 painter.drawImage(QPointF(0,0),view);painter.restore();
 auto masks=redactionRects();if(masks.isEmpty())return;
 for(auto& mask:masks)mask.translate(-origin);
 const auto device=logical*painter.deviceTransform();const auto fromDevice=device.inverted();
 const auto deviceToLogical=painter.deviceTransform().inverted();
 const auto painted=painter.hasClipping()?target.intersected(painter.clipBoundingRect()):target;
 const auto viewport=painter.deviceTransform().mapRect(painted).toAlignedRect();
 // Restrict work to edge bands. The interior is opaque black; only pixels
 // near a mask edge can mix a covered source sample into a neighbour.
 painter.save();painter.setClipRect(target,Qt::IntersectClip);
 for(const auto& mask:std::as_const(masks))painter.fillRect(logical.mapRect(QRectF(mask)),Qt::black);
 const int stepX=std::clamp(int(1020*std::abs(device.m11())),1,64);
 const int stepY=std::clamp(int(1020*std::abs(device.m22())),1,64);
 for(const auto& mask:masks){
  const QRect box=device.mapRect(QRectF(mask)).toAlignedRect();
  const bool smooth=painter.testRenderHint(QPainter::SmoothPixmapTransform);
  const int fringeX=smooth?int(std::ceil(std::abs(device.m11())/2))+1:1;
  const int fringeY=smooth?int(std::ceil(std::abs(device.m22())/2))+1:1;
  // At high DPI a logical downscale can still magnify source pixels on the
  // backing store. Include both sides of the full device-space filter reach.
  const auto outer=box.adjusted(-fringeX,-fringeY,fringeX,fringeY),inner=box.adjusted(fringeX,fringeY,-fringeX,-fringeY);
  QList<QRect> bands;
  if(inner.isEmpty())bands.append(outer);
  else bands={QRect(outer.x(),outer.y(),outer.width(),inner.y()-outer.y()),
              QRect(outer.x(),inner.y()+inner.height(),outer.width(),outer.y()+outer.height()-inner.y()-inner.height()),
              QRect(outer.x(),inner.y(),inner.x()-outer.x(),inner.height()),
              QRect(inner.x()+inner.width(),inner.y(),outer.x()+outer.width()-inner.x()-inner.width(),inner.height())};
  for(auto band:std::as_const(bands)){
   band=band.intersected(viewport);
   for(int y=band.y();y<band.y()+band.height();y+=stepY)for(int x=band.x();x<band.x()+band.width();x+=stepX){
    const int left=viewport.x()+((x-viewport.x())/4)*4;
    const int right=std::min(viewport.x()+viewport.width(),viewport.x()+((x+std::min(stepX,band.x()+band.width()-x)-viewport.x()+3)/4)*4);
    // Match the full span's four-pixel SIMD groups, avoiding a different
    // scalar filter precision at the ends of a narrow edge patch.
    const QRect pixels(left,y,right-left,std::min(stepY,band.y()+band.height()-y));
    // Qt's smooth painter samples the two adjacent source pixels per axis.
    // Two source pixels of halo keep the filter footprint off patch borders.
    const auto area=fromDevice.mapRect(QRectF(pixels)).toAlignedRect().adjusted(-2,-2,2,2).intersected(view.rect());
    if(area.isEmpty())continue;
    const QPoint position=area.topLeft()+origin;
    const QImage raw(base_.constScanLine(position.y())+qsizetype(position.x())*sizeof(QRgb),area.width(),area.height(),base_.bytesPerLine(),base_.format());
    auto patch=raw.copy();
    {QPainter cover(&patch);cover.setCompositionMode(QPainter::CompositionMode_Source);
     for(const auto& other:std::as_const(masks))cover.fillRect(other.intersected(area).translated(-area.topLeft()),Qt::black);}
    const auto clip=deviceToLogical.mapRect(QRectF(pixels));
    painter.save();painter.setClipRect(clip,Qt::IntersectClip);
    // Restore the backdrop before replacing a filtered alpha edge, rather
    // than blending it over the already-filtered, unsanitized base a second time.
    if(backdrop)backdrop(painter,clip);
    else{painter.setCompositionMode(QPainter::CompositionMode_Source);painter.fillRect(clip,Qt::transparent);}
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setWorldTransform(logical,true);painter.drawImage(QPointF(area.topLeft()),patch);painter.restore();
   }
  }
 }
 painter.restore();
}
QImage EditDocument::previewSource(const QRect& area,const QSize& displaySize) const {
 QImage image(displaySize,base_.format());if(image.isNull())return {};
 image.fill(Qt::transparent);QPainter painter(&image);painter.setRenderHint(QPainter::SmoothPixmapTransform);
 paintPreviewSource(painter,QRectF(image.rect()),QRectF(area),{},area);return image;
}

QImage EditDocument::highlightPreview(const Shape& shape,double scale) const {
 const auto area=clampRect(shape.rect,state_.crop);if(area.isEmpty())return {};
 // The plan reads full-resolution pixels (sampled); only the tint runs on the
 // bounded preview raster, like blur. The raster is a window around the box
 // that holds the reach of every blur crossing its edge, so the blur sees the
 // same neighbours it sees in the export. Other highlights are not laid: the
 // export tints each from the pre-highlight pixels too, and the later one wins.
 const auto plan=highlight::plan(base_,area,redactionRects(),shape.color);
 const auto window=highlightWindow(shape);
 constexpr double previewPixels=1.5e6;
 scale=std::min({1.0,scale,std::sqrt(previewPixels/(double(window.width())*window.height()))});
 const QSize target(std::max(1,int(std::lround(window.width()*scale))),std::max(1,int(std::lround(window.height()*scale))));
 auto preview=previewSource(window,target);
 const double sx=double(target.width())/window.width(),sy=double(target.height())/window.height();
 auto mapped=[&](const QRect& rect){return QRectF((rect.left()-window.left())*sx,(rect.top()-window.top())*sy,rect.width()*sx,rect.height()*sy).toAlignedRect();};
 {
  QPainter painter(&preview);painter.setCompositionMode(QPainter::CompositionMode_Source);
  for(const auto& other:state_.shapes)if(other.kind==Shape::Kind::Redact){const auto overlap=other.rect.intersected(window);if(!overlap.isEmpty())painter.fillRect(mapped(overlap),Qt::black);}
 }
 for(const auto& other:state_.shapes)if(other.kind==Shape::Kind::Blur){
  const auto overlap=clampRect(other.rect,state_.crop).intersected(window);if(overlap.isEmpty())continue;
  if(other.pixelate)pixelatePreview(preview,window,other);else blurInPlace(preview,mapped(overlap),std::max(1,int(std::lround(other.strength*std::min(sx,sy)))));
 }
 highlight::apply(preview,mapped(area),plan);
 const auto cut=mapped(area).intersected(preview.rect());
 return cut==preview.rect()?preview:preview.copy(cut);
}

QImage EditDocument::blurPreview(const Shape& shape,double scale) const {
 const auto area=clampRect(shape.rect,state_.crop);if(area.isEmpty())return {};
 // Bound preview work to about 1.5 megapixels regardless of the selected area.
 constexpr double previewPixels=1.5e6;
 scale=std::min({1.0,scale,std::sqrt(previewPixels/(double(area.width())*area.height()))});
 const QSize target(std::max(1,int(std::lround(area.width()*scale))),std::max(1,int(std::lround(area.height()*scale))));
 if(shape.pixelate){QImage preview(target,base_.format());if(preview.isNull())return {};pixelatePreview(preview,area,shape);return preview;}
 auto preview=previewSource(area,target);
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
