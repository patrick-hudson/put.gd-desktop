#pragma once
#include <QString>
#include <QByteArray>
#include <xcb/xcb.h>
namespace putgd {
// One resident-owned server cursor. Only call between capture helper generations.
// The helper retains Qt/native cursor loading and its supervised failure boundary.
class CaptureCursorCache final {
public:
 CaptureCursorCache();
 ~CaptureCursorCache();
 CaptureCursorCache(const CaptureCursorCache&)=delete;
 CaptureCursorCache& operator=(const CaptureCursorCache&)=delete;
 bool supported()const{return supported_;}
 xcb_cursor_t lookup(const QString& fingerprint);
 bool seed(int width,int height,int xhot,int yhot,const QByteArray& networkArgb,const QString& fingerprint);
 void clear();
private:
 bool checked(xcb_void_cookie_t);
 xcb_connection_t* connection_=nullptr;
 xcb_cursor_t cursor_=XCB_CURSOR_NONE;
 QString fingerprint_;
 bool supported_=false;
 quint32 format_=0;
};
}
