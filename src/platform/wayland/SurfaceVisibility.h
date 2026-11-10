#pragma once
#include <functional>
class QObject;
namespace putgd {
// Before reading pixels, require a fresh frame from an input-empty overlay.
// labwc withholds it behind gtklock surfaces; silence fails closed.
void querySurfaceVisible(QObject* context,std::function<void(bool)> reply);
}
