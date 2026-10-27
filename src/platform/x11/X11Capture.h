#pragma once
#include "platform/Geometry.h"
#include <QObject>
#include <xcb/xcb.h>
namespace putgd {
struct X11Target {
 QString action;
 xcb_window_t window=XCB_WINDOW_NONE;
 QRect nativeWindowRect;
 QList<CaptureFrame> layout;
 QRectF logicalSelection;
 QString outcome;
};
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
