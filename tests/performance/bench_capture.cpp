// Independent physical X11 hotkey/paint witness. Never saves desktop pixels.
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <vector>

namespace {
class Canvas:public QWidget {
public:
 explicit Canvas(QScreen* screen):QWidget(nullptr,Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::X11BypassWindowManagerHint){setScreen(screen);setGeometry(screen->geometry());show();raise();}
 void paintEvent(QPaintEvent*)override{QPainter painter(this);painter.fillRect(rect(),QColor(80,90,100));painter.setPen(Qt::white);auto font=painter.font();font.setPixelSize(22);painter.setFont(font);painter.drawText(QRect(40,45,width()-80,100),Qt::AlignLeft|Qt::TextWordWrap,"Capture performance test\nSynthetic background — your desktop is not saved. Selections cancel automatically.");}
};
xcb_atom_t atom(xcb_connection_t* c,const char* name){auto* r=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,strlen(name),name),nullptr);if(!r)throw std::runtime_error("X11 atom lookup failed");const auto a=r->atom;free(r);return a;}
bool color(xcb_connection_t* c,xcb_window_t w,bool shaded){
 auto* r=xcb_get_image_reply(c,xcb_get_image(c,XCB_IMAGE_FORMAT_Z_PIXMAP,w,20,20,1,1,~0u),nullptr);
 if(!r)return false;
 const auto* p=xcb_get_image_data(r);const bool ok=xcb_get_image_data_length(r)>=4 &&
  (shaded?(p[2]>=50&&p[2]<=53&&p[1]>=57&&p[1]<=60&&p[0]>=63&&p[0]<=66):(p[2]==80&&p[1]==90&&p[0]==100));free(r);return ok;
 }
bool visibleCanvas(xcb_connection_t* c,xcb_window_t root,xcb_window_t canvas){
 auto* location=xcb_translate_coordinates_reply(c,xcb_translate_coordinates(c,canvas,root,20,20),nullptr);
 if(!location)return false;
 auto* pixels=xcb_get_image_reply(c,xcb_get_image(c,XCB_IMAGE_FORMAT_Z_PIXMAP,root,location->dst_x,location->dst_y,1,1,~0u),nullptr);free(location);
 if(!pixels)return false;
 const auto* p=xcb_get_image_data(pixels);const bool ok=xcb_get_image_data_length(pixels)>=4&&p[2]==80&&p[1]==90&&p[0]==100;free(pixels);return ok;
}
}
int main(int argc,char** argv){
 QApplication application(argc,argv);
 QCommandLineParser parser;parser.addHelpOption();parser.addOption({"resident-pid","Owned release resident PID","pid"});parser.addOption({"output","Synthetic result JSON","path"});parser.process(application);
 const auto pid=parser.value("resident-pid").toUInt();const auto output=parser.value("output");
 if(!pid||output.isEmpty()||QGuiApplication::platformName()!="xcb"||qEnvironmentVariable("XDG_SESSION_TYPE")!="x11"||!qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("Cinnamon"))return 2;
 xcb_connection_t* connection=xcb_connect(nullptr,nullptr);if(xcb_connection_has_error(connection))return 2;
 auto* symbols=xcb_key_symbols_alloc(connection);
 std::vector<std::unique_ptr<Canvas>> canvases;QJsonArray displays;
 // Keep every enabled display covered before the production resident acquires it.
 for(auto* screen:QGuiApplication::screens()){
  canvases.push_back(std::make_unique<Canvas>(screen));const auto r=screen->geometry();
  displays.append(QJsonObject{{"name",screen->name()},{"x",r.x()},{"y",r.y()},{"width",r.width()},{"height",r.height()},{"scale",screen->devicePixelRatio()}});
 }
 auto code=[&](uint32_t key){auto* codes=xcb_key_symbols_get_keycode(symbols,key);const auto c=codes?codes[0]:0;free(codes);if(!c)throw std::runtime_error("Missing native keycode");return c;};
 auto input=[&](uint32_t key,bool down){xcb_test_fake_input(connection,down?XCB_KEY_PRESS:XCB_KEY_RELEASE,code(key),XCB_CURRENT_TIME,0,0,0,0);};
 auto escape=[&]{input(0xff1b,true);input(0xff1b,false);xcb_flush(connection);};
 auto settle=[&](int ms){QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents();QThread::msleep(2);}};
 const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
 const uint32_t mask=XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;xcb_change_window_attributes(connection,root,XCB_CW_EVENT_MASK,&mask);xcb_flush(connection);
 const auto pidAtom=atom(connection,"_NET_WM_PID");QJsonArray samples;QString error;
 try{
  settle(500);
  for(const auto& canvas:canvases)if(!color(connection,canvas->winId(),false))throw std::runtime_error("Synthetic covering window has not painted");
  for(int iteration=0;iteration<35;++iteration){
   QElapsedTimer ready;ready.start();
   while(!std::all_of(canvases.cbegin(),canvases.cend(),[&](const auto& canvas){return visibleCanvas(connection,root,canvas->winId());})){
    if(ready.elapsed()>2000)throw std::runtime_error("Synthetic covering windows are not visibly ready on every display");
    settle(5);
   }
   while(auto* event=xcb_poll_for_event(connection))free(event);
   QElapsedTimer timer;timer.start();input(0xffe3,true);input(0xffe1,true);input(0xffc9,true);input(0xffc9,false);input(0xffe1,false);input(0xffe3,false);xcb_flush(connection);
   QList<xcb_window_t> overlays;double mappedMs=-1,paintedMs=-1;
   while(timer.elapsed()<5000){
    while(auto* event=xcb_poll_for_event(connection)){
     if((event->response_type&0x7f)==XCB_MAP_NOTIFY){
      const auto window=reinterpret_cast<xcb_map_notify_event_t*>(event)->window;
      auto* property=xcb_get_property_reply(connection,xcb_get_property(connection,0,window,pidAtom,XCB_ATOM_CARDINAL,0,1),nullptr);
      uint32_t owner=0;if(property&&property->format==32&&xcb_get_property_value_length(property)==4)memcpy(&owner,xcb_get_property_value(property),4);free(property);
      auto* attributes=xcb_get_window_attributes_reply(connection,xcb_get_window_attributes(connection,window),nullptr);
      const bool own=owner==pid&&attributes&&attributes->override_redirect;free(attributes);
      if(own&&!overlays.contains(window))overlays.append(window);
     }
     free(event);
    }
    if(size_t(overlays.size())==canvases.size()){
     if(mappedMs<0)mappedMs=timer.nsecsElapsed()/1000000.0;
     if(std::all_of(overlays.cbegin(),overlays.cend(),[&](auto w){return color(connection,w,true);})){paintedMs=timer.nsecsElapsed()/1000000.0;break;}
    }
    QCoreApplication::processEvents();QThread::msleep(1);
   }
   escape();settle(100);
   if(paintedMs<0)throw std::runtime_error("Actual resident selector did not map/paint on every display");
   if(iteration>=5)samples.append(QJsonObject{{"mapMs",mappedMs},{"paintMs",paintedMs}});
  }
 }catch(const std::exception& e){error=QString::fromUtf8(e.what());escape();}
 std::vector<double> sorted;for(const auto& sample:samples)sorted.push_back(sample.toObject()["paintMs"].toDouble());std::sort(sorted.begin(),sorted.end());
 const double p95=sorted.size()==30?sorted[28]:-1;
 QJsonObject result{{"schemaVersion",1},{"scope","physical Cinnamon X11 release resident, actual hotkey to all-display mapped/painted selector; synthetic covering windows; selection cancelled"},{"warmupInvocations",5},{"samples",samples},{"displays",displays},{"p95PaintMs",p95},{"budgetPassed",error.isEmpty()&&p95>=0&&p95<=150},{"error",error}};
 QSaveFile file(output);const auto bytes=QJsonDocument(result).toJson();const bool saved=file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit();
 xcb_key_symbols_free(symbols);xcb_disconnect(connection);return saved&&result["budgetPassed"].toBool()?0:1;
}
