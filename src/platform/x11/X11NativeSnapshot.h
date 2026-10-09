#pragma once
#include "core/Error.h"
#include <xcb/xcb.h>
#include <QJsonObject>
#include <QSet>
namespace putgd {
class X11Capture;
struct X11Target;
// No QPA/GUI initialization: invocation metadata only, never pixel acquisition.
class X11NativeSnapshot final {
public:
 X11NativeSnapshot();
 ~X11NativeSnapshot();
 bool available()const;
 QJsonObject freeze(const QString& action,const QSet<qint64>& excluded,Error&);
 bool unchanged(const QJsonObject&,Error&);
 X11Target resolve(const QJsonObject&,X11Capture&,Error&);
 static Error validate(const QJsonObject&);
private:
 QJsonObject layout(Error&);
 QRect windowRect(xcb_window_t,Error&);
 QByteArray property(xcb_window_t,const char*,Error&);
 QString scaleFingerprint(Error&);
 xcb_atom_t atom(const char*);
 xcb_connection_t* connection_=nullptr;
 xcb_window_t root_=XCB_WINDOW_NONE;
};
}
