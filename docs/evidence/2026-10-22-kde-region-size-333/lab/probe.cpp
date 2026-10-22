// #333 probe: log the pointer positions a fullscreen Wayland surface receives.
#include <QApplication>
#include <QWidget>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>
#include <cstdio>
#include <LayerShellQt/Window>
#include <QWindow>
struct W:QWidget{
 W(){setWindowFlags(Qt::FramelessWindowHint);setCursor(Qt::CrossCursor);}
 void log(const char* t,QMouseEvent* e){std::printf("%s pos=%.6f,%.6f global=%.6f,%.6f dpr=%.4f\n",t,e->position().x(),e->position().y(),e->globalPosition().x(),e->globalPosition().y(),devicePixelRatioF());std::fflush(stdout);}
 void mousePressEvent(QMouseEvent* e)override{log("press",e);}
 void mouseReleaseEvent(QMouseEvent* e)override{log("release",e);}
 void paintEvent(QPaintEvent*)override{}
};
int main(int c,char** v){QApplication a(c,v);W w;w.setStyleSheet("background:#204060");if(qEnvironmentVariableIsSet("PROBE_LAYER")){w.setAttribute(Qt::WA_TranslucentBackground);w.resize(QGuiApplication::primaryScreen()->geometry().size());w.winId();auto* l=LayerShellQt::Window::get(w.windowHandle());l->setScope("probe333");l->setLayer(LayerShellQt::Window::LayerOverlay);l->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)|LayerShellQt::Window::AnchorBottom|LayerShellQt::Window::AnchorLeft|LayerShellQt::Window::AnchorRight);l->setExclusiveZone(-1);l->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);w.show();}else w.showFullScreen();
 auto* s=QGuiApplication::primaryScreen();std::printf("screen %s geom=%d,%d %dx%d dpr=%.4f platform=%s\n",qPrintable(s->name()),s->geometry().x(),s->geometry().y(),s->geometry().width(),s->geometry().height(),s->devicePixelRatio(),qPrintable(a.platformName()));std::fflush(stdout);
 QTimer::singleShot(120000,&a,&QApplication::quit);return a.exec();}
