#pragma once
#include "core/Error.h"
#include <xcb/xcb.h>
#include <QJsonArray>
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
 // monitor (#1363) freezes the output under the pointer as "area". repeat takes
 // the remembered logical rectangle as `selection`; the caller has already
 // compared layouts with sameArrangement().
 QJsonObject freeze(const QString& action,const QSet<qint64>& excluded,Error&,const QJsonArray& selection={});
 // Whether two frozen layouts hold the same outputs, root and scaling. The
 // RandR configuration timestamp also moves when the same layout is applied again.
 static bool sameArrangement(const QJsonObject& layout,const QJsonObject& other);
 // The current layout alone, for that comparison.
 QJsonObject currentLayout(Error& error){return layout(error);}
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
