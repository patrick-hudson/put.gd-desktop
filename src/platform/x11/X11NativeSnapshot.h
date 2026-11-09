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
 // the remembered logical rectangle as `selection` and the monitors it was
 // chosen on as `selectionLayout`; resolve() refuses when they are no longer
 // the monitors there are.
 QJsonObject freeze(const QString& action,const QSet<qint64>& excluded,Error&,const QJsonArray& selection={},const QJsonArray& selectionLayout={});
 // A layout as a region result reports it and a repeat carries it: one
 // {identity, logical, native, rotation} per monitor. Qt's logical rectangles
 // depend on the scale factor, so only the helper writes one (describeLayout
 // in X11Capture.h); this checks its shape.
 static bool describes(const QJsonArray&);
 // Whether the remembered monitors have the native rectangles and rotations of
 // the current RandR outputs: the check the resident can make without Qt.
 bool sameNativeOutputs(const QJsonArray& remembered,Error&);
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
