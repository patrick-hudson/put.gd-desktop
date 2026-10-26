#include "EditorCanvas.h"
#include "ui/Theme.h"
#include <QAccessible>
#include <QApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QStyleHints>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>

namespace putgd::editor {
namespace {
constexpr double minimumZoom=0.02,maximumZoom=32.0;
constexpr int handleTargetPixels=32;
template<size_t N>
int nearestHandle(const QPointF (&points)[N],const QPointF& position,int stride=1){
 int nearest=-1;double distance=0;
 for(size_t i=0;i<N;i+=stride){
  const auto delta=position-points[i];
  if(!QRectF(points[i]-QPointF(handleTargetPixels/2,handleTargetPixels/2),QSizeF(handleTargetPixels,handleTargetPixels)).contains(position))continue;
  const double squared=QPointF::dotProduct(delta,delta);
  if(nearest<0||squared<distance){nearest=int(i);distance=squared;}
 }
 return nearest;
}
// The scheme's colours at paint time (#60): the canvas behind the image and
// the accent of the keyboard focus ring around it.
QColor canvasBackground(){return Theme::current().bg;}
QColor accent(){return Theme::current().accent;}
// Guides drawn over the image read on pale and dark content alike, whatever
// the app theme: a dark under-stroke beneath an amber (or white) over-stroke,
// as the recording review outlines its masks. Outside a crop is darkened.
QColor dim(){return QColor(0,0,0,140);}
QColor guide(){return Theme::brandAccent();}
QColor underStroke(){return QColor(0,0,0,170);}
void dualRect(QPainter& painter,const QRectF& r,const QColor& over,qreal width,Qt::PenStyle style=Qt::SolidLine){
 painter.setBrush(Qt::NoBrush);
 painter.setPen(QPen(underStroke(),width+2,Qt::SolidLine,Qt::SquareCap,Qt::MiterJoin));painter.drawRect(r);
 painter.setPen(QPen(over,width,style,Qt::SquareCap,Qt::MiterJoin));painter.drawRect(r);
}
void dualLine(QPainter& painter,const QPointF& a,const QPointF& b,const QColor& over,Qt::PenStyle style){
 painter.setPen(QPen(underStroke(),1));painter.drawLine(a,b);
 painter.setPen(QPen(over,1,style));painter.drawLine(a,b);
}
QString kindLabel(Shape::Kind kind){
 switch(kind){case Shape::Kind::Redact:return "Redaction";case Shape::Kind::Blur:return "Blur";case Shape::Kind::Arrow:return "Arrow";
 case Shape::Kind::Rectangle:return "Rectangle";case Shape::Kind::Highlight:return "Highlight";case Shape::Kind::Text:return "Text label";}
 return "Mark";
}
double distanceToSegment(const QPointF& p,const QPointF& a,const QPointF& b){
 const QPointF ab=b-a;const double length=QPointF::dotProduct(ab,ab);
 if(length<=0)return QLineF(p,a).length();
 const double t=std::clamp(QPointF::dotProduct(p-a,ab)/length,0.0,1.0);
 return QLineF(p,a+t*ab).length();
}
Qt::CursorShape resizeCursor(int handle){return handle==0||handle==4?Qt::SizeFDiagCursor:handle==2||handle==6?Qt::SizeBDiagCursor:handle==1||handle==5?Qt::SizeVerCursor:Qt::SizeHorCursor;}
}

EditorCanvas::EditorCanvas(EditDocument& doc,QWidget* parent):QWidget(parent),doc_(doc){
 setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);setAttribute(Qt::WA_OpaquePaintEvent);
 setMinimumSize(320,240);setAccessibleName("Image being edited");
 // Screen readers hear the selection, or the crop, and the keys that act on it.
 connect(this,&EditorCanvas::selectionChanged,this,&EditorCanvas::announce);connect(this,&EditorCanvas::changed,this,&EditorCanvas::announce);
 announce();
}

void EditorCanvas::setTool(Tool tool){
 if(textEditor_)finishText(true);
 // A newly chosen drawing tool starts unselected, so its style controls
 // configure the tool rather than restyling an earlier shape.
 const bool deselect=tool!=tool_&&tool!=Tool::Select&&selected_;
 if(tool_==Tool::Crop&&tool!=Tool::Crop)cancelCrop();
 tool_=tool;
 if(tool_==Tool::Crop){cropDraft_=doc_.crop();selected_=0;emit selectionChanged();}
 else if(deselect){selected_=0;emit selectionChanged();}
 drag_=Drag::None;
 setCursor(tool_==Tool::Select?Qt::ArrowCursor:Qt::CrossCursor);
 if(fitted_)fit();else update();
 announce();
}

void EditorCanvas::setStyle(const ToolStyle& style){style_=style;update();}

void EditorCanvas::applyStyleToSelection(){
 const auto& style=style_;
 if(const auto* current=doc_.find(selected_)){
  auto shape=*current;
  using Kind=Shape::Kind;const auto k=shape.kind;
  if(k==Kind::Arrow||k==Kind::Rectangle||k==Kind::Text||k==Kind::Highlight)shape.color=style.color;
  if(k==Kind::Arrow||k==Kind::Rectangle)shape.width=style.width;
  if(k==Kind::Blur)shape.strength=style.strength;
  if(k==Kind::Text){shape.fontPixels=style.fontPixels;shape.fontFamily=style.fontFamily;}
  if(shape.kind==Shape::Kind::Text&&!shape.text.isEmpty()){const QFontMetrics metrics(textFont(shape.fontFamily,shape.fontPixels));shape.rect.setSize(QSize(metrics.horizontalAdvance(shape.text)+1,metrics.height()));}
  if(doc_.replace(shape))emit changed();
 }
 update();
}

QRect EditorCanvas::visibleBounds() const {return tool_==Tool::Crop?doc_.imageRect():doc_.crop();}
QPointF EditorCanvas::toImage(const QPointF& p) const {return (p-offset_)/scale_+QPointF(visibleBounds().topLeft());}
QPointF EditorCanvas::toWidget(const QPointF& p) const {return (p-QPointF(visibleBounds().topLeft()))*scale_+offset_;}
QRectF EditorCanvas::toWidget(const QRect& r) const {return QRectF(toWidget(QPointF(r.topLeft())),QSizeF(r.size())*scale_);}

void EditorCanvas::fit(){
 const auto bounds=visibleBounds();const QSizeF room=QSizeF(size())-QSizeF(32,32);
 scale_=std::clamp(std::min({1.0,room.width()/bounds.width(),room.height()/bounds.height()}),minimumZoom,maximumZoom);
 offset_=QPointF((width()-bounds.width()*scale_)/2,(height()-bounds.height()*scale_)/2);
 fitted_=true;update();emit viewChanged();
}
void EditorCanvas::actualSize(){zoomBy(1.0/scale_,QPointF(width()/2.0,height()/2.0));}
void EditorCanvas::zoomBy(double factor,QPointF anchor){
 if(anchor.x()<0)anchor=QPointF(width()/2.0,height()/2.0);
 const auto image=toImage(anchor);
 scale_=std::clamp(scale_*factor,minimumZoom,maximumZoom);
 offset_=anchor-(image-QPointF(visibleBounds().topLeft()))*scale_;
 fitted_=false;clampOffset();update();emit viewChanged();
}
void EditorCanvas::setZoom(double scale){zoomBy(scale/scale_);}
void EditorCanvas::panBy(const QPointF& delta){offset_+=delta;fitted_=false;clampOffset();update();emit viewChanged();}
QRect EditorCanvas::visibleImageRect() const {return boxFrom(toImage(QPointF(0,0)),toImage(QPointF(width(),height()))).intersected(visibleBounds());}
void EditorCanvas::clampOffset(){
 const auto bounds=visibleBounds();const double w=bounds.width()*scale_,h=bounds.height()*scale_;
 auto axis=[](double offset,double content,double room){return content<=room?(room-content)/2:std::clamp(offset,room-content-48,48.0);};
 offset_=QPointF(axis(offset_.x(),w,width()),axis(offset_.y(),h,height()));
}
void EditorCanvas::resizeEvent(QResizeEvent*){
 // Never move the image while a gesture maps pointer positions through it.
 if(drag_!=Drag::None){update();return;}
 if(fitted_)fit();else{clampOffset();update();}
}

Shape::Kind EditorCanvas::kindFor(Tool tool) const {
 switch(tool){case Tool::Redact:return Shape::Kind::Redact;case Tool::Blur:return Shape::Kind::Blur;case Tool::Arrow:return Shape::Kind::Arrow;
 case Tool::Highlight:return Shape::Kind::Highlight;case Tool::Text:return Shape::Kind::Text;default:return Shape::Kind::Rectangle;}
}
bool EditorCanvas::drawingTool() const {return tool_==Tool::Redact||tool_==Tool::Blur||tool_==Tool::Arrow||tool_==Tool::Rectangle||tool_==Tool::Highlight;}

// Outward rounding: a partly covered source pixel is inside the box.
QRect EditorCanvas::boxFrom(const QPointF& a,const QPointF& b) const {
 const int left=int(std::floor(std::min(a.x(),b.x()))),top=int(std::floor(std::min(a.y(),b.y())));
 const int right=int(std::ceil(std::max(a.x(),b.x()))),bottom=int(std::ceil(std::max(a.y(),b.y())));
 return QRect(QPoint(left,top),QPoint(std::max(left,right-1),std::max(top,bottom-1)));
}

int EditorCanvas::handleAt(const QRect& rect,const QPointF& p,bool cornersOnly) const {
 const auto r=toWidget(rect);
 const QPointF points[8]={r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())};
 return nearestHandle(points,p,cornersOnly?2:1);
}
int EditorCanvas::arrowHandleAt(const Shape& shape,const QPointF& p) const {
 const QPointF points[]={toWidget(QPointF(shape.from)),toWidget(QPointF(shape.to))};
 return nearestHandle(points,p);
}
// A drawing tool picks up what the user sees: an outlined rectangle by its
// outline (its interior stays drawable), an arrow by its line, and filled
// shapes and text by their area. Topmost shapes win.
quint64 EditorCanvas::grabAt(const QPointF& p) const {
 const auto& shapes=doc_.shapes();const double tolerance=6/scale_;
 for(auto it=shapes.crbegin();it!=shapes.crend();++it){
  if(it->kind==Shape::Kind::Arrow){if(distanceToSegment(p,it->from,it->to)<=std::max(tolerance,double(it->width)))return it->id;continue;}
  const QRectF box(it->rect);
  if(!box.adjusted(-tolerance,-tolerance,tolerance,tolerance).contains(p))continue;
  if(it->kind==Shape::Kind::Rectangle){const double edge=tolerance+it->width/2.0;if(box.adjusted(edge,edge,-edge,-edge).contains(p))continue;}
  return it->id;
 }
 return 0;
}
void EditorCanvas::updateHoverCursor(const QPointF& position,Qt::KeyboardModifiers modifiers){
 if(drag_!=Drag::None)return;
 if(space_){setCursor(Qt::OpenHandCursor);return;}
 if(tool_==Tool::Crop){const int h=handleAt(cropDraft_,position);setCursor(h>=0?resizeCursor(h):QRectF(cropDraft_).contains(toImage(position))?Qt::SizeAllCursor:Qt::CrossCursor);return;}
 const auto fallback=tool_==Tool::Select?Qt::ArrowCursor:Qt::CrossCursor;
 // Ctrl draws a new shape even over existing ones.
 if(drawingTool()&&(modifiers&Qt::ControlModifier)){setCursor(fallback);return;}
 const auto image=toImage(position);
 if(const auto* shape=doc_.find(selected_);shape&&(tool_!=Tool::Text||shape->kind==Shape::Kind::Text)){
  const int h=selectionHandleAt(*shape,position);
  if(h>=0){setCursor(shape->kind==Shape::Kind::Arrow?Qt::SizeAllCursor:resizeCursor(h));return;}
 }
 quint64 hit=0;
 if(tool_==Tool::Select)hit=shapeAt(image);
 else if(tool_==Tool::Text){const auto* shape=doc_.find(shapeAt(image));hit=shape&&shape->kind==Shape::Kind::Text?shape->id:0;}
 else if(drawingTool())hit=grabAt(image);
 setCursor(hit?Qt::SizeAllCursor:fallback);
}
quint64 EditorCanvas::shapeAt(const QPointF& p) const {
 const auto& shapes=doc_.shapes();const double tolerance=6/scale_;
 for(auto it=shapes.crbegin();it!=shapes.crend();++it){
  if(it->kind==Shape::Kind::Arrow){if(distanceToSegment(p,it->from,it->to)<=std::max(tolerance,double(it->width)))return it->id;continue;}
  if(QRectF(it->rect).adjusted(-tolerance,-tolerance,tolerance,tolerance).contains(p))return it->id;
 }
 return 0;
}
int EditorCanvas::selectionHandleAt(const Shape& shape,const QPointF& p) const {
 if(shape.kind==Shape::Kind::Arrow)return arrowHandleAt(shape,p);
 return handleAt(shape.rect,p,shape.kind==Shape::Kind::Text);
}
// Dragging a text corner scales the font around the opposite corner, which
// stays fixed. The scale is the drag projected onto the box's diagonal, so
// pulling outward along any direction grows the text smoothly.
Shape EditorCanvas::scaledText(const Shape& original,int handle,const QPointF& now) const {
 const QRect box=original.rect;const QRectF exact(box);
 const QPointF corners[4]={exact.topLeft(),exact.topRight(),exact.bottomRight(),exact.bottomLeft()};
 const QPointF dragged=corners[handle/2],anchor=corners[(handle/2+2)%4],diagonal=dragged-anchor;
 const double length=QPointF::dotProduct(diagonal,diagonal);
 const double scale=length>0?std::max(0.0,QPointF::dotProduct(now-anchor,diagonal)/length):1.0;
 auto shape=original;
 shape.fontPixels=std::clamp(int(std::lround(original.fontPixels*scale)),minimumFontPixels,maximumFontPixels);
 const QFontMetrics metrics(textFont(shape.fontFamily,shape.fontPixels));
 QRect resized(QPoint(),QSize(metrics.horizontalAdvance(shape.text)+1,metrics.height()));
 if(handle==0)resized.moveBottomRight(box.bottomRight());else if(handle==2)resized.moveBottomLeft(box.bottomLeft());
 else if(handle==4)resized.moveTopLeft(box.topLeft());else resized.moveTopRight(box.topRight());
 shape.rect=resized;return shape;
}
QRect EditorCanvas::resized(QRect r,int handle,const QPoint& d) const {
 if(handle==0||handle==6||handle==7)r.setLeft(r.left()+d.x());
 if(handle==2||handle==3||handle==4)r.setRight(r.right()+d.x());
 if(handle==0||handle==1||handle==2)r.setTop(r.top()+d.y());
 if(handle==4||handle==5||handle==6)r.setBottom(r.bottom()+d.y());
 return r.normalized();
}

const QImage& EditorCanvas::blurTile(const Shape& shape){
 const double bucket=std::pow(2.0,std::round(std::log2(std::max(scale_,1e-3))*2)/2);
 QString key=QString("%1,%2,%3,%4/%5/%6/%7,%8,%9,%10").arg(shape.rect.x()).arg(shape.rect.y()).arg(shape.rect.width()).arg(shape.rect.height()).arg(shape.strength).arg(bucket)
  .arg(doc_.crop().x()).arg(doc_.crop().y()).arg(doc_.crop().width()).arg(doc_.crop().height());
 for(const auto& other:doc_.shapes())if(other.kind==Shape::Kind::Redact&&other.rect.intersects(shape.rect))key+=QString(";%1,%2,%3,%4").arg(other.rect.x()).arg(other.rect.y()).arg(other.rect.width()).arg(other.rect.height());
 auto& tile=tiles_[shape.id];tile.used=++tick_;
 if(tile.key==key)return tile.image;
 cacheBytes_-=tile.image.sizeInBytes();tile.image=doc_.blurPreview(shape,bucket);tile.key=key;cacheBytes_+=tile.image.sizeInBytes();
 // Least-recently painted tiles are recomputed later rather than retained.
 while(cacheBytes_>blurCacheBytes&&tiles_.size()>1){
  auto oldest=tiles_.end();for(auto it=tiles_.begin();it!=tiles_.end();++it)if(it.key()!=shape.id&&(oldest==tiles_.end()||it->used<oldest->used))oldest=it;
  if(oldest==tiles_.end())break;
  cacheBytes_-=oldest->image.sizeInBytes();tiles_.erase(oldest);
 }
 return tiles_[shape.id].image;
}

void EditorCanvas::documentChanged(){
 if(!doc_.find(selected_)){selected_=0;emit selectionChanged();}
 QSet<quint64> live;for(const auto& shape:doc_.shapes())live.insert(shape.id);
 for(auto it=tiles_.begin();it!=tiles_.end();){if(!live.contains(it.key())){cacheBytes_-=it->image.sizeInBytes();it=tiles_.erase(it);}else ++it;}
 if(fitted_)fit();else{clampOffset();update();}
}

void EditorCanvas::paintEvent(QPaintEvent*){
 QPainter painter(this);painter.fillRect(rect(),canvasBackground());
 const auto bounds=visibleBounds();const auto target=toWidget(bounds);
 // Paint only the visible source rectangle; never allocate a scaled full frame.
 const QRectF visible=target.intersected(QRectF(rect()));
 if(!visible.isEmpty()){
  const QRectF source(toImage(visible.topLeft()),toImage(visible.bottomRight()));
  painter.setRenderHint(QPainter::SmoothPixmapTransform,scale_<1.0);
  painter.drawImage(visible,doc_.base(),source);
 }
 painter.save();painter.setClipRect(target);
 painter.translate(offset_);painter.scale(scale_,scale_);painter.translate(-QPointF(bounds.topLeft()));
 for(const auto& shape:doc_.shapes()){
  if(shape.kind==Shape::Kind::Redact)painter.fillRect(shape.rect,Qt::black);
  else if(shape.kind==Shape::Kind::Blur){const auto& tile=blurTile(shape);const auto area=shape.rect.intersected(doc_.crop());if(!tile.isNull())painter.drawImage(QRectF(area),tile);}
  else if(!(textEditor_&&shape.id==textShape_))EditDocument::paintAnnotation(painter,shape);
 }
 painter.restore();
 if(tool_==Tool::Crop){
  const auto crop=toWidget(cropDraft_);QPainterPath outside;outside.addRect(target);outside.addRect(crop);
  painter.fillPath(outside,dim());
  // Thirds: white dashes over a dark line; the frame: amber over dark.
  const QColor thirds(255,255,255,200);
  for(int i=1;i<3;++i){dualLine(painter,QPointF(crop.left()+crop.width()*i/3,crop.top()),QPointF(crop.left()+crop.width()*i/3,crop.bottom()),thirds,Qt::DashLine);dualLine(painter,QPointF(crop.left(),crop.top()+crop.height()*i/3),QPointF(crop.right(),crop.top()+crop.height()*i/3),thirds,Qt::DashLine);}
  dualRect(painter,crop,guide(),2);
 }
 // Handles: amber squares in a dark rim, legible on any image.
 auto handles=[&](const QList<QPointF>& points){painter.setPen(QPen(underStroke(),2));painter.setBrush(guide());for(const auto& point:points)painter.drawRect(QRectF(point-QPointF(4,4),QSizeF(8,8)));};
 if(tool_==Tool::Crop){const auto r=toWidget(cropDraft_);handles({r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())});}
 else if(const auto* shape=doc_.find(selected_)){
  if(shape->kind==Shape::Kind::Arrow)handles({toWidget(QPointF(shape->from)),toWidget(QPointF(shape->to))});
  else{
   const auto r=toWidget(shape->rect);dualRect(painter,r.adjusted(-2,-2,2,2),guide(),1,Qt::DashLine);
   if(shape->kind==Shape::Kind::Text)handles({r.topLeft(),r.topRight(),r.bottomRight(),r.bottomLeft()});
   else handles({r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())});
  }
 }
 // The pane ring marks keyboard focus only, like every other control.
 if(hasFocus()&&window()->testAttribute(Qt::WA_KeyboardFocusChange)){painter.setPen(QPen(accent(),2));painter.setBrush(Qt::NoBrush);painter.drawRect(QRectF(rect()).adjusted(1,1,-1,-1));}
}

void EditorCanvas::mousePressEvent(QMouseEvent* event){
 setFocus();
 press_=last_=event->position();const auto image=toImage(press_);
 if(event->button()==Qt::MiddleButton||(event->button()==Qt::LeftButton&&space_)){drag_=Drag::Pan;setCursor(Qt::ClosedHandCursor);return;}
 if(event->button()!=Qt::LeftButton)return;
 if(textEditor_){finishText(true);return;}
 if(tool_==Tool::Crop){
  cropOriginal_=cropDraft_;handle_=handleAt(cropDraft_,press_);
  // An uncropped image has nothing to move: dragging anywhere draws a new crop.
  drag_=handle_>=0?Drag::CropResize:(QRectF(cropDraft_).contains(image)&&cropDraft_!=doc_.imageRect())?Drag::CropMove:Drag::CropNew;
  return;
 }
 moved_=false;editOnClick_=0;
 if(tool_==Tool::Text){
  if(const auto* current=doc_.find(selected_);current&&current->kind==Shape::Kind::Text){
   const int handle=selectionHandleAt(*current,press_);
   if(handle>=0){original_=*current;handle_=handle;drag_=Drag::Resize;doc_.beginGesture();return;}
  }
  const auto* shape=doc_.find(shapeAt(image));
  // Dragging a text label moves it; a click edits it.
  if(shape&&shape->kind==Shape::Kind::Text){
   if(selected_!=shape->id){selected_=shape->id;emit selectionChanged();}
   original_=*shape;editOnClick_=shape->id;drag_=Drag::Move;doc_.beginGesture();update();return;
  }
  beginText(image,nullptr);return;
 }
 const bool force=drawingTool()&&(event->modifiers()&Qt::ControlModifier);
 // Handles and the visible body of the current selection edit it in every tool.
 if(!force)if(const auto* shape=doc_.find(selected_)){
  const int handle=selectionHandleAt(*shape,press_);
  if(handle>=0){original_=*shape;handle_=handle;drag_=Drag::Resize;doc_.beginGesture();return;}
  if((tool_==Tool::Select?shapeAt(image):grabAt(image))==selected_){original_=*shape;drag_=Drag::Move;doc_.beginGesture();return;}
 }
 if(tool_==Tool::Select){
  selected_=shapeAt(image);emit selectionChanged();
  if(const auto* shape=doc_.find(selected_)){original_=*shape;drag_=Drag::Move;doc_.beginGesture();}
  update();return;
 }
 if(!drawingTool())return;
 // A drawing tool picks up a visible shape to move it; Ctrl draws over it.
 if(!force)if(const auto hit=grabAt(image)){
  selected_=hit;emit selectionChanged();original_=*doc_.find(hit);drag_=Drag::Move;doc_.beginGesture();update();return;
 }
 Shape shape;shape.kind=kindFor(tool_);shape.color=shape.kind==Shape::Kind::Redact?QColor(Qt::black):style_.color;
 shape.width=style_.width;shape.strength=style_.strength;shape.fontPixels=style_.fontPixels;shape.fontFamily=style_.fontFamily;
 const QPoint at(int(std::floor(image.x())),int(std::floor(image.y())));
 shape.from=shape.to=at;shape.rect=boxFrom(image,image);
 doc_.beginGesture();selected_=doc_.add(shape);original_=*doc_.find(selected_);drag_=Drag::Create;emit selectionChanged();update();
}

void EditorCanvas::mouseMoveEvent(QMouseEvent* event){
 const auto position=event->position();
 if(drag_==Drag::None){updateHoverCursor(position,event->modifiers());return;}
 // A press that has not travelled the platform drag distance is a click:
 // it selects (or, for text, edits) without nudging the shape.
 if(drag_==Drag::Move&&!moved_){if(QLineF(press_,position).length()<QGuiApplication::styleHints()->startDragDistance())return;moved_=true;}
 if(drag_==Drag::Pan){offset_+=position-last_;last_=position;fitted_=false;clampOffset();update();emit viewChanged();return;}
 const auto start=toImage(press_),now=toImage(position);
 const QPoint delta(int(std::lround(now.x()-start.x())),int(std::lround(now.y()-start.y())));
 last_=position;
 const auto image=doc_.imageRect();
 if(drag_==Drag::CropNew){cropDraft_=EditDocument::clampRect(boxFrom(start,now),image);if(cropDraft_.isEmpty())cropDraft_=cropOriginal_;update();return;}
 if(drag_==Drag::CropMove){auto moved=cropOriginal_.translated(delta);moved.moveLeft(std::clamp(moved.left(),0,image.width()-moved.width()));moved.moveTop(std::clamp(moved.top(),0,image.height()-moved.height()));cropDraft_=moved;update();return;}
 if(drag_==Drag::CropResize){const auto r=EditDocument::clampRect(resized(cropOriginal_,handle_,delta),image);if(!r.isEmpty())cropDraft_=r;update();return;}
 auto shape=original_;
 if(drag_==Drag::Create){
  if(shape.kind==Shape::Kind::Arrow)shape.to=QPoint(int(std::lround(now.x())),int(std::lround(now.y())));
  else{shape.rect=boxFrom(start,now);if(event->modifiers()&Qt::ShiftModifier){const int side=std::max(shape.rect.width(),shape.rect.height());shape.rect=QRect(shape.rect.topLeft(),QSize(side,side));}}
 }else if(drag_==Drag::Move){shape.rect.translate(delta);shape.from+=delta;shape.to+=delta;}
 else if(drag_==Drag::Resize){
  if(shape.kind==Shape::Kind::Arrow){if(handle_==0)shape.from+=delta;else shape.to+=delta;}
  else if(shape.kind==Shape::Kind::Text)shape=scaledText(original_,handle_,now);
  else shape.rect=resized(shape.rect,handle_,delta);
 }
 doc_.preview(shape);update();
 // The size control follows a text label while its corner is dragged.
 if(drag_==Drag::Resize&&shape.kind==Shape::Kind::Text)emit selectionChanged();
}

void EditorCanvas::mouseReleaseEvent(QMouseEvent* event){
 if(drag_==Drag::Pan){drag_=Drag::None;updateHoverCursor(event->position(),event->modifiers());return;}
 if(event->button()!=Qt::LeftButton)return;
 const auto kind=drag_;drag_=Drag::None;const auto edit=std::exchange(editOnClick_,0);
 if(kind==Drag::CropNew||kind==Drag::CropMove||kind==Drag::CropResize||kind==Drag::None)return;
 if(kind==Drag::Create){
  const auto* shape=doc_.find(selected_);
  const bool tiny=!shape||(shape->kind==Shape::Kind::Arrow?QLineF(toWidget(QPointF(shape->from)),toWidget(QPointF(shape->to))).length()<6:(shape->rect.width()*scale_<4||shape->rect.height()*scale_<4));
  if(tiny){doc_.cancelGesture();selected_=0;emit selectionChanged();update();return;}
 }
 doc_.commitGesture();
 if(kind==Drag::Move&&edit&&!moved_)if(const auto* shape=doc_.find(edit)){beginText(QPointF(shape->rect.topLeft()),shape);return;}
 update();emit changed();updateHoverCursor(event->position(),event->modifiers());
}

void EditorCanvas::mouseDoubleClickEvent(QMouseEvent* event){
 const auto* shape=doc_.find(shapeAt(toImage(event->position())));
 if(shape&&shape->kind==Shape::Kind::Text)beginText(QPointF(shape->rect.topLeft()),shape);
}

void EditorCanvas::wheelEvent(QWheelEvent* event){
 const auto angle=event->angleDelta();
 if(event->modifiers()&Qt::ControlModifier){if(angle.y())zoomBy(std::pow(1.0015,angle.y()),event->position());}
 else{offset_+=(event->modifiers()&Qt::ShiftModifier)?QPointF(angle.y()/2.0,0):QPointF(angle.x()/2.0,angle.y()/2.0);fitted_=false;clampOffset();update();emit viewChanged();}
 event->accept();
}

bool EditorCanvas::removeSelected(){
 if(!selected_||cropPending()||editingText()||!doc_.remove(selected_))return false;
 selected_=0;emit selectionChanged();update();emit changed();return true;
}

void EditorCanvas::keyPressEvent(QKeyEvent* event){
 if(event->key()==Qt::Key_Space&&!event->isAutoRepeat()){space_=true;setCursor(Qt::OpenHandCursor);return;}
 if(event->key()==Qt::Key_Control){updateHoverCursor(mapFromGlobal(QCursor::pos()),Qt::ControlModifier);return;}
 if(tool_==Tool::Crop){
  // Plain Enter applies only the pending crop; upload requires Ctrl+Enter.
  if((event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)&&!(event->modifiers()&Qt::ControlModifier)){applyCrop();return;}
  if(event->key()==Qt::Key_Escape){cancelCrop();return;}
 }
 // Arrows move the selection or crop a pixel (Ctrl: ten); Shift+arrows
 // resize it from its right and bottom edges (an arrow by its head, text by
 // its size).
 const int step=(event->modifiers()&Qt::ControlModifier)?10:1;QPoint delta;
 switch(event->key()){case Qt::Key_Left:delta={-step,0};break;case Qt::Key_Right:delta={step,0};break;case Qt::Key_Up:delta={0,-step};break;case Qt::Key_Down:delta={0,step};break;default:break;}
 if(!delta.isNull()&&(event->modifiers()&Qt::AltModifier)){
  const int pixels=(event->modifiers()&Qt::ShiftModifier)?128:32;
  panBy(QPointF(delta.x()/step*pixels,delta.y()/step*pixels));return;
 }
 if(!delta.isNull()&&nudge(delta,event->modifiers()&Qt::ShiftModifier))return;
 // Without a pointer: Insert places the tool's mark, brackets cycle the marks
 // and Enter edits a selected text label.
 if(event->key()==Qt::Key_Insert){placeMark();return;}
 if(event->key()==Qt::Key_BracketRight||event->key()==Qt::Key_BracketLeft){selectAdjacent(event->key()==Qt::Key_BracketRight?1:-1,true);return;}
 if((event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)&&!(event->modifiers()&Qt::ControlModifier)){
  if(const auto* shape=doc_.find(selected_);shape&&shape->kind==Shape::Kind::Text){beginText(QPointF(shape->rect.topLeft()),shape);return;}
 }
 if((event->key()==Qt::Key_Delete||event->key()==Qt::Key_Backspace)&&selected_){removeSelected();return;}
 if(event->key()==Qt::Key_Escape&&selected_){selected_=0;emit selectionChanged();update();return;}
 QWidget::keyPressEvent(event);
}
void EditorCanvas::keyReleaseEvent(QKeyEvent* event){
 if(event->key()==Qt::Key_Space&&!event->isAutoRepeat()){space_=false;updateHoverCursor(mapFromGlobal(QCursor::pos()),event->modifiers());return;}
 if(event->key()==Qt::Key_Control){updateHoverCursor(mapFromGlobal(QCursor::pos()),Qt::NoModifier);return;}
 QWidget::keyReleaseEvent(event);
}

bool EditorCanvas::nudge(const QPoint& d,bool resize){
 if(tool_==Tool::Crop){
  auto r=cropDraft_;
  if(resize){r.setRight(r.right()+d.x());r.setBottom(r.bottom()+d.y());r=EditDocument::clampRect(r,doc_.imageRect());if(r.width()<1||r.height()<1)return true;}
  else{r.translate(d);if(!doc_.imageRect().contains(r))return true;}
  cropDraft_=r;update();announce();emit viewChanged();return true;
 }
 const auto* current=doc_.find(selected_);if(!current)return false;
 auto shape=*current;using Kind=Shape::Kind;
 if(!resize){shape.rect.translate(d);shape.from+=d;shape.to+=d;}
 else if(shape.kind==Kind::Arrow){shape.to+=d;if(shape.to==shape.from)return true;}
 else if(shape.kind==Kind::Text){
  shape.fontPixels=std::clamp(shape.fontPixels+d.x()+d.y(),minimumFontPixels,maximumFontPixels);
  const QFontMetrics metrics(textFont(shape.fontFamily,shape.fontPixels));shape.rect.setSize(QSize(metrics.horizontalAdvance(shape.text)+1,metrics.height()));
 }else{shape.rect.setRight(shape.rect.right()+d.x());shape.rect.setBottom(shape.rect.bottom()+d.y());if(shape.rect.width()<1||shape.rect.height()<1)return true;}
 const auto bounds=shape.kind==Kind::Arrow?QRect(shape.from,shape.to).normalized():shape.rect;
 if(!bounds.intersects(doc_.imageRect()))return true;
 if(doc_.replace(shape)){update();emit changed();if(shape.kind==Kind::Text)emit selectionChanged();}
 return true;
}

void EditorCanvas::select(quint64 id){
 if(textEditor_)finishText(true);
 if(id&&!doc_.find(id))return;
 if(id&&tool_==Tool::Crop)cancelCrop();
 if(selected_==id)return;
 selected_=id;emit selectionChanged();update();
}
bool EditorCanvas::selectAdjacent(int step,bool wrap){
 const auto& shapes=doc_.shapes();
 if(shapes.isEmpty()||tool_==Tool::Crop||step==0)return false;
 qsizetype index=-1;for(qsizetype i=0;i<shapes.size();++i)if(shapes[i].id==selected_)index=i;
 qsizetype next=index<0?(step>0?0:shapes.size()-1):index+step;
 if(next<0||next>=shapes.size()){if(!wrap)return false;next=((next%shapes.size())+shapes.size())%shapes.size();}
 select(shapes[next].id);return true;
}
quint64 EditorCanvas::placeMark(){
 if(textEditor_)finishText(true);
 const auto area=doc_.crop();
 if(tool_==Tool::Text){beginText(QPointF(area.left()+area.width()/4.0,area.center().y()-style_.fontPixels/2.0),nullptr);return 0;}
 if(!drawingTool())return 0;
 Shape shape;shape.kind=kindFor(tool_);shape.color=shape.kind==Shape::Kind::Redact?QColor(Qt::black):style_.color;
 shape.width=style_.width;shape.strength=style_.strength;shape.fontPixels=style_.fontPixels;shape.fontFamily=style_.fontFamily;
 // A quarter of the visible image, centred; another placement steps down and
 // right rather than hiding exactly under the last one.
 QRect box(QPoint(),QSize(std::max(1,area.width()/2),std::max(1,area.height()/2)));box.moveCenter(area.center());
 const int offset=std::max(8,std::min(area.width(),area.height())/20);
 auto occupied=[&](const QRect& r){for(const auto& other:doc_.shapes())if(other.kind==shape.kind&&(other.kind==Shape::Kind::Arrow?other.from==r.bottomLeft():other.rect==r))return true;return false;};
 for(int i=0;i<16&&occupied(box);++i){const auto moved=box.translated(offset,offset);if(!area.contains(moved))break;box=moved;}
 shape.rect=box;shape.from=box.bottomLeft();shape.to=box.topRight();
 selected_=doc_.add(shape);emit selectionChanged();update();emit changed();
 return selected_;
}

std::optional<QRect> EditorCanvas::selectionGeometry() const {
 if(tool_==Tool::Crop)return cropDraft_;
 const auto* shape=doc_.find(selected_);if(!shape)return std::nullopt;
 if(shape->kind==Shape::Kind::Arrow)return QRect(shape->from.x(),shape->from.y(),shape->to.x()-shape->from.x(),shape->to.y()-shape->from.y());
 return shape->rect;
}
bool EditorCanvas::setSelectionGeometry(const QRect& r){
 const auto image=doc_.imageRect();
 if(tool_==Tool::Crop){
  const auto clamped=EditDocument::clampRect(r,image);if(r.width()<1||r.height()<1||clamped.isEmpty())return false;
  cropDraft_=clamped;update();announce();emit viewChanged();return true;
 }
 const auto* current=doc_.find(selected_);if(!current)return false;
 auto shape=*current;using Kind=Shape::Kind;
 if(shape.kind==Kind::Arrow){shape.from=r.topLeft();shape.to=QPoint(r.x()+r.width(),r.y()+r.height());if(shape.from==shape.to)return false;}
 else if(shape.kind==Kind::Text)shape.rect.moveTopLeft(r.topLeft());
 else{if(r.width()<1||r.height()<1)return false;shape.rect=r;if(shape.kind==Kind::Redact||shape.kind==Kind::Blur)shape.rect=EditDocument::clampRect(r,image);}
 const auto bounds=shape.kind==Kind::Arrow?QRect(shape.from,shape.to).normalized().adjusted(0,0,1,1):shape.rect;
 if(!bounds.intersects(image)||!doc_.replace(shape))return false;
 update();emit changed();return true;
}

QString EditorCanvas::selectionSummary() const {
 const QString panHelp=" Alt+arrow keys pan the image; Alt+Shift+arrow keys pan farther.";
 auto box=[](const QRect& r){return QString("x %1, y %2, %3 × %4 px").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());};
 if(tool_==Tool::Crop)return QString("Crop area %1. Arrow keys move it, Shift+arrow keys resize it, Enter applies, Escape cancels.").arg(box(cropDraft_))+panHelp;
 const auto& shapes=doc_.shapes();
 for(qsizetype i=0;i<shapes.size();++i){
  const auto& shape=shapes[i];if(shape.id!=selected_)continue;
  const auto where=shape.kind==Shape::Kind::Arrow?QString("from %1, %2 to %3, %4").arg(shape.from.x()).arg(shape.from.y()).arg(shape.to.x()).arg(shape.to.y())
   :shape.kind==Shape::Kind::Text?QString("\"%1\" at x %2, y %3, %4 px").arg(shape.text).arg(shape.rect.x()).arg(shape.rect.y()).arg(shape.fontPixels):box(shape.rect);
  return QString("%1 %2 of %3 selected, %4. Arrow keys move it, Shift+arrow keys resize it, Delete removes it, [ and ] cycle marks, Tab leaves the canvas for properties, Escape deselects.").arg(kindLabel(shape.kind)).arg(i+1).arg(shapes.size()).arg(where)+panHelp;
 }
 QString text=shapes.isEmpty()?QString("No marks."):shapes.size()==1?QString("1 mark, none selected."):QString("%1 marks, none selected.").arg(shapes.size());
 if(drawingTool()||tool_==Tool::Text)text+=QString(" Insert places a %1 in the middle.").arg(kindLabel(kindFor(tool_)).toLower());
 if(!shapes.isEmpty())text+=" [ and ] cycle marks; Escape deselects. Tab leaves the canvas.";
 return text+panHelp;
}
void EditorCanvas::announce(){
 const auto text=selectionSummary();if(accessibleDescription()==text)return;
 setAccessibleDescription(text);
 QAccessibleEvent event(this,QAccessible::DescriptionChanged);QAccessible::updateAccessibility(&event);
}

bool EditorCanvas::eventFilter(QObject* watched,QEvent* event){
 if(watched==textEditor_&&event->type()==QEvent::KeyPress){
  // Enter ends the label here: the field would pass it on to the canvas,
  // where Enter on a selected label opens it again.
  const auto key=static_cast<QKeyEvent*>(event)->key();
  if(key==Qt::Key_Escape){finishText(false);return true;}
  if((key==Qt::Key_Return||key==Qt::Key_Enter)&&!(static_cast<QKeyEvent*>(event)->modifiers()&Qt::ControlModifier)){finishText(true);return true;}
 }
 return QWidget::eventFilter(watched,event);
}

bool EditorCanvas::applyCrop(){
 if(tool_!=Tool::Crop)return false;
 const bool changedCrop=doc_.setCrop(cropDraft_);
 tool_=Tool::Select;setCursor(Qt::ArrowCursor);emit toolRequested(Tool::Select);
 documentChanged();if(changedCrop)emit changed();fit();return changedCrop;
}
void EditorCanvas::cancelCrop(){
 if(tool_!=Tool::Crop)return;
 cropDraft_=doc_.crop();tool_=Tool::Select;setCursor(Qt::ArrowCursor);emit toolRequested(Tool::Select);fit();
}

void EditorCanvas::beginText(const QPointF& image,const Shape* existing){
 textAnchor_=QPointF(std::floor(image.x()),std::floor(image.y()));textShape_=existing?existing->id:0;
 const int pixels=existing?existing->fontPixels:style_.fontPixels;const auto family=existing?existing->fontFamily:style_.fontFamily;
 auto* editor=new QLineEdit(this);editor->setObjectName("annotationText");editor->setMaxLength(EditDocument::maximumTextCharacters);
 editor->setPlaceholderText("Type, then press Enter");editor->setAccessibleName("Annotation text");
 QFont font=textFont(family,std::clamp(int(std::lround(pixels*scale_)),12,96));editor->setFont(font);
 if(existing)editor->setText(existing->text);
 const auto at=toWidget(textAnchor_);editor->move(int(at.x())-4,int(at.y())-4);
 editor->resize(std::max(220,QFontMetrics(font).horizontalAdvance(editor->text())+48),QFontMetrics(font).height()+12);
 connect(editor,&QLineEdit::returnPressed,this,[this]{finishText(true);});
 connect(editor,&QLineEdit::textChanged,editor,[editor](const QString& text){editor->resize(std::max(220,QFontMetrics(editor->font()).horizontalAdvance(text)+48),editor->height());});
 editor->installEventFilter(this);textEditor_=editor;editor->show();editor->setFocus();
 selected_=textShape_;emit selectionChanged();update();
}

void EditorCanvas::finishText(bool commit){
 if(!textEditor_)return;
 const auto text=textEditor_->text().trimmed();auto* editor=textEditor_.data();textEditor_=nullptr;editor->hide();editor->deleteLater();
 const auto* existing=doc_.find(textShape_);
 if(commit){
  Shape shape=existing?*existing:Shape{};
  if(!existing){shape.kind=Shape::Kind::Text;shape.color=style_.color;shape.fontPixels=style_.fontPixels;shape.fontFamily=style_.fontFamily;shape.width=style_.width;}
  shape.text=text;const QFontMetrics metrics(textFont(shape.fontFamily,shape.fontPixels));
  shape.rect=QRect(QPoint(int(textAnchor_.x()),int(textAnchor_.y())),QSize(metrics.horizontalAdvance(text)+1,metrics.height()));
  if(text.isEmpty()){if(existing&&doc_.remove(existing->id))emit changed();selected_=0;}
  else if(existing){if(doc_.replace(shape))emit changed();}
  else{selected_=doc_.add(shape);emit changed();}
 }
 textShape_=0;emit selectionChanged();setFocus();update();
}
}
