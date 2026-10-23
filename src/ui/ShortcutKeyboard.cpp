#include "ShortcutKeyboard.h"
#include <QChar>
#include <QGuiApplication>
#include <QKeyEvent>
#ifdef PUTGD_SHORTCUT_KEYBOARD
#include "platform/kde/KdeKeyboardMap.h"
#include <QtGui/qguiapplication_platform.h>
#include <xkbcommon/xkbcommon.h>
#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
#define PUTGD_WAYLAND_KEYBOARD 1
#include <wayland-client.h>
#endif
#if QT_CONFIG(xcb)
#define PUTGD_X11_KEYBOARD 1
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#endif
#endif

namespace putgd {
namespace {
// Qt's key for a symbol a Shift press can produce: ASCII and Latin-1, with
// Qt's uppercase Latin-1 rule. Anything else does not match a recorded key.
int symbolKey(uint32_t symbol) {
 if(symbol>='a'&&symbol<='z')return int(symbol-'a'+'A');
 if(symbol>=0x20&&symbol<=0x7e)return int(symbol);
 if(symbol>=0xa0&&symbol<=0xff){const auto upper=QChar(ushort(symbol)).toUpper().unicode();return upper<=0xff?int(upper):int(symbol);}
 return 0;
}
}
QKeyCombination unshiftedChord(QKeyCombination pressed,uint32_t baseSymbol,uint32_t shiftedSymbol) {
 const auto modifiers=pressed.keyboardModifiers();
 if(!modifiers.testFlag(Qt::ShiftModifier)||modifiers.testFlag(Qt::KeypadModifier))return pressed;
 // Digits and ASCII punctuation only. KDE resolves those back to the Shift
 // level of the current layout; a non-ASCII base would register literally.
 const bool letter=(baseSymbol>='a'&&baseSymbol<='z')||(baseSymbol>='A'&&baseSymbol<='Z');
 if(baseSymbol<=0x20||baseSymbol>=0x7f||letter||baseSymbol==shiftedSymbol)return pressed;
 const int shifted=symbolKey(shiftedSymbol);
 if(shifted==0||shifted!=int(pressed.key()))return pressed;
 return QKeyCombination(modifiers,static_cast<Qt::Key>(baseSymbol));
}
#ifdef PUTGD_SHORTCUT_KEYBOARD
QKeyCombination unshiftedChord(QKeyCombination pressed,xkb_keymap* map,uint32_t layout,uint32_t keycode) {
 if(!map||keycode<xkb_keymap_min_keycode(map)||keycode>xkb_keymap_max_keycode(map)||layout>=xkb_keymap_num_layouts(map))return pressed;
 const auto shift=xkb_keymap_mod_get_index(map,XKB_MOD_NAME_SHIFT);
 if(shift==XKB_MOD_INVALID||shift>=32)return pressed;
 auto* state=xkb_state_new(map);
 if(!state)return pressed;
 xkb_state_update_mask(state,0,0,0,0,0,layout);
 const auto base=xkb_state_key_get_one_sym(state,keycode);
 xkb_state_update_mask(state,xkb_mod_mask_t{1}<<shift,0,0,0,0,layout);
 const auto shifted=xkb_state_key_get_one_sym(state,keycode);
 xkb_state_unref(state);
 return unshiftedChord(pressed,base,shifted);
}
#else
QKeyCombination unshiftedChord(QKeyCombination pressed,xkb_keymap*,uint32_t,uint32_t) {return pressed;}
#endif

struct ShortcutKeyboard::Private {
#ifdef PUTGD_WAYLAND_KEYBOARD
 wl_display* display=nullptr;wl_event_queue* queue=nullptr;wl_seat* seat=nullptr;wl_keyboard* keyboard=nullptr;
 xkb_context* context=nullptr;xkb_keymap* map=nullptr;uint32_t layout=0;
 static const wl_keyboard_listener listener;
#endif
#ifdef PUTGD_X11_KEYBOARD
 xcb_key_symbols_t* symbols=nullptr;
#endif
};
#ifdef PUTGD_WAYLAND_KEYBOARD
const wl_keyboard_listener ShortcutKeyboard::Private::listener{
 [](void* data,wl_keyboard*,uint32_t format,int32_t fd,uint32_t size){
  auto* d=static_cast<Private*>(data);
  if(auto* next=readKdeKeyboardMap(d->context,format,fd,size)){if(d->map)xkb_keymap_unref(d->map);d->map=next;}
 },
 [](void*,wl_keyboard*,uint32_t,wl_surface*,wl_array*){},
 [](void*,wl_keyboard*,uint32_t,wl_surface*){},
 [](void*,wl_keyboard*,uint32_t,uint32_t,uint32_t,uint32_t){},
 [](void* data,wl_keyboard*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t layout){static_cast<Private*>(data)->layout=layout;},
 [](void*,wl_keyboard*,int32_t,int32_t){}
};
#endif

ShortcutKeyboard::ShortcutKeyboard():d_(std::make_unique<Private>()) {
 if(!qGuiApp)return;
#ifdef PUTGD_WAYLAND_KEYBOARD
 if(auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();native&&native->display()&&native->seat()) {
  // A second keyboard on a private queue leaves Qt's own keyboard untouched.
  // Qt's reader thread still reads the socket; this queue only holds our events.
  d_->display=native->display();d_->queue=wl_display_create_queue(d_->display);
  d_->seat=static_cast<wl_seat*>(wl_proxy_create_wrapper(native->seat()));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(d_->seat),d_->queue);
  d_->keyboard=wl_seat_get_keyboard(d_->seat);
  d_->context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  wl_keyboard_add_listener(d_->keyboard,&Private::listener,d_.get());
  // The compositor answers with the keymap and, while this window has focus,
  // the active layout.
  wl_display_roundtrip_queue(d_->display,d_->queue);
  return;
 }
#endif
#ifdef PUTGD_X11_KEYBOARD
 if(auto* native=qGuiApp->nativeInterface<QNativeInterface::QX11Application>();native&&native->connection())
  d_->symbols=xcb_key_symbols_alloc(native->connection());
#endif
}
ShortcutKeyboard::~ShortcutKeyboard() {
#ifdef PUTGD_WAYLAND_KEYBOARD
 if(d_->keyboard){
  if(wl_proxy_get_version(reinterpret_cast<wl_proxy*>(d_->keyboard))>=WL_KEYBOARD_RELEASE_SINCE_VERSION)wl_keyboard_release(d_->keyboard);
  else wl_keyboard_destroy(d_->keyboard);
  wl_display_flush(d_->display);
 }
 if(d_->seat)wl_proxy_wrapper_destroy(d_->seat);
 if(d_->queue)wl_event_queue_destroy(d_->queue);
 if(d_->map)xkb_keymap_unref(d_->map);
 if(d_->context)xkb_context_unref(d_->context);
#endif
#ifdef PUTGD_X11_KEYBOARD
 if(d_->symbols)xcb_key_symbols_free(d_->symbols);
#endif
}
QKeyCombination ShortcutKeyboard::chord(const QKeyEvent& event) {
 const auto pressed=event.keyCombination();
 if(!pressed.keyboardModifiers().testFlag(Qt::ShiftModifier))return pressed;
#ifdef PUTGD_WAYLAND_KEYBOARD
 if(d_->keyboard){
  // Apply layout switches that arrived since the roundtrip.
  wl_display_dispatch_queue_pending(d_->display,d_->queue);
  return unshiftedChord(pressed,d_->map,d_->layout,event.nativeScanCode());
 }
#endif
#ifdef PUTGD_X11_KEYBOARD
 if(d_->symbols){
  // Core key symbols hold two groups: columns 0/1 and 2/3. The XKB group
  // travels in bits 13-14 of the event state.
  const auto code=event.nativeScanCode(),group=(event.nativeModifiers()>>13)&3;
  if(code<8||code>255||group>1)return pressed;
  const auto keycode=static_cast<xcb_keycode_t>(code);
  return unshiftedChord(pressed,xcb_key_symbols_get_keysym(d_->symbols,keycode,int(group*2)),xcb_key_symbols_get_keysym(d_->symbols,keycode,int(group*2+1)));
 }
#endif
 return pressed;
}
}
