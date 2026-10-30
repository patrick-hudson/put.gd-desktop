#include "X11CompositorSettle.h"
#include "X11Reply.h"
#include <QPointer>
#include <QSocketNotifier>
#include <xcb/damage.h>
namespace putgd {
X11CompositorSettle::X11CompositorSettle(QList<QRect> nativeAreas,QObject* parent,Timing timing):QObject(parent),areas_(std::move(nativeAreas)),timing_(timing),quiet_(this),limit_(this){
 quiet_.setSingleShot(true);limit_.setSingleShot(true);
 connect(&quiet_,&QTimer::timeout,this,[this]{
  // A delayed event loop can deliver this timer before the socket notifier.
  // Consume pending Damage first; a repaint starts a fresh quiet period.
  // events() can emit settled() on a connection error, and a handler may
  // destroy this object synchronously.
  const QPointer<X11CompositorSettle> self(this);
  events();
  if(!self)return;
  if(!finished_&&!quiet_.isActive())finish();
 });
 connect(&limit_,&QTimer::timeout,this,&X11CompositorSettle::finish);
}
X11CompositorSettle::~X11CompositorSettle(){finished_=true;release();}
// Connect to settled() first: an uncomposited or unreachable display settles here.
void X11CompositorSettle::start(){
 if(started_||finished_)return;
 started_=true;
 int screen=0;connection_=xcb_connect(nullptr,&screen);
 if(xcb_connection_has_error(connection_)){finish();return;}
 auto roots=xcb_setup_roots_iterator(xcb_get_setup(connection_));
 for(int i=0;i<screen&&roots.rem;++i)xcb_screen_next(&roots);
 if(!roots.rem){finish();return;}
 const auto root=roots.data->root;
 const auto selection=x11::internAtom(connection_,("_NET_WM_CM_S"+QByteArray::number(screen)).constData());
 auto owner=x11::reply(xcb_get_selection_owner_reply(connection_,xcb_get_selection_owner(connection_,selection),nullptr));
 composited_=owner&&owner->owner!=XCB_WINDOW_NONE;
 if(!composited_){finish();return;}
 limit_.start(timing_.limitMs);
 const auto* extension=xcb_get_extension_data(connection_,&xcb_damage_id);
 if(extension&&extension->present&&x11::reply(xcb_damage_query_version_reply(connection_,xcb_damage_query_version(connection_,XCB_DAMAGE_MAJOR_VERSION,XCB_DAMAGE_MINOR_VERSION),nullptr))){
  damage_=xcb_generate_id(connection_);
  if(auto* error=xcb_request_check(connection_,xcb_damage_create_checked(connection_,damage_,root,XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES))){std::free(error);damage_=0;}
  else {
   damageEvent_=extension->first_event+XCB_DAMAGE_NOTIFY;
   notifier_=new QSocketNotifier(xcb_get_file_descriptor(connection_),QSocketNotifier::Read,this);
   connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});
  }
 }
 // Without Damage, the quiet period is a single bounded frame delay.
 quiet_.start(timing_.quietMs);
 // Replies above may already have buffered damage.
 events();
}
void X11CompositorSettle::events(){
 while(connection_&&!finished_){
  auto* raw=xcb_poll_for_event(connection_);if(!raw)break;
  if(damage_&&(raw->response_type&0x7f)==damageEvent_){
   const auto* event=reinterpret_cast<const xcb_damage_notify_event_t*>(raw);
   const QRect area(event->area.x,event->area.y,event->area.width,event->area.height);
   for(const auto& selected:areas_)if(selected.intersects(area)){++repaints_;quiet_.start(timing_.quietMs);break;}
  }
  std::free(raw);
 }
 if(connection_&&!finished_&&xcb_connection_has_error(connection_))finish();
}
void X11CompositorSettle::finish(){
 if(finished_)return;
 finished_=true;release();
 emit settled();
}
void X11CompositorSettle::release(){
 quiet_.stop();limit_.stop();
 if(notifier_){notifier_->setEnabled(false);delete notifier_;}
 // The server frees the Damage object with this client.
 if(connection_){xcb_disconnect(connection_);connection_=nullptr;damage_=0;}
}
}
