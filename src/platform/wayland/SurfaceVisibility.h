#pragma once
#include <functional>
class QObject;
namespace putgd {
class CaptureConnection;
// Bracket pixel reads with fresh frames from an input-empty overlay.
// labwc withholds it behind gtklock surfaces; silence fails closed.
void querySurfaceVisible(QObject* context,const CaptureConnection&,std::function<void(bool)> reply);
}
