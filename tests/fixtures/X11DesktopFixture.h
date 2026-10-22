#pragma once
#include <xcb/xcb.h>
#include <xcb/xtest.h>
#include <xcb/xcb_keysyms.h>
#include <QSet>
#include <cstdlib>
// A genuine separate X11 client window and XTest input source, restricted to
// tests launched by xvfb-run. No physical display is used by the unit suite.
class X11DesktopFixture {
public:
 X11DesktopFixture(){
  connection=xcb_connect(nullptr,nullptr);auto* screen=xcb_setup_roots_iterator(xcb_get_setup(connection)).data;window=xcb_generate_id(connection);uint32_t values[]{0x00ff00,1};xcb_create_window(connection,screen->root_depth,window,screen->root,20,20,80,60,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_BACK_PIXEL|XCB_CW_OVERRIDE_REDIRECT,values);xcb_map_window(connection,window);xcb_set_input_focus(connection,XCB_INPUT_FOCUS_POINTER_ROOT,window,XCB_CURRENT_TIME);xcb_flush(connection);symbols=xcb_key_symbols_alloc(connection);inputFocus();
 }
 ~X11DesktopFixture(){for(auto code:pressed_)xcb_test_fake_input(connection,XCB_KEY_RELEASE,code,XCB_CURRENT_TIME,0,0,0,0);xcb_destroy_window(connection,window);xcb_flush(connection);inputFocus();xcb_key_symbols_free(symbols);xcb_disconnect(connection);}
 xcb_keycode_t code(xcb_keysym_t key){auto* codes=xcb_key_symbols_get_keycode(symbols,key);auto result=codes?codes[0]:xcb_keycode_t(0);free(codes);return result;}
 void key(xcb_keysym_t key,bool down){rawKey(code(key),down);}
 void rawKey(xcb_keycode_t code,bool down){if(down)pressed_.insert(code);else pressed_.remove(code);xcb_test_fake_input(connection,down?XCB_KEY_PRESS:XCB_KEY_RELEASE,code,XCB_CURRENT_TIME,0,0,0,0);xcb_flush(connection);}
 xcb_window_t inputFocus(){auto* reply=xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr);auto focus=reply?reply->focus:xcb_window_t(0);free(reply);return focus;}
 xcb_connection_t* connection=nullptr;xcb_window_t window=0;xcb_key_symbols_t* symbols=nullptr;
private:
 QSet<xcb_keycode_t> pressed_;
};
// A separate client that holds an active keyboard grab (and optionally the
// pointer) on its own mapped window, as an open menu does (#341).
class X11GrabbingClient {
public:
 explicit X11GrabbingClient(bool pointer=false){
  connection=xcb_connect(nullptr,nullptr);auto* screen=xcb_setup_roots_iterator(xcb_get_setup(connection)).data;window=xcb_generate_id(connection);uint32_t values[]{0xff00ff,1,XCB_EVENT_MASK_KEY_PRESS};xcb_create_window(connection,screen->root_depth,window,screen->root,200,200,40,40,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_BACK_PIXEL|XCB_CW_OVERRIDE_REDIRECT|XCB_CW_EVENT_MASK,values);xcb_map_window(connection,window);
  free(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));
  auto* keyboard=xcb_grab_keyboard_reply(connection,xcb_grab_keyboard(connection,0,window,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC),nullptr);
  grabbed=keyboard&&keyboard->status==XCB_GRAB_STATUS_SUCCESS;free(keyboard);
  if(pointer){auto* reply=xcb_grab_pointer_reply(connection,xcb_grab_pointer(connection,0,window,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,XCB_NONE,XCB_CURRENT_TIME),nullptr);grabbed=grabbed&&reply&&reply->status==XCB_GRAB_STATUS_SUCCESS;free(reply);}
 }
 ~X11GrabbingClient(){release();xcb_destroy_window(connection,window);free(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));xcb_disconnect(connection);}
 void release(){xcb_ungrab_keyboard(connection,XCB_CURRENT_TIME);xcb_ungrab_pointer(connection,XCB_CURRENT_TIME);free(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));}
 // Key presses the grab delivered here: the "menu" still sees the keys.
 int keyPresses(){free(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));while(auto* event=xcb_poll_for_event(connection)){if((event->response_type&127)==XCB_KEY_PRESS)++presses_;free(event);}return presses_;}
 xcb_connection_t* connection=nullptr;xcb_window_t window=0;bool grabbed=false;
private:
 int presses_=0;
};
