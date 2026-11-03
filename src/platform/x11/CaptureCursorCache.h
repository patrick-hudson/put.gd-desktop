#pragma once
#include <QString>
#include <QByteArray>
#include <QList>
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
 // The cursor seeded for this layout, or the last one seeded when the layout is
 // not known yet, with no effect on the cache.
 xcb_cursor_t current()const{return cursor_;}
 // Shows the cursor at once and takes the pointer. Another client's grab refuses
 // it, and nothing changes. Button events made meanwhile are returned on release,
 // in order: a frozen (sync) grab would replay a press at the pointer's final
 // position instead of where it was made.
 bool holdPointer();
 struct HeldButton {bool press=false;int button=0;int x=0,y=0;};
 QList<HeldButton> releasePointer();
 bool holding()const{return holding_;}
private:
 bool checked(xcb_void_cookie_t);
 xcb_connection_t* connection_=nullptr;
 xcb_cursor_t cursor_=XCB_CURSOR_NONE;
 QString fingerprint_;
 bool supported_=false,holding_=false;
 quint32 format_=0;
};
}
