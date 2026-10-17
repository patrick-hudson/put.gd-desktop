#include "EditorCanvas.h"
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
constexpr int handlePixels=9;
const QColor canvasBackground("#111317"),accent("#6EA8FE"),dim(17,19,23,170);
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

int EditorCanvas::handleAt(const QRect& rect,const QPointF& p) const {
 const auto r=toWidget(rect);
 const QPointF points[8]={r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())};
 for(int i=0;i<8;++i)if(QLineF(points[i],p).length()<=handlePixels)return i;
 return -1;
}
int EditorCanvas::arrowHandleAt(const Shape& shape,const QPointF& p) const {
 if(QLineF(toWidget(QPointF(shape.from)),p).length()<=handlePixels)return 0;
 if(QLineF(toWidget(QPointF(shape.to)),p).length()<=handlePixels)return 1;
 return -1;
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
 if(tool_!=Tool::Text)if(const auto* shape=doc_.find(selected_)){
  const int h=shape->kind==Shape::Kind::Arrow?arrowHandleAt(*shape,position):shape->kind==Shape::Kind::Text?-1:handleAt(shape->rect,position);
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
 QPainter painter(this);painter.fillRect(rect(),canvasBackground);
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
  painter.fillPath(outside,dim);
  painter.setPen(QPen(accent,2));painter.setBrush(Qt::NoBrush);painter.drawRect(crop);
  painter.setPen(QPen(QColor(255,255,255,90),1,Qt::DashLine));
  for(int i=1;i<3;++i){painter.drawLine(QPointF(crop.left()+crop.width()*i/3,crop.top()),QPointF(crop.left()+crop.width()*i/3,crop.bottom()));painter.drawLine(QPointF(crop.left(),crop.top()+crop.height()*i/3),QPointF(crop.right(),crop.top()+crop.height()*i/3));}
 }
 auto handles=[&](const QList<QPointF>& points){painter.setPen(QPen(accent,1.5));painter.setBrush(canvasBackground);for(const auto& point:points)painter.drawRect(QRectF(point-QPointF(4,4),QSizeF(8,8)));};
 if(tool_==Tool::Crop){const auto r=toWidget(cropDraft_);handles({r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())});}
 else if(const auto* shape=doc_.find(selected_)){
  if(shape->kind==Shape::Kind::Arrow)handles({toWidget(QPointF(shape->from)),toWidget(QPointF(shape->to))});
  else{
   const auto r=toWidget(shape->rect);painter.setPen(QPen(accent,1,Qt::DashLine));painter.setBrush(Qt::NoBrush);painter.drawRect(r.adjusted(-2,-2,2,2));
   if(shape->kind!=Shape::Kind::Text)handles({r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())});
  }
 }
 if(hasFocus()){painter.setPen(QPen(accent,2));painter.setBrush(Qt::NoBrush);painter.drawRect(QRectF(rect()).adjusted(1,1,-1,-1));}
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
  const int handle=shape->kind==Shape::Kind::Arrow?arrowHandleAt(*shape,press_):shape->kind==Shape::Kind::Text?-1:handleAt(shape->rect,press_);
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
  else shape.rect=resized(shape.rect,handle_,delta);
 }
 doc_.preview(shape);update();
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

void EditorCanvas::keyPressEvent(QKeyEvent* event){
 if(event->key()==Qt::Key_Space&&!event->isAutoRepeat()){space_=true;setCursor(Qt::OpenHandCursor);return;}
 if(event->key()==Qt::Key_Control){updateHoverCursor(mapFromGlobal(QCursor::pos()),Qt::ControlModifier);return;}
 if(tool_==Tool::Crop){
  // Plain Enter applies only the pending crop; upload requires Ctrl+Enter.
  if((event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)&&!(event->modifiers()&Qt::ControlModifier)){applyCrop();return;}
  if(event->key()==Qt::Key_Escape){cancelCrop();return;}
 }
 const int step=(event->modifiers()&Qt::ShiftModifier)?10:1;QPoint nudge;
 switch(event->key()){case Qt::Key_Left:nudge={-step,0};break;case Qt::Key_Right:nudge={step,0};break;case Qt::Key_Up:nudge={0,-step};break;case Qt::Key_Down:nudge={0,step};break;default:break;}
 if(!nudge.isNull()){
  if(tool_==Tool::Crop){auto r=cropDraft_.translated(nudge);if(doc_.imageRect().contains(r))cropDraft_=r;update();return;}
  if(const auto* current=doc_.find(selected_)){auto shape=*current;shape.rect.translate(nudge);shape.from+=nudge;shape.to+=nudge;if(doc_.replace(shape)){update();emit changed();}return;}
 }
 if((event->key()==Qt::Key_Delete||event->key()==Qt::Key_Backspace)&&selected_){if(doc_.remove(selected_)){selected_=0;emit selectionChanged();update();emit changed();}return;}
 if(event->key()==Qt::Key_Escape&&selected_){selected_=0;emit selectionChanged();update();return;}
 QWidget::keyPressEvent(event);
}
void EditorCanvas::keyReleaseEvent(QKeyEvent* event){
 if(event->key()==Qt::Key_Space&&!event->isAutoRepeat()){space_=false;updateHoverCursor(mapFromGlobal(QCursor::pos()),event->modifiers());return;}
 if(event->key()==Qt::Key_Control){updateHoverCursor(mapFromGlobal(QCursor::pos()),Qt::NoModifier);return;}
 QWidget::keyReleaseEvent(event);
}

bool EditorCanvas::eventFilter(QObject* watched,QEvent* event){
 if(watched==textEditor_&&event->type()==QEvent::KeyPress&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){finishText(false);return true;}
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
