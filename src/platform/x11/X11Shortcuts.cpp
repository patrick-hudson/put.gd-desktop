#include "X11Shortcuts.h"
#include "X11Reply.h"
#include "X11SessionLock.h"
#include "core/Log.h"
#include "platform/NativeSession.h"
#include <QKeySequence>
#include <QPointer>
#include <QSet>
#include <X11/keysym.h>
#include <xcb/xinput.h>
#include <algorithm>
#include <cstdlib>
namespace putgd {
namespace {xcb_keysym_t symbol(Qt::Key key){const int k=int(key);if(k>=Qt::Key_F1&&k<=Qt::Key_F35)return XK_F1+(k-Qt::Key_F1);if(k>=Qt::Key_A&&k<=Qt::Key_Z)return XK_a+(k-Qt::Key_A);if(k>=32&&k<=126)return k;
 switch(key){case Qt::Key_Escape:return XK_Escape;case Qt::Key_Print:return XK_Print;case Qt::Key_Space:return XK_space;case Qt::Key_Return:return XK_Return;case Qt::Key_Enter:return XK_KP_Enter;case Qt::Key_Tab:return XK_Tab;case Qt::Key_Backspace:return XK_BackSpace;case Qt::Key_Delete:return XK_Delete;case Qt::Key_Insert:return XK_Insert;case Qt::Key_Home:return XK_Home;case Qt::Key_End:return XK_End;case Qt::Key_PageUp:return XK_Page_Up;case Qt::Key_PageDown:return XK_Page_Down;case Qt::Key_Left:return XK_Left;case Qt::Key_Right:return XK_Right;case Qt::Key_Up:return XK_Up;case Qt::Key_Down:return XK_Down;default:return XCB_NO_SYMBOL;}}
}
X11Shortcuts::X11Shortcuts(QObject* parent):QObject(parent){if(!nativeX11SessionEnvironment())return;connection_=xcb_connect(nullptr,nullptr);if(xcb_connection_has_error(connection_))return;root_=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;symbols_=xcb_key_symbols_alloc(connection_);if(!symbols_)return;notifier_=new QSocketNotifier(xcb_get_file_descriptor(connection_),QSocketNotifier::Read,this);connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});listenRaw();}
void X11Shortcuts::listenRaw(){
 // An open menu holds an active keyboard grab, and the server activates no
 // passive grab meanwhile. XI 2.1+ raw key events reach every root listener
 // whatever grab is active, so a shortcut still works with a menu open (#341).
 const auto* extension=xcb_get_extension_data(connection_,&xcb_input_id);
 if(!extension||!extension->present)return;
 const auto version=x11::reply(xcb_input_xi_query_version_reply(connection_,xcb_input_xi_query_version(connection_,2,2),nullptr));
 if(!version||version->major_version<2||(version->major_version==2&&version->minor_version<1))return;
 struct {xcb_input_event_mask_t head;uint32_t mask;} selection{{XCB_INPUT_DEVICE_ALL_MASTER,1},XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS|XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE};
 auto* error=xcb_request_check(connection_,xcb_input_xi_select_events_checked(connection_,root_,1,&selection.head));
 if(error){free(error);return;}
 xinputOpcode_=extension->major_opcode;xinput_=QString("%1.%2").arg(version->major_version).arg(version->minor_version);
 if(auto keys=x11::reply(xcb_query_keymap_reply(connection_,xcb_query_keymap(connection_),nullptr)))matcher_.sync(keys->keys);
}
QJsonObject X11Shortcuts::keyPaths()const{return {{"passive",available()},{"raw",QJsonObject{{"available",xinputOpcode_!=0},{"xinput",xinput_}}},{"lastActivation",last_}};}
void X11Shortcuts::syncMatcher(const xcb_get_modifier_mapping_reply_t* map){
 std::array<uint16_t,256> bits{};
 if(map){auto* codes=xcb_get_modifier_mapping_keycodes(map);for(int m=0;m<8;++m)for(int i=0;i<map->keycodes_per_modifier;++i)if(const auto code=codes[m*map->keycodes_per_modifier+i])bits[code]|=uint16_t(1<<m);}
 matcher_.setModifierMap(bits);matcher_.setLocks(locks_);
 QList<X11RawKeyMatcher::Binding> bindings;for(const auto& grab:grabs_)bindings.append({grab.action,grab.code,grab.modifiers});matcher_.setBindings(std::move(bindings));
}
void X11Shortcuts::deliver(const QString& action,const QString& path,uint32_t time){
 last_={{"action",action},{"path",path},{"sequence",qint64(++activations_)},{"serverTime",qint64(time)}};
 logging::info(logApp(),"Shortcut pressed",{{"action",action},{"path",path}});
 emit activated(action);
}
void X11Shortcuts::deliverRaw(const Pending& press){
 // Only the raw event arrived: another client holds the keyboard. A screen
 // locker does too, and a locked session never captures.
 QPointer<X11Shortcuts> guard(this);
 querySessionLocked(this,[guard,press](bool locked){
  if(!guard||guard->recording_)return;
  if(locked){guard->last_={{"action",press.action},{"path","raw"},{"sequence",qint64(++guard->activations_)},{"serverTime",qint64(press.time)},{"suppressed","session_locked"}};logging::info(logApp(),"Shortcut ignored while the session is locked",{{"action",press.action}});return;}
  guard->deliver(press.action,"raw",press.time);
 });
}
X11Shortcuts::~X11Shortcuts(){clear();if(symbols_)xcb_key_symbols_free(symbols_);if(connection_)xcb_disconnect(connection_);}
bool X11Shortcuts::available()const{return notifier_&&connection_&&!xcb_connection_has_error(connection_);}
uint16_t X11Shortcuts::modifier(const xcb_get_modifier_mapping_reply_t* map,xcb_keysym_t one,xcb_keysym_t two){if(!map)return 0;auto* codes=xcb_get_modifier_mapping_keycodes(map);uint16_t result=0;for(int m=0;m<8;++m)for(int i=0;i<map->keycodes_per_modifier;++i){auto code=codes[m*map->keycodes_per_modifier+i];if(!code)continue;for(int col=0;col<4;++col){auto sym=xcb_key_symbols_get_keysym(symbols_,code,col);if(sym==one||sym==two)result|=uint16_t(1<<m);}}return result;}
void X11Shortcuts::clear(){matcher_.setBindings({});if(!connection_)return;for(const auto& grab:grabs_)xcb_ungrab_key(connection_,grab.code,root_,grab.modifiers);grabs_.clear();xcb_flush(connection_);}
void X11Shortcuts::bind(const QHash<QString,QString>& bindings){
 requested_=bindings;status_={};
 if(recording_){for(auto it=bindings.begin();it!=bindings.end();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"bound",false},{"state","recording"}};emit bindingsChanged();return;}
 if(!available()){clear();for(auto it=bindings.begin();it!=bindings.end();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"bound",false},{"error","shortcut_unavailable"}};emit bindingsChanged();return;}
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
 grabs_=std::move(next);syncMatcher(mapping.get());
 xcb_flush(connection_);emit bindingsChanged();
}
void X11Shortcuts::setRecording(bool active,std::function<void(Error)> reply){
 if(active){
  recording_=true;clear();
  if(!available()){reply({"shortcut_unavailable","Global shortcuts cannot be paused in this session.",{}});return;}
  xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);
  auto* sync=xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr);
  const bool acknowledged=sync&&!xcb_connection_has_error(connection_);free(sync);
  // Discard shortcut presses queued before ungrabbing, including those read
  // during the roundtrip. Mapping events may update the saved bindings only.
  events();bind(requested_);
  reply(acknowledged?Error{}:Error{"shortcut_unavailable","The desktop did not acknowledge pausing global shortcuts.",{}});
 }else{
  if(available())events();
  recording_=false;bind(requested_);
  Error error;if(!available())error={"shortcut_unavailable","Global shortcuts could not be restored.",{}};
  else for(auto it=status_.begin();it!=status_.end();++it){const auto entry=it.value().toObject();if(!entry["requested"].toString().isEmpty()&&!entry["bound"].toBool()){error={"shortcut_restore_failed","A saved global shortcut could not be restored.",{}};break;}}
  reply(error);
 }
}
void X11Shortcuts::events(){while(auto* raw=xcb_poll_for_event(connection_)){const auto type=raw->response_type&127;if(type==XCB_KEY_PRESS&&!recording_){const auto* key=reinterpret_cast<xcb_key_press_event_t*>(raw);for(const auto& grab:grabs_)if(grab.code==key->detail&&grab.modifiers==(key->state&255)){// Release this connection's active passive-key grab before the selector
 // requests its own keyboard grab. Roundtrip to establish cross-connection
 // ordering even while the user's trigger keys remain physically held.
 xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);
 // While this grab was active the server sent this client no raw events, so
 // a release then went missing. The keymap reply is also the ordering fence.
 if(auto keys=x11::reply(xcb_query_keymap_reply(connection_,xcb_query_keymap(connection_),nullptr)))matcher_.sync(keys->keys);
 // The same press's raw event came first; it must not fire a second time.
 const auto mirror=std::find_if(pending_.begin(),pending_.end(),[&](const Pending& press){return press.code==key->detail;});if(mirror!=pending_.end())pending_.erase(mirror);
 deliver(grab.action,"passive",key->time);break;}}
 else if(type==XCB_GE_GENERIC&&xinputOpcode_){
  const auto* generic=reinterpret_cast<xcb_ge_generic_event_t*>(raw);
  if(generic->extension==xinputOpcode_&&(generic->event_type==XCB_INPUT_RAW_KEY_PRESS||generic->event_type==XCB_INPUT_RAW_KEY_RELEASE)){
   const auto* key=reinterpret_cast<xcb_input_raw_key_press_event_t*>(raw);const auto code=uint8_t(key->detail);
   if(generic->event_type==XCB_INPUT_RAW_KEY_RELEASE)matcher_.release(code);
   else if(const auto action=matcher_.press(code);action&&!recording_){
    // The server queues a passive grab's core press right after this raw
    // event. A roundtrip brings it into this drain, so both are matched here.
    auto* sync=xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr);free(sync);
    pending_.append({*action,code,key->time});
   }
  }
 }
 else if(type==XCB_MAPPING_NOTIFY){auto* mapping=reinterpret_cast<xcb_mapping_notify_event_t*>(raw);xcb_refresh_keyboard_mapping(symbols_,mapping);free(raw);bind(requested_);continue;}
 free(raw);}
 for(const auto& press:std::exchange(pending_,{}))deliverRaw(press);}
}
