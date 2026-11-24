#pragma once
#include "platform/Geometry.h"
#include <QJsonArray>
#include <QObject>
#include <xcb/xcb.h>
namespace putgd {
struct X11Target {
 QString action;
 xcb_window_t window=XCB_WINDOW_NONE;
 QRect nativeWindowRect;
 // Capture monitor (#1363): the native rectangle of the output under the pointer.
 QRect nativeArea;
 // Delayed capture (#1363): a live region counts down between selection and pixels.
 int delaySeconds=0;
 QList<CaptureFrame> layout;
 QRectF logicalSelection;
 QString outcome;
 // Region screenshots in Screenshot first mode (#629): every monitor is read
 // before selection, and the result is cut from those frames.
 bool screenshotFirst=false;
 // Copy text uses region selection without replacing Repeat last region.
 bool rememberRegion=true;
};
// The monitors a region was chosen on, for Repeat last region (#1363): one
// {identity, logical, native, rotation} per monitor, in layout order.
QJsonArray describeLayout(const QList<CaptureFrame>&);
class X11Capture final:public QObject {
 Q_OBJECT
public:
 explicit X11Capture(QObject* parent=nullptr);
 ~X11Capture() override;
 bool available() const;
 bool cinnamonCompositor();
 void setUiProcess(qint64 pid){uiProcess_=pid>0?pid:0;}
 X11Target freeze(const QString&,Error&);
 QList<CaptureFrame> frames(const X11Target&,Error&);
 bool unchanged(const X11Target&,Error&);
private:
 QList<CaptureFrame> layout(Error&);
 QRect windowRect(xcb_window_t,Error&);
 xcb_atom_t atom(const char*);
 xcb_connection_t* connection_=nullptr;
 xcb_window_t root_=XCB_WINDOW_NONE;
 qint64 uiProcess_=0;
};
}
