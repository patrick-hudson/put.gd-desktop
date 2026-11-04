#include "CaptureCursorCache.h"
#include <xcb/render.h>
#include <QtEndian>
#include <cstdlib>
namespace putgd {
namespace {
bool fingerprintValid(const QString& value){
 if(value.size()!=64)return false;
 for(const auto c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
 return true;
}
}
CaptureCursorCache::CaptureCursorCache(){
 connection_=xcb_connect(nullptr,nullptr);
 if(!connection_||xcb_connection_has_error(connection_))return;
 const auto* extension=xcb_get_extension_data(connection_,&xcb_render_id);
 if(!extension||!extension->present)return;
 auto* version=xcb_render_query_version_reply(connection_,xcb_render_query_version(connection_,0,5),nullptr);
 const bool supported=version&&(version->major_version>0||version->minor_version>=5);std::free(version);
 if(!supported)return;
 auto* formats=xcb_render_query_pict_formats_reply(connection_,xcb_render_query_pict_formats(connection_),nullptr);
 if(!formats)return;
 auto iterator=xcb_render_query_pict_formats_formats_iterator(formats);
 for(;iterator.rem;xcb_render_pictforminfo_next(&iterator)){
  const auto& f=*iterator.data;const auto& d=f.direct;
  if(f.type==XCB_RENDER_PICT_TYPE_DIRECT&&f.depth==32&&d.red_shift==16&&d.red_mask==255&&d.green_shift==8&&d.green_mask==255&&d.blue_shift==0&&d.blue_mask==255&&d.alpha_shift==24&&d.alpha_mask==255){format_=f.id;break;}
 }
 std::free(formats);supported_=format_!=0;
}
CaptureCursorCache::~CaptureCursorCache(){releasePointer();clear();if(connection_)xcb_disconnect(connection_);}
bool CaptureCursorCache::checked(xcb_void_cookie_t cookie){
 auto* error=xcb_request_check(connection_,cookie);const bool ok=!error&&!xcb_connection_has_error(connection_);std::free(error);return ok;
}
bool CaptureCursorCache::holdPointer(){
 if(holding_||!cursor_||!supported_||xcb_connection_has_error(connection_))return false;
 const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;
 // Buttons only: motion is not needed to place a selection, and this connection
 // reads nothing until the release, however long the helper takes to load.
 auto* reply=xcb_grab_pointer_reply(connection_,xcb_grab_pointer(connection_,false,root,XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_WINDOW_NONE,cursor_,XCB_CURRENT_TIME),nullptr);
 holding_=reply&&reply->status==XCB_GRAB_STATUS_SUCCESS;std::free(reply);
 return holding_;
}
QList<CaptureCursorCache::HeldButton> CaptureCursorCache::releasePointer(){
 QList<HeldButton> buttons;
 if(!holding_)return buttons;
 holding_=false;
 xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);
 // Every event from before the ungrab has reached this connection after the fence.
 auto* fence=xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr);std::free(fence);
 while(auto* event=xcb_poll_for_event(connection_)){
  const auto type=event->response_type&127;
  if(type==XCB_BUTTON_PRESS||type==XCB_BUTTON_RELEASE){
   const auto* button=reinterpret_cast<xcb_button_press_event_t*>(event);
   // Only these act on a selection; a wheel sends a stream of presses of its own.
   if((button->detail==1||button->detail==3)&&buttons.size()<64)buttons.append({type==XCB_BUTTON_PRESS,button->detail,button->root_x,button->root_y});
  }
  std::free(event);
 }
 return buttons;
}
void CaptureCursorCache::clear(){
 if(cursor_&&connection_){xcb_free_cursor(connection_,cursor_);xcb_flush(connection_);}
 cursor_=XCB_CURSOR_NONE;fingerprint_.clear();
}
xcb_cursor_t CaptureCursorCache::lookup(const QString& fingerprint){
 if(!supported_||xcb_connection_has_error(connection_)||!fingerprintValid(fingerprint)||fingerprint!=fingerprint_){clear();return XCB_CURSOR_NONE;}
 return cursor_;
}
bool CaptureCursorCache::seed(int width,int height,int xhot,int yhot,const QByteArray& networkArgb,const QString& fingerprint){
 if(!supported_||!fingerprintValid(fingerprint)||xcb_connection_has_error(connection_)||width<1||height<1||width>64||height>64||xhot<0||yhot<0||xhot>=width||yhot>=height||networkArgb.size()!=qint64(width)*height*4)return false;
 if(uint64_t(sizeof(xcb_put_image_request_t))+uint64_t(networkArgb.size())>uint64_t(xcb_get_maximum_request_length(connection_))*4)return false;
 // Make a new server cursor with our own identity. XFixes ChangeCursor keeps
 // the producer's internal cursor ID, causing image-access denial after it exits.
 QByteArray pixels(networkArgb.size(),Qt::Uninitialized);
 const auto* setup=xcb_get_setup(connection_);
 for(qsizetype offset=0;offset<pixels.size();offset+=4){
  const auto value=qFromBigEndian<quint32>(networkArgb.constData()+offset);
  if(setup->image_byte_order==XCB_IMAGE_ORDER_LSB_FIRST)qToLittleEndian(value,pixels.data()+offset);
  else qToBigEndian(value,pixels.data()+offset);
 }
 const auto root=xcb_setup_roots_iterator(setup).data->root;
 const auto pixmap=xcb_generate_id(connection_),gc=xcb_generate_id(connection_),picture=xcb_generate_id(connection_),cursor=xcb_generate_id(connection_);
 if(!checked(xcb_create_pixmap_checked(connection_,32,pixmap,root,uint16_t(width),uint16_t(height))))return false;
 const bool gcCreated=checked(xcb_create_gc_checked(connection_,gc,pixmap,0,nullptr));
 bool pictureCreated=false,created=false;
 if(gcCreated&&checked(xcb_put_image_checked(connection_,XCB_IMAGE_FORMAT_Z_PIXMAP,pixmap,gc,uint16_t(width),uint16_t(height),0,0,0,32,uint32_t(pixels.size()),reinterpret_cast<const uint8_t*>(pixels.constData())))){
  pictureCreated=checked(xcb_render_create_picture_checked(connection_,picture,pixmap,format_,0,nullptr));
  if(pictureCreated)created=checked(xcb_render_create_cursor_checked(connection_,cursor,picture,uint16_t(xhot),uint16_t(yhot)));
 }
 if(pictureCreated)xcb_render_free_picture(connection_,picture);
 if(gcCreated)xcb_free_gc(connection_,gc);
 xcb_free_pixmap(connection_,pixmap);xcb_flush(connection_);
 if(!created)return false;
 clear();cursor_=cursor;fingerprint_=fingerprint;return true;
}
}
