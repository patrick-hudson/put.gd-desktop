#include "EditorCanvas.h"
#include "HighlightFilter.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QPushButton>
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
#include <limits>

namespace putgd::editor {
namespace {
// The image pixel under a fractional image position, like every other pixel index here.
QPoint pixelAt(const QPointF& image){return QPoint(int(std::floor(image.x())),int(std::floor(image.y())));}

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
// Pointer and keyboard edits keep marks on the visible image: points stop at
// its edges, and a mark already partly outside (cropped since) may move back
// in but never farther out.
QPointF inside(const QPointF& p,const QRect& bounds){
 return {std::clamp(p.x(),double(bounds.left()),double(bounds.left()+bounds.width())),std::clamp(p.y(),double(bounds.top()),double(bounds.top()+bounds.height()))};
}
QPoint inside(const QPoint& p,const QRect& bounds){
 return {std::clamp(p.x(),bounds.left(),bounds.left()+bounds.width()),std::clamp(p.y(),bounds.top(),bounds.top()+bounds.height())};
}
// An arrow end moved by one axis keeps the other where it was, even outside.
QPoint insideFrom(const QPoint& was,const QPoint& p,const QRect& bounds){
 auto axis=[](int now,int before,int low,int high){return std::clamp(now,std::min(low,before),std::max(high,before));};
 return {axis(p.x(),was.x(),bounds.left(),bounds.left()+bounds.width()),axis(p.y(),was.y(),bounds.top(),bounds.top()+bounds.height())};
}
QPoint containedMove(const Shape& shape,const QPoint& d,const QRect& bounds){
 // Line ends and pen points may sit on the image's far edge, one past its last pixel.
 const bool points=shape.twoPoint()||shape.kind==Shape::Kind::Pen;
 const auto box=shape.box();const auto room=points?bounds.adjusted(0,0,1,1):bounds;
 auto axis=[](int delta,int low,int high){return std::clamp(delta,std::min(low,0),std::max(high,0));};
 return {axis(d.x(),room.left()-box.left(),room.right()-box.right()),axis(d.y(),room.top()-box.top(),room.bottom()-box.bottom())};
}
void translateShape(Shape& shape,const QPoint& d){shape.rect.translate(d);shape.from+=d;shape.to+=d;for(auto& p:shape.points)p+=d;}
// Shift snaps a line to the nearest 45°, kept as long as the image allows.
QPoint snapLine(const QPoint& from,const QPointF& to,const QRect& bounds){
 const QPointF v=to-QPointF(from);const double length=std::hypot(v.x(),v.y());if(length<1)return from;
 const double angle=std::round(std::atan2(v.y(),v.x())/(M_PI/4))*(M_PI/4);
 const QPointF dir(std::cos(angle),std::sin(angle));double reach=length;
 auto limit=[&](double start,double step,double low,double high){if(step>1e-9)reach=std::min(reach,(high-start)/step);else if(step<-1e-9)reach=std::min(reach,(low-start)/step);};
 limit(from.x(),dir.x(),bounds.left(),bounds.left()+bounds.width());limit(from.y(),dir.y(),bounds.top(),bounds.top()+bounds.height());
 // Whole pixels along the diagonal keep it exactly 45°.
 const bool diagonal=std::abs(std::abs(dir.x())-std::abs(dir.y()))<1e-6;
 if(diagonal){const int side=int(std::floor(std::max(0.0,reach)*std::abs(dir.x())+1e-9));return from+QPoint(dir.x()>0?side:-side,dir.y()>0?side:-side);}
 return from+QPoint(int(std::lround(dir.x()*std::max(0.0,reach))),int(std::lround(dir.y()*std::max(0.0,reach))));
}
double distanceToStroke(const QPointF& p,const QList<QPoint>& points){
 double best=std::numeric_limits<double>::max();
 if(points.size()==1)return QLineF(p,QPointF(points.first())).length();
 for(qsizetype i=1;i<points.size();++i){
  const QPointF a(points[i-1]),b(points[i]);const QPointF ab=b-a;const double length=QPointF::dotProduct(ab,ab);
  const double t=length<=0?0:std::clamp(QPointF::dotProduct(p-a,ab)/length,0.0,1.0);best=std::min(best,QLineF(p,a+t*ab).length());
 }
 return best;
}
// The distance from an ellipse outline, approximated through its normalized radius.
double distanceToEllipse(const QPointF& p,const QRectF& box){
 const double a=std::max(0.5,box.width()/2),b=std::max(0.5,box.height()/2);const auto c=box.center();
 const double r=std::hypot((p.x()-c.x())/a,(p.y()-c.y())/b);return std::abs(r-1)*std::min(a,b);
}
QColor canvasBackground(){return Theme::current().well;}
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
 case Shape::Kind::Rectangle:return "Rectangle";case Shape::Kind::Highlight:return "Highlight";case Shape::Kind::Text:return "Text label";
 case Shape::Kind::Step:return "Step";case Shape::Kind::Pen:return "Pen stroke";case Shape::Kind::Ellipse:return "Ellipse";case Shape::Kind::Line:return "Line";}
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
 // A document can open with steps already on it (an agent's first edits).
 for(const auto& shape:doc_.shapes())if(shape.kind==Shape::Kind::Step){nextStep_=doc_.nextStepNumber();break;}
 // Screen readers hear the selection, or the crop, and the keys that act on it.
 connect(this,&EditorCanvas::selectionChanged,this,&EditorCanvas::announce);connect(this,&EditorCanvas::changed,this,&EditorCanvas::announce);
 auto* warning=new Ui::Banner(Ui::Banner::Tone::Warning,{},this);textWarning_=warning;textWarning_->setObjectName("annotationTextWarning");
 warning->textLabel()->setObjectName("annotationTextWarningText");Ui::setLocked(warning->textLabel(),true);warning->setAction("Select affected text");
 warning->actionButton()->setObjectName("selectAffectedText");
 connect(warning->actionButton(),&QPushButton::clicked,this,[this]{selectAffectedText();});textWarning_->hide();
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
 setCursor(tool_==Tool::Select?Qt::ArrowCursor:tool_==Tool::CopyText?Qt::IBeamCursor:Qt::CrossCursor);
 if(fitted_)fit();else update();
 announce();
}

void EditorCanvas::setStyle(const ToolStyle& style){style_=style;update();}

void EditorCanvas::applyStyleToSelection(){
 const auto& style=style_;
 if(const auto* current=doc_.find(selected_)){
  auto shape=*current;
  using Kind=Shape::Kind;const auto k=shape.kind;
  if(k!=Kind::Redact&&k!=Kind::Blur)shape.color=style.color;
  if(k==Kind::Arrow||k==Kind::Rectangle||k==Kind::Line||k==Kind::Ellipse||k==Kind::Pen)shape.width=style.width;
  if(k==Kind::Blur){shape.strength=style.strength;shape.pixelate=style.pixelate;}
  if(k==Kind::Text){shape.fontPixels=style.fontPixels;shape.fontFamily=style.fontFamily;}
  if(shape.kind==Shape::Kind::Text&&!shape.text.isEmpty())shape=boundedText(*current,shape);
  if(doc_.replace(shape))emit changed();
  emit selectionChanged();
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
 fitted_=true;placeTextEditor();update();emit viewChanged();
}
void EditorCanvas::actualSize(){zoomBy(1.0/scale_,QPointF(width()/2.0,height()/2.0));}
void EditorCanvas::zoomBy(double factor,QPointF anchor){
 if(anchor.x()<0)anchor=QPointF(width()/2.0,height()/2.0);
 const auto image=toImage(anchor);
 scale_=std::clamp(scale_*factor,minimumZoom,maximumZoom);
 offset_=anchor-(image-QPointF(visibleBounds().topLeft()))*scale_;
 fitted_=false;clampOffset();placeTextEditor();update();emit viewChanged();
}
void EditorCanvas::setZoom(double scale){zoomBy(scale/scale_);}
void EditorCanvas::panBy(const QPointF& delta){offset_+=delta;fitted_=false;clampOffset();placeTextEditor();update();emit viewChanged();}
QRect EditorCanvas::visibleImageRect() const {return boxFrom(toImage(QPointF(0,0)),toImage(QPointF(width(),height()))).intersected(visibleBounds());}
void EditorCanvas::clampOffset(){
 const auto bounds=visibleBounds();const double w=bounds.width()*scale_,h=bounds.height()*scale_;
 auto axis=[](double offset,double content,double room){return content<=room?(room-content)/2:std::clamp(offset,room-content-48,48.0);};
 offset_=QPointF(axis(offset_.x(),w,width()),axis(offset_.y(),h,height()));
}
void EditorCanvas::resizeEvent(QResizeEvent*){
 // Never move the image while a gesture maps pointer positions through it.
 if(drag_!=Drag::None){update();return;}
 if(fitted_)fit();else{clampOffset();placeTextEditor();update();}
}

Shape::Kind EditorCanvas::kindFor(Tool tool) const {
 switch(tool){case Tool::Redact:return Shape::Kind::Redact;case Tool::Blur:return Shape::Kind::Blur;case Tool::Arrow:return Shape::Kind::Arrow;
 case Tool::Highlight:return Shape::Kind::Highlight;case Tool::Text:return Shape::Kind::Text;case Tool::Step:return Shape::Kind::Step;
 case Tool::Pen:return Shape::Kind::Pen;case Tool::Ellipse:return Shape::Kind::Ellipse;case Tool::Line:return Shape::Kind::Line;default:return Shape::Kind::Rectangle;}
}
bool EditorCanvas::drawingTool() const {return tool_==Tool::Redact||tool_==Tool::Blur||tool_==Tool::Arrow||tool_==Tool::Rectangle||tool_==Tool::Highlight||tool_==Tool::Step||tool_==Tool::Pen||tool_==Tool::Ellipse||tool_==Tool::Line;}
void EditorCanvas::setNextStepNumber(int number){
 nextStep_=std::clamp(number,1,maximumStepNumber);
 // With no steps yet, this is where counting starts again after an undo.
 bool any=false;for(const auto& shape:doc_.shapes())any=any||shape.kind==Shape::Kind::Step;
 if(!any)stepStart_=nextStep_;
 announce();
}

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
  if(it->twoPoint()){if(distanceToSegment(p,it->from,it->to)<=std::max(tolerance,double(it->width)))return it->id;continue;}
  if(it->kind==Shape::Kind::Pen){if(distanceToStroke(p,it->points)<=std::max(tolerance,double(it->width)))return it->id;continue;}
  const QRectF box(it->rect);
  if(!box.adjusted(-tolerance,-tolerance,tolerance,tolerance).contains(p))continue;
  if(it->kind==Shape::Kind::Rectangle){const double edge=tolerance+it->width/2.0;if(box.adjusted(edge,edge,-edge,-edge).contains(p))continue;}
  // An ellipse is grabbed by its outline too, so its inside stays drawable.
  if(it->kind==Shape::Kind::Ellipse){const auto inset=QRectF(box).adjusted(it->width/2.0,it->width/2.0,-it->width/2.0,-it->width/2.0);if(distanceToEllipse(p,inset)>tolerance+it->width/2.0)continue;}
  if(it->kind==Shape::Kind::Step&&QLineF(p,box.center()).length()>box.width()/2+tolerance)continue;
  return it->id;
 }
 return 0;
}
void EditorCanvas::updateHoverCursor(const QPointF& position,Qt::KeyboardModifiers modifiers){
 if(drag_!=Drag::None)return;
 if(space_){setCursor(Qt::OpenHandCursor);return;}
 if(tool_==Tool::Crop){const int h=handleAt(cropDraft_,position);setCursor(h>=0?resizeCursor(h):QRectF(cropDraft_).contains(toImage(position))?Qt::SizeAllCursor:Qt::CrossCursor);return;}
 if(tool_==Tool::CopyText){setCursor(textLayer_&&textLayer_->wordAt(toImage(position))>=0?Qt::IBeamCursor:Qt::ArrowCursor);return;}
 const auto fallback=tool_==Tool::Select?Qt::ArrowCursor:Qt::CrossCursor;
 // Ctrl draws a new shape even over existing ones.
 if(drawingTool()&&(modifiers&Qt::ControlModifier)){setCursor(fallback);return;}
 const auto image=toImage(position);
 if(const auto* shape=doc_.find(selected_);shape&&(tool_!=Tool::Text||shape->kind==Shape::Kind::Text)){
  const int h=selectionHandleAt(*shape,position);
  if(h>=0){setCursor(shape->twoPoint()?Qt::SizeAllCursor:resizeCursor(h));return;}
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
  if(it->twoPoint()){if(distanceToSegment(p,it->from,it->to)<=std::max(tolerance,double(it->width)))return it->id;continue;}
  if(it->kind==Shape::Kind::Pen){if(distanceToStroke(p,it->points)<=std::max(tolerance,double(it->width)))return it->id;continue;}
  if(QRectF(it->rect).adjusted(-tolerance,-tolerance,tolerance,tolerance).contains(p))return it->id;
 }
 return 0;
}
int EditorCanvas::selectionHandleAt(const Shape& shape,const QPointF& p) const {
 if(shape.twoPoint())return arrowHandleAt(shape,p);
 return handleAt(shape.box(),p,shape.kind==Shape::Kind::Text||shape.kind==Shape::Kind::Step);
}
// Size edits stop where both the layout box and the exported glyph stroke
// fit. A label clipped by a later crop can shrink without jumping its anchor.
Shape EditorCanvas::boundedText(const Shape& original,Shape requested,int handle) const {
 const QRectF allowed=QRectF(doc_.crop()).united(QRectF(original.rect)).united(textOutputBounds(original));
 for(int pixels=requested.fontPixels;pixels>=minimumFontPixels;--pixels){
  requested.fontPixels=pixels;
  const QFontMetrics metrics(textFont(requested.fontFamily,pixels));
  requested.rect.setSize(QSize(metrics.horizontalAdvance(requested.text)+1,metrics.height()));
  if(handle==0)requested.rect.moveBottomRight(original.rect.bottomRight());
  else if(handle==2)requested.rect.moveBottomLeft(original.rect.bottomLeft());
  else if(handle==6)requested.rect.moveTopRight(original.rect.topRight());
  else requested.rect.moveTopLeft(original.rect.topLeft());
  if(allowed.contains(QRectF(requested.rect))&&allowed.contains(textOutputBounds(requested)))return requested;
 }
 return original;
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
 return boundedText(original,shape,handle);
}
// A step corner scales the circle around the opposite corner, as text does.
Shape EditorCanvas::scaledStep(const Shape& original,int handle,const QPointF& now) const {
 const QRectF box(original.rect);
 const QPointF corners[4]={box.topLeft(),box.topRight(),box.bottomRight(),box.bottomLeft()};
 const QPointF anchor=corners[(handle/2+2)%4];
 const auto area=doc_.crop();
 // The side follows the pointer's larger reach from the fixed corner, and
 // stops where the circle would leave the image.
 const QPointF clamped=inside(now,area);
 int side=int(std::lround(std::max(std::abs(clamped.x()-anchor.x()),std::abs(clamped.y()-anchor.y()))));
 const bool left=handle==0||handle==6,top=handle==0||handle==2;
 const int room=std::min(left?int(std::lround(anchor.x()))-area.left():area.left()+area.width()-int(std::lround(anchor.x())),top?int(std::lround(anchor.y()))-area.top():area.top()+area.height()-int(std::lround(anchor.y())));
 side=std::clamp(std::min(side,std::max(room,minimumStepPixels)),minimumStepPixels,maximumStepPixels);
 auto shape=original;
 const QPoint fixed(int(std::lround(anchor.x())),int(std::lround(anchor.y())));
 shape.rect=QRect(left?fixed.x()-side:fixed.x(),top?fixed.y()-side:fixed.y(),side,side);
 return shape;
}
// The edges a handle moves stay inside bounds; one already outside (a mark
// cropped since) may come in but never jumps there or moves farther out.
QRect EditorCanvas::resized(QRect r,int handle,const QPoint& d,const QRect& bounds) const {
 auto edge=[](int now,int delta,int low,int high){return std::clamp(now+delta,std::min(low,now),std::max(high,now));};
 if(handle==0||handle==6||handle==7)r.setLeft(edge(r.left(),d.x(),bounds.left(),bounds.right()));
 if(handle==2||handle==3||handle==4)r.setRight(edge(r.right(),d.x(),bounds.left(),bounds.right()));
 if(handle==0||handle==1||handle==2)r.setTop(edge(r.top(),d.y(),bounds.top(),bounds.bottom()));
 if(handle==4||handle==5||handle==6)r.setBottom(edge(r.bottom(),d.y(),bounds.top(),bounds.bottom()));
 return r.normalized();
}

const QImage& EditorCanvas::blurTile(const Shape& shape){
 QString key=QString("%1%2/").arg(shape.pixelate?"P":"B").arg(shape.strength);
 for(const auto& other:doc_.shapes())if(other.kind==Shape::Kind::Redact&&other.rect.intersects(shape.rect))key+=QString(";%1,%2,%3,%4").arg(other.rect.x()).arg(other.rect.y()).arg(other.rect.width()).arg(other.rect.height());
 return cachedTile(shape,key,&EditDocument::blurPreview);
}
const QImage& EditorCanvas::highlightTile(const Shape& shape){
 // Redactions and blurs in the preview's window (the box plus the reach of
 // blurs crossing its edge) change its pixels, so they key the tile.
 QString key=QString("H%1/").arg(shape.color.name());const auto window=doc_.highlightWindow(shape);
 for(const auto& other:doc_.shapes())if((other.kind==Shape::Kind::Redact||other.kind==Shape::Kind::Blur)&&other.rect.intersects(window))key+=QString(";%1,%2,%3,%4,%5").arg(other.rect.x()).arg(other.rect.y()).arg(other.rect.width()).arg(other.rect.height()).arg(other.kind==Shape::Kind::Blur?(other.pixelate?-other.strength:other.strength):0);
 return cachedTile(shape,key,&EditDocument::highlightPreview);
}
const QImage& EditorCanvas::cachedTile(const Shape& shape,const QString& prefix,QImage (EditDocument::*render)(const Shape&,double) const){
 const double bucket=std::pow(2.0,std::round(std::log2(std::max(scale_,1e-3))*2)/2);
 const QString key=prefix+QString("%1,%2,%3,%4/%5/%6,%7,%8,%9").arg(shape.rect.x()).arg(shape.rect.y()).arg(shape.rect.width()).arg(shape.rect.height()).arg(bucket)
  .arg(doc_.crop().x()).arg(doc_.crop().y()).arg(doc_.crop().width()).arg(doc_.crop().height());
 auto& tile=tiles_[shape.id];tile.used=++tick_;
 if(tile.key==key)return tile.image;
 cacheBytes_-=tile.image.sizeInBytes();tile.image=(doc_.*render)(shape,bucket);tile.key=key;cacheBytes_+=tile.image.sizeInBytes();
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
 bool steps=false;for(const auto& shape:doc_.shapes())steps=steps||shape.kind==Shape::Kind::Step;
 nextStep_=steps?doc_.nextStepNumber():stepStart_;
 QSet<quint64> live;for(const auto& shape:doc_.shapes())live.insert(shape.id);
 for(auto it=tiles_.begin();it!=tiles_.end();){if(!live.contains(it.key())){cacheBytes_-=it->image.sizeInBytes();it=tiles_.erase(it);}else ++it;}
 if(fitted_)fit();else{clampOffset();placeTextEditor();update();}
}

void EditorCanvas::paintEvent(QPaintEvent*){
 placeTextWarning();
 QPainter painter(this);painter.fillRect(rect(),canvasBackground());
 const auto bounds=visibleBounds();const auto target=toWidget(bounds);
 Ui::paintImageShadow(painter,target);
 // Paint only the visible source rectangle; never allocate a scaled full frame.
 const QRectF visible=target.intersected(QRectF(rect()));
 if(!visible.isEmpty()){
  painter.setRenderHint(QPainter::SmoothPixmapTransform,scale_<1.0);
  painter.save();painter.setClipRect(visible,Qt::IntersectClip);
  doc_.paintPreviewSource(painter,target,QRectF(bounds),[&](QPainter& p,const QRectF& patch){
   p.fillRect(patch,canvasBackground());
   if(!target.adjusted(4,4,-4,-4).contains(patch))Ui::paintImageShadow(p,target);
  },bounds);painter.restore();
 }
 painter.save();painter.setClipRect(target);
 painter.translate(offset_);painter.scale(scale_,scale_);painter.translate(-QPointF(bounds.topLeft()));
 // Effects paint in the export's order, whatever order they were added in:
 // redactions, then blurs (whose tiles hold the redactions they cover,
 // blurred), then highlights (which tint the blurred result), then the drawn
 // annotations.
 for(const auto& shape:doc_.shapes())if(shape.kind==Shape::Kind::Blur){const auto& tile=blurTile(shape);const auto area=shape.rect.intersected(doc_.crop());if(!tile.isNull())painter.drawImage(QRectF(area),tile);}
 for(const auto& shape:doc_.shapes())if(shape.kind==Shape::Kind::Highlight){const auto& tile=highlightTile(shape);const auto area=shape.rect.intersected(doc_.crop());if(!tile.isNull())painter.drawImage(QRectF(area),tile);}
 for(const auto& shape:doc_.shapes())if(shape.kind!=Shape::Kind::Redact&&shape.kind!=Shape::Kind::Blur&&shape.kind!=Shape::Kind::Highlight&&!(textEditor_&&shape.id==textShape_))EditDocument::paintAnnotation(painter,shape);
 // The open field shows the text being typed; painting it here as well showed it twice.
 // Copy text: the selected words are tinted with the selection colour.
 if(tool_==Tool::CopyText&&textLayer_&&textLayer_->hasSelection()){
  auto tint=palette().color(QPalette::Highlight);tint.setAlpha(96);
  for(int i=textLayer_->first();i<=textLayer_->last();++i)painter.fillRect(QRectF(textLayer_->words().at(i).rect).adjusted(-1,-1,1,1),tint);
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
 if(tool_==Tool::Crop)if(const auto* shape=doc_.find(selected_);shape&&shape->kind==Shape::Kind::Text)dualRect(painter,toWidget(shape->rect).adjusted(-2,-2,2,2),guide(),1,Qt::DashLine);
 // Handles: amber squares in a dark rim, legible on any image.
 auto handles=[&](const QList<QPointF>& points){painter.setPen(QPen(underStroke(),2));painter.setBrush(guide());for(const auto& point:points)painter.drawRect(QRectF(point-QPointF(4,4),QSizeF(8,8)));};
 if(tool_==Tool::Crop){const auto r=toWidget(cropDraft_);handles({r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())});}
 else if(const auto* shape=doc_.find(selected_);shape&&!textEditor_){
  if(shape->twoPoint())handles({toWidget(QPointF(shape->from)),toWidget(QPointF(shape->to))});
  else{
   const auto r=toWidget(shape->box());dualRect(painter,r.adjusted(-2,-2,2,2),guide(),1,Qt::DashLine);
   if(shape->kind==Shape::Kind::Text||shape->kind==Shape::Kind::Step)handles({r.topLeft(),r.topRight(),r.bottomRight(),r.bottomLeft()});
   else handles({r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())});
  }
 }
 if(textEditor_){
  const auto anchor=toWidget(textAnchor_);const QRectF field(textEditor_->geometry());
  const QPointF end(std::clamp(anchor.x(),field.left(),field.right()),std::clamp(anchor.y(),field.top(),field.bottom()));
  dualLine(painter,anchor,end,guide(),Qt::DashLine);
  dualLine(painter,anchor-QPointF(7,0),anchor+QPointF(7,0),guide(),Qt::SolidLine);
  dualLine(painter,anchor-QPointF(0,7),anchor+QPointF(0,7),guide(),Qt::SolidLine);
  painter.setBrush(guide());painter.setPen(QPen(underStroke(),1));painter.drawEllipse(anchor,3,3);
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
 if(tool_==Tool::CopyText){textPress(event);return;}
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
  // A new label starts on the image, not in the surrounding margin.
  if(QRectF(doc_.crop()).contains(image))beginText(image,nullptr);
  return;
 }
 // Shift-click grows the selected highlight by the word under the pointer,
 // inside the selection too, so it comes before the move and resize hit tests.
 if(tool_==Tool::Highlight&&(event->modifiers()&Qt::ShiftModifier))if(const auto* current=doc_.find(selected_);current&&current->kind==Shape::Kind::Highlight){
  const auto word=highlight::wordAt(doc_.base(),pixelAt(image),doc_.redactionRects());
  if(!word.isNull()){auto next=*current;next.rect=next.rect.united(word).intersected(doc_.crop());doc_.replace(next);update();emit changed();}
  return;
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
 shape.width=style_.width;shape.strength=style_.strength;shape.pixelate=style_.pixelate;shape.fontPixels=style_.fontPixels;shape.fontFamily=style_.fontFamily;
 // A press in the margin starts the mark at the nearest image edge.
 const auto start=inside(image,doc_.crop());
 const QPoint at(int(std::floor(start.x())),int(std::floor(start.y())));
 shape.from=shape.to=at;shape.rect=boxFrom(start,start);
 if(shape.kind==Shape::Kind::Pen)shape.points={QPoint(int(std::lround(start.x())),int(std::lround(start.y())))};
 // A step drops centred on the click with the next number; dragging before
 // release places it, and a step never leaves the image.
 if(shape.kind==Shape::Kind::Step){
  const int side=std::min({style_.stepPixels,doc_.crop().width(),doc_.crop().height()});shape.number=nextStep_;
  shape.rect=QRect(at.x()-side/2,at.y()-side/2,side,side);
  const auto& area=doc_.crop();shape.rect.moveLeft(std::clamp(shape.rect.left(),area.left(),std::max(area.left(),area.right()-side+1)));shape.rect.moveTop(std::clamp(shape.rect.top(),area.top(),std::max(area.top(),area.bottom()-side+1)));
 }
 doc_.beginGesture();selected_=doc_.add(shape);original_=*doc_.find(selected_);drag_=Drag::Create;emit selectionChanged();update();
}

void EditorCanvas::mouseMoveEvent(QMouseEvent* event){
 const auto position=event->position();
 if(selectingText_&&textLayer_){if(const int word=textLayer_->wordAt(toImage(position));word>=0){textLayer_->extendTo(word);update();emit textSelectionChanged();}return;}
 if(drag_==Drag::None){updateHoverCursor(position,event->modifiers());return;}
 // A press that has not travelled the platform drag distance is a click:
 // it selects (or, for text, edits) without nudging the shape.
 if(drag_==Drag::Move&&!moved_){if(QLineF(press_,position).length()<QGuiApplication::styleHints()->startDragDistance())return;moved_=true;}
 if(drag_==Drag::Pan){offset_+=position-last_;last_=position;fitted_=false;clampOffset();placeTextEditor();update();emit viewChanged();return;}
 const auto start=toImage(press_),now=toImage(position);
 const QPoint delta(int(std::lround(now.x()-start.x())),int(std::lround(now.y()-start.y())));
 last_=position;
 const auto image=doc_.imageRect();
 if(drag_==Drag::CropNew){cropDraft_=EditDocument::clampRect(boxFrom(start,now),image);if(cropDraft_.isEmpty())cropDraft_=cropOriginal_;update();return;}
 if(drag_==Drag::CropMove){auto moved=cropOriginal_.translated(delta);moved.moveLeft(std::clamp(moved.left(),0,image.width()-moved.width()));moved.moveTop(std::clamp(moved.top(),0,image.height()-moved.height()));cropDraft_=moved;update();return;}
 if(drag_==Drag::CropResize){const auto r=EditDocument::clampRect(resized(cropOriginal_,handle_,delta,image),image);if(!r.isEmpty())cropDraft_=r;update();return;}
 auto shape=original_;const auto bounds=doc_.crop();
 if(drag_==Drag::Create){
  const auto end=inside(now,bounds);
  if(shape.kind==Shape::Kind::Line&&(event->modifiers()&Qt::ShiftModifier))shape.to=snapLine(shape.from,end,bounds);
  else if(shape.twoPoint())shape.to=QPoint(int(std::lround(end.x())),int(std::lround(end.y())));
  else if(shape.kind==Shape::Kind::Pen){
   // Each move adds a point once the pointer has travelled two screen pixels.
   const auto* live=doc_.find(selected_);shape=live?*live:original_;
   const QPoint point(int(std::lround(end.x())),int(std::lround(end.y())));
   if(QLineF(toWidget(QPointF(shape.points.last())),toWidget(QPointF(point))).length()>=2)shape.points.append(point);
  }else if(shape.kind==Shape::Kind::Step){
   const auto d=containedMove(shape,delta,bounds);translateShape(shape,d);
  }else{
   const auto from=inside(start,bounds);
   // A drag wholly in one margin is discarded rather than kept as a sliver
   // on the image's edge.
   auto beyond=[](double a,double b,double low,double high){return (a<low&&b<low)||(a>high&&b>high);};
   if(beyond(start.x(),now.x(),bounds.left(),bounds.left()+bounds.width())||beyond(start.y(),now.y(),bounds.top(),bounds.top()+bounds.height()))shape.rect=QRect();
   else if(event->modifiers()&Qt::ShiftModifier){
    // The square grows from the pressed pixel toward the pointer and covers
    // every pixel the pointer reached, as far as the image allows.
    const int px=std::clamp(int(std::floor(start.x())),bounds.left(),bounds.right()),py=std::clamp(int(std::floor(start.y())),bounds.top(),bounds.bottom());
    const bool right=now.x()>=start.x(),down=now.y()>=start.y();
    const int reachX=right?int(std::ceil(end.x()))-px:px-int(std::floor(end.x()))+1,reachY=down?int(std::ceil(end.y()))-py:py-int(std::floor(end.y()))+1;
    const int roomX=right?bounds.right()-px+1:px-bounds.left()+1,roomY=down?bounds.bottom()-py+1:py-bounds.top()+1;
    const int side=std::min({std::max({reachX,reachY,1}),roomX,roomY});
    shape.rect=QRect(right?px:px-side+1,down?py:py-side+1,side,side);
   }else shape.rect=boxFrom(from,end).intersected(bounds);
   // A highlight follows the text lines it crosses; Alt keeps the free box.
   // Snapping waits for a real drag: a click with a pixel of jitter stays tiny and highlights the word on release.
   if(shape.kind==Shape::Kind::Highlight&&!shape.rect.isEmpty()&&!(event->modifiers()&Qt::AltModifier)&&QLineF(press_,event->position()).length()>=QGuiApplication::styleHints()->startDragDistance())shape.rect=highlight::snapToText(doc_.base(),shape.rect,doc_.redactionRects()).intersected(bounds);
  }
 }else if(drag_==Drag::Move){const auto d=containedMove(shape,delta,bounds);translateShape(shape,d);}
 else if(drag_==Drag::Resize){
  if(shape.twoPoint()){
   auto& end=handle_==0?shape.from:shape.to;const auto& other=handle_==0?shape.to:shape.from;
   // Shift keeps a line on the nearest 45° from its other end.
   if(shape.kind==Shape::Kind::Line&&(event->modifiers()&Qt::ShiftModifier))end=snapLine(other,inside(now,bounds),bounds);
   else end=insideFrom(end,end+delta,bounds);
  }
  else if(shape.kind==Shape::Kind::Text)shape=scaledText(original_,handle_,now);
  else if(shape.kind==Shape::Kind::Step)shape=scaledStep(original_,handle_,now);
  else if(shape.kind==Shape::Kind::Pen){const auto from=original_.box();auto to=resized(from,handle_,delta,bounds);if(to.width()<1)to.setWidth(1);if(to.height()<1)to.setHeight(1);shape.points=fitStroke(original_.points,from,to);}
  else shape.rect=resized(shape.rect,handle_,delta,bounds);
 }
 doc_.preview(shape);update();
 // The size control follows a text label while its corner is dragged.
 if(drag_==Drag::Resize&&shape.kind==Shape::Kind::Text)emit selectionChanged();
}

void EditorCanvas::mouseReleaseEvent(QMouseEvent* event){
 if(drag_==Drag::Pan){drag_=Drag::None;updateHoverCursor(event->position(),event->modifiers());return;}
 if(event->button()!=Qt::LeftButton)return;
 if(selectingText_){selectingText_=false;return;}
 const auto kind=drag_;drag_=Drag::None;const auto edit=std::exchange(editOnClick_,0);
 if(kind==Drag::CropNew||kind==Drag::CropMove||kind==Drag::CropResize||kind==Drag::None)return;
 if(kind==Drag::Create){
  const auto* shape=doc_.find(selected_);
  // A pen click leaves a dot and a step is placed by its click; neither is too small.
  const bool tiny=!shape||(shape->twoPoint()?QLineF(toWidget(QPointF(shape->from)),toWidget(QPointF(shape->to))).length()<6:shape->kind==Shape::Kind::Pen||shape->kind==Shape::Kind::Step?false:(shape->rect.width()*scale_<4||shape->rect.height()*scale_<4));
  if(shape&&shape->kind==Shape::Kind::Pen){auto next=*shape;next.points=smoothStroke(next.points);doc_.preview(next);}
  if(shape&&shape->kind==Shape::Kind::Step)nextStep_=std::min(shape->number+1,maximumStepNumber);
  // A click, or a press that never travelled the drag distance, highlights the word under the pointer.
  const bool click=tiny||QLineF(press_,event->position()).length()<QGuiApplication::styleHints()->startDragDistance();
  if(click&&shape&&shape->kind==Shape::Kind::Highlight&&!(event->modifiers()&Qt::AltModifier)){
   const auto word=highlight::wordAt(doc_.base(),pixelAt(toImage(press_)),doc_.redactionRects()).intersected(doc_.crop());
   if(!word.isEmpty()){auto next=*shape;next.rect=word;doc_.preview(next);doc_.commitGesture();update();emit changed();updateHoverCursor(event->position(),event->modifiers());return;}
  }
  if(tiny){doc_.cancelGesture();selected_=0;emit selectionChanged();update();return;}
 }
 doc_.commitGesture();
 if(kind==Drag::Move&&edit&&!moved_)if(const auto* shape=doc_.find(edit)){beginText(QPointF(shape->rect.topLeft()),shape);return;}
 update();emit changed();updateHoverCursor(event->position(),event->modifiers());
}

void EditorCanvas::mouseDoubleClickEvent(QMouseEvent* event){
 // Copy text: a double-click takes the line under the pointer.
 if(tool_==Tool::CopyText){if(textLayer_){const int word=textLayer_->wordAt(toImage(event->position()));if(word>=0){textLayer_->selectLine(word);selectingText_=false;update();emit textSelectionChanged();}}return;}
 const auto* shape=doc_.find(shapeAt(toImage(event->position())));
 if(shape&&shape->kind==Shape::Kind::Text){beginText(QPointF(shape->rect.topLeft()),shape);return;}
 // The first click made a word highlight; the double-click grows it to the line.
 if(tool_==Tool::Highlight&&!(event->modifiers()&Qt::AltModifier))if(const auto* current=doc_.find(selected_);current&&current->kind==Shape::Kind::Highlight){
  const auto line=highlight::lineAt(doc_.base(),pixelAt(toImage(event->position())),doc_.redactionRects()).intersected(doc_.crop());
  if(!line.isEmpty()&&line!=current->rect){auto next=*current;next.rect=line.united(current->rect);doc_.replace(next);update();emit changed();}
 }
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
 if(tool_==Tool::CopyText&&textKey(event))return;
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
 auto shape=*current;using Kind=Shape::Kind;const auto area=doc_.crop();
 if(!resize){const auto step=containedMove(shape,d,area);translateShape(shape,step);}
 else if(shape.twoPoint()){shape.to=insideFrom(shape.to,shape.to+d,area);if(shape.to==shape.from)return true;}
 else if(shape.kind==Kind::Text){
  shape.fontPixels=std::clamp(shape.fontPixels+d.x()+d.y(),minimumFontPixels,maximumFontPixels);
  shape=boundedText(*current,shape);
 }else if(shape.kind==Kind::Step){
  // The circle grows from its top-left corner, as far as the image allows.
  const int room=std::min(area.left()+area.width()-shape.rect.left(),area.top()+area.height()-shape.rect.top());
  const int side=std::clamp(shape.rect.width()+d.x()+d.y(),minimumStepPixels,std::max(minimumStepPixels,std::min(maximumStepPixels,room)));
  if(side==shape.rect.width())return true;shape.rect.setSize(QSize(side,side));
 }else if(shape.kind==Kind::Pen){
  const auto from=shape.box();auto to=from;
  to.setRight(std::clamp(from.right()+d.x(),from.left(),std::max(area.right()+1,from.right())));to.setBottom(std::clamp(from.bottom()+d.y(),from.top(),std::max(area.bottom()+1,from.bottom())));
  if(to==from)return true;shape.points=fitStroke(shape.points,from,to);
 }else{
  // The moving edges stay on the crop; one already outside may only come in.
  shape.rect.setRight(std::clamp(shape.rect.right()+d.x(),std::min(area.left(),current->rect.right()),std::max(area.right(),current->rect.right())));
  shape.rect.setBottom(std::clamp(shape.rect.bottom()+d.y(),std::min(area.top(),current->rect.bottom()),std::max(area.bottom(),current->rect.bottom())));
  if(shape.rect.width()<1||shape.rect.height()<1)return true;}
 const auto bounds=shape.twoPoint()||shape.kind==Kind::Pen?shape.box().adjusted(0,0,1,1):shape.rect;
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
 shape.width=style_.width;shape.strength=style_.strength;shape.pixelate=style_.pixelate;shape.fontPixels=style_.fontPixels;shape.fontFamily=style_.fontFamily;
 // A quarter of the visible image, centred; another placement steps down and
 // right rather than hiding exactly under the last one. A step is its own size.
 const bool step=shape.kind==Shape::Kind::Step;const int side=std::min({style_.stepPixels,area.width(),area.height()});
 QRect box(QPoint(),step?QSize(side,side):QSize(std::max(1,area.width()/2),std::max(1,area.height()/2)));box.moveCenter(area.center());
 const int offset=step?side+side/4:std::max(8,std::min(area.width(),area.height())/20);
 auto occupied=[&](const QRect& r){for(const auto& other:doc_.shapes())if(other.kind==shape.kind&&(other.twoPoint()?other.from==r.bottomLeft():other.kind==Shape::Kind::Pen?other.box().left()==r.left()&&other.box().center().y()==r.center().y():other.rect==r))return true;return false;};
 for(int i=0;i<16&&occupied(box);++i){const auto moved=box.translated(offset,step?0:offset);if(!area.contains(moved))break;box=moved;}
 shape.rect=box;shape.from=box.bottomLeft();shape.to=box.topRight();
 if(step)shape.number=nextStep_;
 // A pen mark without a pointer is one gentle wave across the box.
 if(shape.kind==Shape::Kind::Pen)for(int i=0;i<=32;++i)shape.points.append(QPoint(box.left()+int(std::lround(box.width()*i/32.0)),box.center().y()+int(std::lround(std::sin(i*M_PI/8)*box.height()/6.0))));
 selected_=doc_.add(shape);if(step)nextStep_=std::min(shape.number+1,maximumStepNumber);emit selectionChanged();update();emit changed();
 return selected_;
}

std::optional<QRect> EditorCanvas::selectionGeometry() const {
 if(tool_==Tool::Crop)return cropDraft_;
 const auto* shape=doc_.find(selected_);if(!shape)return std::nullopt;
 if(shape->twoPoint())return QRect(shape->from.x(),shape->from.y(),shape->to.x()-shape->from.x(),shape->to.y()-shape->from.y());
 return shape->box();
}
bool EditorCanvas::setSelectionGeometry(const QRect& r){
 const auto image=doc_.imageRect();
 if(tool_==Tool::Crop){
  const auto clamped=EditDocument::clampRect(r,image);if(r.width()<1||r.height()<1||clamped.isEmpty())return false;
  cropDraft_=clamped;update();announce();emit viewChanged();return true;
 }
 const auto* current=doc_.find(selected_);if(!current)return false;
 auto shape=*current;using Kind=Shape::Kind;
 if(shape.twoPoint()){shape.from=r.topLeft();shape.to=QPoint(r.x()+r.width(),r.y()+r.height());if(shape.from==shape.to)return false;}
 else if(shape.kind==Kind::Text)shape.rect.moveTopLeft(r.topLeft());
 else if(shape.kind==Kind::Step){
  // A step stays round: the changed side sets the diameter.
  const int side=r.width()!=current->rect.width()?r.width():r.height();
  if(side<minimumStepPixels||side>maximumStepPixels)return false;shape.rect=QRect(r.topLeft(),QSize(side,side));
 }else if(shape.kind==Kind::Pen){if(r.width()<1||r.height()<1)return false;shape.points=fitStroke(shape.points,shape.box(),r);}
 else{if(r.width()<1||r.height()<1)return false;shape.rect=r;if(shape.kind==Kind::Redact||shape.kind==Kind::Blur)shape.rect=EditDocument::clampRect(r,image);}
 const auto bounds=shape.twoPoint()||shape.kind==Kind::Pen?shape.box().adjusted(0,0,1,1):shape.rect;
 if(!bounds.intersects(image)||!doc_.replace(shape))return false;
 update();emit changed();return true;
}

QString EditorCanvas::selectionSummary() const {
 const QString panHelp=" Alt+arrow keys pan the image; Alt+Shift+arrow keys pan farther.";
 auto box=[](const QRect& r){return QString("x %1, y %2, %3 × %4 px").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());};
 if(tool_==Tool::Crop){
  const auto* affected=doc_.find(selected_);
  const auto selection=affected?QString(" Affected text selected: \"%1\". The crop is still pending.").arg(affected->text):QString{};
  return QString("Crop area %1. Arrow keys move it, Shift+arrow keys resize it, Enter applies, Escape cancels.").arg(box(cropDraft_))+selection+panHelp;
 }
 const auto& shapes=doc_.shapes();
 for(qsizetype i=0;i<shapes.size();++i){
  const auto& shape=shapes[i];if(shape.id!=selected_)continue;
  const auto where=shape.twoPoint()?QString("from %1, %2 to %3, %4").arg(shape.from.x()).arg(shape.from.y()).arg(shape.to.x()).arg(shape.to.y())
   :shape.kind==Shape::Kind::Text?QString("\"%1\" at x %2, y %3, %4 px").arg(shape.text).arg(shape.rect.x()).arg(shape.rect.y()).arg(shape.fontPixels)
   :shape.kind==Shape::Kind::Step?QString("number %1 centred at x %2, y %3, %4 px across").arg(shape.number).arg(stepCentre(shape.rect).x()).arg(stepCentre(shape.rect).y()).arg(shape.rect.width()):box(shape.box());
  return QString("%1 %2 of %3 selected, %4. Arrow keys move it, Shift+arrow keys resize it, Delete removes it, [ and ] cycle marks, Tab leaves the canvas for properties, Escape deselects.").arg(kindLabel(shape.kind)).arg(i+1).arg(shapes.size()).arg(where)+panHelp;
 }
 QString text=shapes.isEmpty()?QString("No marks."):shapes.size()==1?QString("1 mark, none selected."):QString("%1 marks, none selected.").arg(shapes.size());
 if(drawingTool()||tool_==Tool::Text)text+=QString(" Insert places a %1 in the middle.").arg(kindLabel(kindFor(tool_)).toLower());
 if(tool_==Tool::Step)text+=QString(" The next step is number %1.").arg(nextStep_);
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

Shape EditorCanvas::pendingText() const {
 const auto* existing=doc_.find(textShape_);Shape shape=existing?*existing:Shape{};
 if(!existing){shape.kind=Shape::Kind::Text;shape.color=style_.color;shape.fontPixels=style_.fontPixels;shape.fontFamily=style_.fontFamily;shape.width=style_.width;}
 shape.text=textEditor_?textEditor_->text().trimmed():QString{};
 const QFontMetrics metrics(textFont(shape.fontFamily,shape.fontPixels));
 shape.rect=QRect(textAnchor_.toPoint(),QSize(metrics.horizontalAdvance(shape.text)+1,metrics.height()));return shape;
}
bool EditorCanvas::textOutsideImage() const {
 const QRectF bounds(tool_==Tool::Crop?cropDraft_:doc_.crop());
 auto outside=[&](const Shape& shape){const auto ink=textOutputBounds(shape);return !ink.isEmpty()&&!bounds.contains(ink);};
 if(textEditor_&&outside(pendingText()))return true;
 for(const auto& shape:doc_.shapes())if(shape.kind==Shape::Kind::Text&&!(textEditor_&&shape.id==textShape_)&&outside(shape))return true;
 return false;
}
bool EditorCanvas::selectAffectedText(){
 if(textEditor_)finishText(true);
 const QRectF bounds(tool_==Tool::Crop?cropDraft_:doc_.crop());
 QList<quint64> affected;
 for(const auto& shape:doc_.shapes())if(shape.kind==Shape::Kind::Text){const auto ink=textOutputBounds(shape);if(!ink.isEmpty()&&!bounds.contains(ink))affected.append(shape.id);}
 if(affected.isEmpty())return false;
 const auto id=affected[(affected.indexOf(selected_)+1)%affected.size()];
 // Identifying a clipped mark must not apply or discard the crop being reviewed.
 if(tool_!=Tool::Crop){setTool(Tool::Select);emit toolRequested(Tool::Select);}
 selected_=id;emit selectionChanged();setFocus(Qt::OtherFocusReason);update();return true;
}
void EditorCanvas::placeTextWarning(){
 const bool outside=textOutsideImage();textWarning_->setVisible(outside);if(!outside)return;
 auto* banner=static_cast<Ui::Banner*>(textWarning_);
 const QString explanation=tool_==Tool::Crop?"Some text will be cut off by this crop":"Some text will be cut off in the saved image";
 const int inset=Ui::space(Ui::Space::l),w=std::max(0,width()-2*inset);
 banner->setText(explanation);
 const auto margins=banner->layout()->contentsMargins();
 const int natural=banner->textLabel()->fontMetrics().horizontalAdvance(explanation)+banner->actionButton()->sizeHint().width()+Ui::space(Ui::Space::s)+margins.left()+margins.right();
 if(natural>w)banner->setText("Text will be cut off");
 banner->setAccessibleDescription(explanation+". Select affected text to move or edit it.");
 banner->resize(w,banner->height());banner->layout()->activate();
 banner->setGeometry(inset,inset,w,std::max(banner->minimumSizeHint().height(),banner->heightForWidth(w)));banner->raise();
}
void EditorCanvas::placeTextEditor(){
 if(!textEditor_)return;
 // Moving the field to keep it readable never moves the image-space anchor.
 const auto bounds=rect().adjusted(4,4,-4,-4);if(bounds.isEmpty())return;
 const auto at=toWidget(textAnchor_);const QFontMetrics metrics(textEditor_->font());
 const int w=std::min(bounds.width(),std::max(220,metrics.horizontalAdvance(textEditor_->text())+48));
 const int h=std::min(bounds.height(),metrics.height()+12);
 const int below=int(std::ceil(at.y()))+12;
 const int y=below+h<=bounds.bottom()?below:int(at.y())-h-12;
 QRect field(std::clamp(int(at.x())-4,bounds.left(),bounds.right()-w+1),std::clamp(y,bounds.top(),bounds.bottom()-h+1),w,h);
 placeTextWarning();
 if(textWarning_->isVisible()&&field.intersects(textWarning_->geometry()))field.moveTop(std::min(bounds.bottom()-h+1,textWarning_->geometry().bottom()+8));
 textEditor_->setGeometry(field);
 textEditor_->setAccessibleDescription(QString("Image anchor: x %1, y %2 px.%3").arg(int(textAnchor_.x())).arg(int(textAnchor_.y())).arg(textOutsideImage()?" Some annotation text will be clipped in the saved image.":""));
 update();
}

void EditorCanvas::beginText(const QPointF& image,const Shape* existing){
 textAnchor_=QPointF(std::floor(image.x()),std::floor(image.y()));textShape_=existing?existing->id:0;
 const int pixels=existing?existing->fontPixels:style_.fontPixels;const auto family=existing?existing->fontFamily:style_.fontFamily;
 auto* editor=new QLineEdit(this);editor->setObjectName("annotationText");editor->setMaxLength(EditDocument::maximumTextCharacters);
 editor->setPlaceholderText("Type, then press Enter");editor->setAccessibleName("Annotation text");
 QFont font=textFont(family,std::clamp(int(std::lround(pixels*scale_)),12,96));editor->setFont(font);
 if(existing)editor->setText(existing->text);
 connect(editor,&QLineEdit::returnPressed,this,[this]{finishText(true);});
 connect(editor,&QLineEdit::textChanged,this,[this]{placeTextEditor();});
 editor->installEventFilter(this);textEditor_=editor;placeTextEditor();editor->show();editor->setFocus();
 selected_=textShape_;emit selectionChanged();update();
}

void EditorCanvas::finishText(bool commit){
 if(!textEditor_)return;
 const auto pending=pendingText();const auto text=pending.text;auto* editor=textEditor_.data();textEditor_=nullptr;editor->hide();editor->deleteLater();
 const auto* existing=doc_.find(textShape_);
 if(commit){
  const auto shape=pending;
  if(text.isEmpty()){if(existing&&doc_.remove(existing->id))emit changed();selected_=0;}
  else if(existing){if(doc_.replace(shape))emit changed();}
  else{selected_=doc_.add(shape);emit changed();}
 }
 textShape_=0;emit selectionChanged();setFocus();update();
}
// Copy text: selection works like text in a document. A press on a word starts
// it and a drag extends it in reading order; Shift+click extends from the anchor.
bool EditorCanvas::textPress(QMouseEvent* event){
 if(!textLayer_)return false;
 const int word=textLayer_->wordAt(toImage(event->position()));
 if(word<0){if(textLayer_->hasSelection()&&!(event->modifiers()&Qt::ShiftModifier)){textLayer_->clearSelection();update();emit textSelectionChanged();}return true;}
 if(event->modifiers()&Qt::ShiftModifier)textLayer_->extendTo(word);else textLayer_->select(word,word);
 selectingText_=true;update();emit textSelectionChanged();return true;
}
// Ctrl+A takes every word, Ctrl+C copies while the canvas has focus (a text
// field keeps its own), Escape clears the selection and then returns to Select.
bool EditorCanvas::textKey(QKeyEvent* event){
 if(event->matches(QKeySequence::SelectAll)){if(textLayer_){textLayer_->selectAll();update();emit textSelectionChanged();}return true;}
 if(event->matches(QKeySequence::Copy)){emit textCopyRequested();return true;}
 if(event->key()==Qt::Key_Escape){
  if(textLayer_&&textLayer_->hasSelection()){textLayer_->clearSelection();update();emit textSelectionChanged();return true;}
  setTool(Tool::Select);emit toolRequested(Tool::Select);return true;
 }
 return false;
}
}
