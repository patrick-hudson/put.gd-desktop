#pragma once
#include <QObject>
#include <QSocketNotifier>
#include <QHash>
#include <QByteArray>
#include <xcb/xcb.h>
#include <cstdlib>
#include <cstring>
// Native synthetic owner. It can deliberately send ICCCM INCR transfers, and
// never accesses the user's clipboard: callers run only inside disposable Xvfb.
class X11SelectionFixture:public QObject {
public:
 X11SelectionFixture(QHash<QByteArray,QByteArray> formats,bool incr=false):formats_(std::move(formats)),incremental_(incr) {
  connection_=xcb_connect(nullptr,nullptr);root_=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;window_=xcb_generate_id(connection_);xcb_create_window(connection_,XCB_COPY_FROM_PARENT,window_,root_,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,0,nullptr);clipboard_=atom("CLIPBOARD");targets_=atom("TARGETS");incr_=atom("INCR");xcb_set_selection_owner(connection_,window_,clipboard_,XCB_CURRENT_TIME);xcb_flush(connection_);auto* ownership=xcb_get_selection_owner_reply(connection_,xcb_get_selection_owner(connection_,clipboard_),nullptr);free(ownership);notifier_=new QSocketNotifier(xcb_get_file_descriptor(connection_),QSocketNotifier::Read,this);connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});
 }
 ~X11SelectionFixture(){delete notifier_;xcb_disconnect(connection_);}
 int incrementalTransfers=0;
private:
 xcb_atom_t atom(const QByteArray& name){auto* r=xcb_intern_atom_reply(connection_,xcb_intern_atom(connection_,0,name.size(),name.constData()),nullptr);auto a=r?r->atom:0;free(r);return a;}
 void events(){while(auto* raw=xcb_poll_for_event(connection_)) {auto type=raw->response_type&127;
  if(type==XCB_SELECTION_REQUEST){auto* r=reinterpret_cast<xcb_selection_request_event_t*>(raw);const auto property=r->property?r->property:r->target;bool accepted=false;
   if(r->target==targets_){QList<xcb_atom_t> atoms;for(auto it=formats_.begin();it!=formats_.end();++it)atoms.append(atom(it.key()));xcb_change_property(connection_,XCB_PROP_MODE_REPLACE,r->requestor,property,XCB_ATOM_ATOM,32,atoms.size(),atoms.constData());accepted=true;}
   else for(auto it=formats_.begin();it!=formats_.end();++it)if(r->target==atom(it.key())){accepted=true;if(incremental_){requestor_=r->requestor;property_=property;target_=r->target;pending_=it.value();offset_=0;uint32_t mask=XCB_EVENT_MASK_PROPERTY_CHANGE;xcb_change_window_attributes(connection_,requestor_,XCB_CW_EVENT_MASK,&mask);uint32_t bytes=pending_.size();xcb_change_property(connection_,XCB_PROP_MODE_REPLACE,requestor_,property_,incr_,32,1,&bytes);++incrementalTransfers;}else xcb_change_property(connection_,XCB_PROP_MODE_REPLACE,r->requestor,property,r->target,8,it.value().size(),it.value().constData());break;}
   xcb_selection_notify_event_t notify{};notify.response_type=XCB_SELECTION_NOTIFY;notify.time=r->time;notify.requestor=r->requestor;notify.selection=r->selection;notify.target=r->target;notify.property=accepted?property:xcb_atom_t(XCB_NONE);xcb_send_event(connection_,0,r->requestor,XCB_EVENT_MASK_NO_EVENT,reinterpret_cast<const char*>(&notify));xcb_flush(connection_);
  }else if(type==XCB_PROPERTY_NOTIFY&&requestor_){auto* p=reinterpret_cast<xcb_property_notify_event_t*>(raw);if(p->window==requestor_&&p->atom==property_&&p->state==XCB_PROPERTY_DELETE){const auto data=pending_.mid(offset_,4096);xcb_change_property(connection_,XCB_PROP_MODE_REPLACE,requestor_,property_,target_,8,data.size(),data.constData());offset_+=data.size();if(data.isEmpty())requestor_=0;xcb_flush(connection_);}}
  free(raw);
 }}
 QHash<QByteArray,QByteArray> formats_;bool incremental_;xcb_connection_t* connection_;xcb_window_t root_,window_,requestor_=0;xcb_atom_t clipboard_,targets_,incr_,property_=0,target_=0;QSocketNotifier* notifier_;QByteArray pending_;int offset_=0;
};
