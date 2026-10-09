#include "X11NativeSnapshot.h"
#include "X11Capture.h"
#include <QJsonArray>
#include <xcb/randr.h>
namespace putgd {
namespace {
QRect rectangle(const QJsonArray& a){return {a[0].toInt(),a[1].toInt(),a[2].toInt(),a[3].toInt()};}
Error targetError(){return {"capture_target","Focused external window is unavailable or changed",{}};}
Error changedError(){return {"display_changed","Display layout or scale changed during source acquisition",{}};}
}
X11Target X11NativeSnapshot::resolve(const QJsonObject& snapshot,X11Capture& capture,Error& error){
 X11Target target;if(!unchanged(snapshot,error))return target;
 // Resolve Qt logical transforms without rediscovering the invocation's active window.
 target=capture.freeze("desktop",error);if(error)return {};
 const auto outputs=snapshot["layout"].toObject()["outputs"].toArray();if(target.layout.size()!=outputs.size()){error=changedError();return {};}
 for(const auto& frame:target.layout){bool matched=false;for(const auto& output:outputs){const auto f=output.toObject();const int bits=f["rotation"].toInt();const int rotation=bits&XCB_RANDR_ROTATION_ROTATE_90?90:bits&XCB_RANDR_ROTATION_ROTATE_180?180:bits&XCB_RANDR_ROTATION_ROTATE_270?270:0;if(f["identity"]==frame.identity&&rectangle(f["native"].toArray())==frame.nativeRect&&rotation==frame.rotation){matched=true;break;}}if(!matched){error=changedError();return {};}}
 target.action=snapshot["action"].toString();target.window=xcb_window_t(snapshot["window"].toString().toUInt());
 if(target.window){target.nativeWindowRect=rectangle(snapshot["windowRect"].toArray());target.logicalSelection={};for(const auto& f:target.layout){const QRect part=f.nativeRect.intersected(target.nativeWindowRect);if(part.isEmpty())continue;target.logicalSelection=target.logicalSelection.united(QRectF(f.logicalRect.x()+(part.x()-f.nativeRect.x())/f.density,f.logicalRect.y()+(part.y()-f.nativeRect.y())/f.density,part.width()/f.density,part.height()/f.density));}if(target.logicalSelection.isEmpty()){error=targetError();return {};}}
 if(!unchanged(snapshot,error))return {};
 return target;
}
}
