#include "X11Shortcuts.h"
#include "X11Reply.h"
#include <QGuiApplication>
#include <QKeySequence>
#include <QSet>
#include <X11/keysym.h>
#include <cstdlib>
namespace putgd {
namespace {xcb_keysym_t symbol(Qt::Key key){const int k=int(key);if(k>=Qt::Key_F1&&k<=Qt::Key_F35)return XK_F1+(k-Qt::Key_F1);if(k>=Qt::Key_A&&k<=Qt::Key_Z)return XK_a+(k-Qt::Key_A);if(k>=32&&k<=126)return k;
 switch(key){case Qt::Key_Escape:return XK_Escape;case Qt::Key_Print:return XK_Print;case Qt::Key_Space:return XK_space;case Qt::Key_Return:return XK_Return;case Qt::Key_Enter:return XK_KP_Enter;case Qt::Key_Tab:return XK_Tab;case Qt::Key_Backspace:return XK_BackSpace;case Qt::Key_Delete:return XK_Delete;case Qt::Key_Insert:return XK_Insert;case Qt::Key_Home:return XK_Home;case Qt::Key_End:return XK_End;case Qt::Key_PageUp:return XK_Page_Up;case Qt::Key_PageDown:return XK_Page_Down;case Qt::Key_Left:return XK_Left;case Qt::Key_Right:return XK_Right;case Qt::Key_Up:return XK_Up;case Qt::Key_Down:return XK_Down;default:return XCB_NO_SYMBOL;}}
}
X11Shortcuts::X11Shortcuts(QObject* parent):QObject(parent){if(QGuiApplication::platformName()!="xcb")return;connection_=xcb_connect(nullptr,nullptr);if(xcb_connection_has_error(connection_))return;root_=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;symbols_=xcb_key_symbols_alloc(connection_);if(!symbols_)return;notifier_=new QSocketNotifier(xcb_get_file_descriptor(connection_),QSocketNotifier::Read,this);connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});}
X11Shortcuts::~X11Shortcuts(){clear();if(symbols_)xcb_key_symbols_free(symbols_);if(connection_)xcb_disconnect(connection_);}
bool X11Shortcuts::available()const{return notifier_&&connection_&&!xcb_connection_has_error(connection_);}
uint16_t X11Shortcuts::modifier(const xcb_get_modifier_mapping_reply_t* map,xcb_keysym_t one,xcb_keysym_t two){if(!map)return 0;auto* codes=xcb_get_modifier_mapping_keycodes(map);uint16_t result=0;for(int m=0;m<8;++m)for(int i=0;i<map->keycodes_per_modifier;++i){auto code=codes[m*map->keycodes_per_modifier+i];if(!code)continue;for(int col=0;col<4;++col){auto sym=xcb_key_symbols_get_keysym(symbols_,code,col);if(sym==one||sym==two)result|=uint16_t(1<<m);}}return result;}
void X11Shortcuts::clear(){if(!connection_)return;for(const auto& grab:grabs_)xcb_ungrab_key(connection_,grab.code,root_,grab.modifiers);grabs_.clear();xcb_flush(connection_);}
void X11Shortcuts::bind(const QHash<QString,QString>& bindings){
 requested_=bindings;status_={};if(!available()){clear();for(auto it=bindings.begin();it!=bindings.end();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"bound",false},{"error","shortcut_unavailable"}};emit bindingsChanged();return;}
 const auto identity=[](xcb_keycode_t code,uint16_t modifiers){return QString::number(code)+":"+QString::number(modifiers);};
 QSet<QString> existing;for(const auto& grab:grabs_)existing.insert(identity(grab.code,grab.modifiers));
 QList<Grab> next;
 const auto mapping=x11::reply(xcb_get_modifier_mapping_reply(connection_,xcb_get_modifier_mapping(connection_),nullptr));
 const auto mask=[&](xcb_keysym_t one,xcb_keysym_t two){return modifier(mapping.get(),one,two);};
 const auto control=mask(XK_Control_L,XK_Control_R),shift=mask(XK_Shift_L,XK_Shift_R),alt=mask(XK_Alt_L,XK_Alt_R),meta=mask(XK_Super_L,XK_Super_R);
 locks_=mask(XK_Caps_Lock,XK_Shift_Lock)|mask(XK_Num_Lock,XK_Num_Lock)|mask(XK_Scroll_Lock,XK_Scroll_Lock);
 QList<uint16_t> ignored{0};for(int bit=0;bit<8;++bit)if(locks_&(1<<bit)){auto old=ignored;for(auto value:old)ignored.append(uint16_t(value|(1<<bit)));}
 QStringList actions=bindings.keys();actions.sort();QSet<QString> accepted;
 for(const auto& action:actions){const QString text=bindings.value(action);QJsonObject entry{{"requested",text},{"bound",false}};if(text.isEmpty()){entry["error"]="disabled";status_[action]=entry;continue;}
  const auto sequence=QKeySequence::fromString(text,QKeySequence::PortableText);const auto combination=sequence[0];const auto key=symbol(combination.key());uint16_t modifiers=0;const auto qt=combination.keyboardModifiers();
  if(((qt&Qt::ControlModifier)&&!control)||((qt&Qt::ShiftModifier)&&!shift)||((qt&Qt::AltModifier)&&!alt)||((qt&Qt::MetaModifier)&&!meta)){entry["error"]="shortcut_unmapped";status_[action]=entry;continue;}
  if(qt&Qt::ControlModifier)modifiers|=control;
  if(qt&Qt::ShiftModifier)modifiers|=shift;
  if(qt&Qt::AltModifier)modifiers|=alt;
  if(qt&Qt::MetaModifier)modifiers|=meta;
  if(sequence.count()!=1||!key||qt.testFlag(Qt::KeypadModifier)||qt.testFlag(Qt::GroupSwitchModifier)){entry["error"]="shortcut_unsupported";status_[action]=entry;continue;}
  auto* codes=xcb_key_symbols_get_keycode(symbols_,key);QList<Grab> attempt,installed;bool conflict=false;QSet<QString> attemptIds;
  bool unsupported=false;
  if(codes)for(auto* code=codes;*code;++code){
   const auto base=xcb_key_symbols_get_keysym(symbols_,*code,0),shifted=xcb_key_symbols_get_keysym(symbols_,*code,1);
   if(base!=key&&shifted!=key){unsupported=true;continue;}
   uint16_t effective=modifiers;
   if(base!=key){if(!shift){unsupported=true;continue;}effective|=shift;}
   for(auto lock:ignored){
    const auto mask=uint16_t(effective|lock);const QString id=identity(*code,mask);
    if(attemptIds.contains(id))continue;
    attemptIds.insert(id);
    if(accepted.contains(id)){conflict=true;continue;}
    // MappingNotify often leaves the native chord unchanged. Keep that grab
    // installed while querying the map and adding any new combinations.
    if(existing.contains(id)){attempt.append({action,*code,mask});continue;}
    auto* error=xcb_request_check(connection_,xcb_grab_key_checked(connection_,0,root_,mask,*code,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC));
    if(error){conflict=true;free(error);}else{const Grab grab{action,*code,mask};attempt.append(grab);installed.append(grab);}
   }
  }
  free(codes);if(conflict||attempt.isEmpty()){for(const auto& grab:installed)xcb_ungrab_key(connection_,grab.code,root_,grab.modifiers);entry["error"]=conflict?"shortcut_conflict":unsupported?"shortcut_unsupported":"shortcut_unmapped";}else{next.append(attempt);accepted.unite(attemptIds);entry["bound"]=true;entry["actual"]=sequence.toString(QKeySequence::PortableText);}status_[action]=entry;
 }
 // Remove obsolete grabs only after every new action has been reconciled.
 // Failed actions release their retained combinations here, unless another
 // accepted action now owns them.
 for(const auto& grab:grabs_)if(!accepted.contains(identity(grab.code,grab.modifiers)))xcb_ungrab_key(connection_,grab.code,root_,grab.modifiers);
 grabs_=std::move(next);
 xcb_flush(connection_);emit bindingsChanged();
}
void X11Shortcuts::events(){while(auto* raw=xcb_poll_for_event(connection_)){const auto type=raw->response_type&127;if(type==XCB_KEY_PRESS){const auto* key=reinterpret_cast<xcb_key_press_event_t*>(raw);for(const auto& grab:grabs_)if(grab.code==key->detail&&grab.modifiers==(key->state&255)){// Release this connection's active passive-key grab before the selector
 // requests its own keyboard grab. Roundtrip to establish cross-connection
 // ordering even while the user's trigger keys remain physically held.
 xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);
 auto* sync=xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr);free(sync);
 emit activated(grab.action);break;}}
 else if(type==XCB_MAPPING_NOTIFY){auto* mapping=reinterpret_cast<xcb_mapping_notify_event_t*>(raw);xcb_refresh_keyboard_mapping(symbols_,mapping);free(raw);bind(requested_);continue;}
 free(raw);}}
}
