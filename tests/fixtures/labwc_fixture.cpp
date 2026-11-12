// A known-colour Wayland client for the headless labwc tests (#1275). Each
// output gets a background layer surface in its own colour with a white square
// at its logical top-left and a black one at its top-right, so a capture shows
// both its pixels and its orientation. One toplevel window is solid green.
// Prints "ready" once every surface has drawn, "press N" (Qt's button
// number) for each button press the window receives, and "wheel D" (Qt's
// vertical angle delta, 120 a notch, negative when scrolling down) for each
// wheel event.
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <LayerShellQt/Window>
#include <algorithm>
#include <cstdio>

namespace {
const QList<QColor> outputColours{QColor(200,40,40),QColor(40,40,200),QColor(200,200,40),QColor(40,200,200)};
class Surface final:public QWidget {
public:
 Surface(QColor fill,bool marks):QWidget(nullptr,Qt::FramelessWindowHint),fill_(fill),marks_(marks){setAttribute(Qt::WA_OpaquePaintEvent);}
 int painted=0;
 // Drops the native window so the next show() makes a new layer surface.
 void reset(){hide();destroy();}
protected:
 void paintEvent(QPaintEvent*) override{
  QPainter painter(this);painter.fillRect(rect(),fill_);
  if(marks_){painter.fillRect(QRect(0,0,64,64),Qt::white);painter.fillRect(QRect(width()-32,0,32,32),Qt::black);}
  ++painted;
 }
 void mousePressEvent(QMouseEvent* event) override{if(!marks_){std::printf("press %d\n",int(event->button()));std::fflush(stdout);}}
 void wheelEvent(QWheelEvent* event) override{if(!marks_){std::printf("wheel %d\n",event->angleDelta().y());std::fflush(stdout);}}
private:
 QColor fill_;bool marks_;
};
}
int main(int argc,char** argv){
 qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION","1");
 QApplication app(argc,argv);app.setDesktopFileName("putgd-labwc-fixture");
 QList<Surface*> surfaces;
 // Colour by output name order, as the test reads outputs: HEADLESS-1 first.
 auto screens=QGuiApplication::screens();std::sort(screens.begin(),screens.end(),[](QScreen* a,QScreen* b){return a->name()<b->name();});
 int index=0;
 // The production selector's order: screen, geometry, native window, then the layer role.
 const auto place=[](Surface* surface,QScreen* screen){
  surface->setScreen(screen);surface->setGeometry(screen->geometry());surface->winId();
  if(auto* layer=LayerShellQt::Window::get(surface->windowHandle())){
   layer->setScope(QStringLiteral("putgd-fixture-background"));
   layer->setLayer(LayerShellQt::Window::LayerBackground);
   layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop|LayerShellQt::Window::AnchorBottom|LayerShellQt::Window::AnchorLeft|LayerShellQt::Window::AnchorRight));
   layer->setExclusiveZone(-1);layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
  }
  surface->show();
 };
 for(auto* screen:screens){
  auto* surface=new Surface(outputColours[index++%outputColours.size()],true);
  place(surface,screen);surfaces.append(surface);
  // A turned or rescaled output gets a fresh surface at its new size.
  QObject::connect(screen,&QScreen::geometryChanged,surface,[surface,screen,place]{surface->reset();place(surface,screen);});
 }
 auto* window=new Surface(QColor(0,160,0),false);window->setWindowTitle("fixture-window");window->resize(240,160);window->show();surfaces.append(window);
 QTimer poll;poll.setInterval(50);bool announced=false;
 QObject::connect(&poll,&QTimer::timeout,&app,[&]{
  if(announced)return;
  for(auto* surface:surfaces)if(!surface->painted)return;
  announced=true;std::printf("ready\n");std::fflush(stdout);
 });
 poll.start();
 return app.exec();
}
