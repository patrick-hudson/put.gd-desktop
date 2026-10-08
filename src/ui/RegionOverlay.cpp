#include "RegionOverlay.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
namespace putgd {
class RegionOverlay::Surface final:public QWidget {
public:
 Surface(RegionOverlay& owner,const CaptureFrame& frame):QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::X11BypassWindowManagerHint|Qt::WindowDoesNotAcceptFocus),owner_(owner),frame_(frame){setObjectName("putgd-region-overlay");for(auto* screen:QGuiApplication::screens())if(screen->name()==frame.identity){setScreen(screen);break;}setGeometry(frame.logicalRect.toAlignedRect());setCursor(Qt::CrossCursor);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);}
protected:
 void paintEvent(QPaintEvent*) override {QPainter painter(this);painter.drawImage(rect(),frame_.pixels);painter.fillRect(rect(),QColor(0,0,0,90));const auto clipped=owner_.selection_.intersected(frame_.logicalRect);if(!clipped.isEmpty()){const QRectF local=clipped.translated(-frame_.logicalRect.topLeft());const QRectF source((clipped.x()-frame_.logicalRect.x())*frame_.density,(clipped.y()-frame_.logicalRect.y())*frame_.density,clipped.width()*frame_.density,clipped.height()*frame_.density);painter.drawImage(local,frame_.pixels,source);painter.setPen(QPen(Qt::white,1));painter.drawRect(local.adjusted(0,0,-1,-1));}}
 void mousePressEvent(QMouseEvent* e) override {if(e->button()==Qt::RightButton){owner_.cancel();return;}if(e->button()==Qt::LeftButton){owner_.press(e->globalPosition());grabMouse();}}
 void mouseMoveEvent(QMouseEvent* e) override {owner_.move(e->globalPosition());}
 void mouseReleaseEvent(QMouseEvent* e) override {if(e->button()==Qt::LeftButton){releaseMouse();owner_.release(e->globalPosition());}}
 void keyPressEvent(QKeyEvent* e) override {if(e->key()==Qt::Key_Escape)owner_.cancel();}
private:
 RegionOverlay& owner_;
 CaptureFrame frame_;
};
RegionOverlay::RegionOverlay(QList<CaptureFrame> frames,QObject* parent):QObject(parent),frames_(std::move(frames)){}
RegionOverlay::~RegionOverlay(){finish();for(auto* s:surfaces_)delete s;}
void RegionOverlay::start(){
 for(const auto& frame:frames_){auto* surface=new Surface(*this,frame);surfaces_.append(surface);surface->show();surface->raise();}
 Surface* focus=surfaces_.isEmpty()?nullptr:surfaces_.first();for(auto* s:surfaces_)if(s->geometry().contains(QCursor::pos()))focus=s;
 if(focus)focus->grabKeyboard();
 connect(qGuiApp,&QGuiApplication::screenAdded,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});connect(qGuiApp,&QGuiApplication::screenRemoved,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});
 for(auto* screen:QGuiApplication::screens()) {connect(screen,&QScreen::geometryChanged,this,[this]{cancel({"display_changed","Display geometry changed during region selection",{}});});connect(screen,&QScreen::logicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display scale changed during region selection",{}});});connect(screen,&QScreen::orientationChanged,this,[this]{cancel({"display_changed","Display rotation changed during region selection",{}});});}
}
void RegionOverlay::press(QPointF point){if(finished_)return;anchor_=point;dragging_=true;selection_=QRectF(point,QSizeF{});}
void RegionOverlay::move(QPointF point){if(!dragging_||finished_)return;selection_=QRectF(anchor_,point).normalized();for(auto* s:surfaces_)s->update();}
void RegionOverlay::release(QPointF point){if(!dragging_||finished_)return;move(point);if(selection_.width()<1||selection_.height()<1){cancel();return;}finish();emit selected(selection_);}
void RegionOverlay::finish(){finished_=true;for(auto* s:surfaces_){s->releaseKeyboard();s->releaseMouse();s->hide();}}
void RegionOverlay::cancel(Error error){if(finished_)return;finish();emit cancelled(error);}
}
