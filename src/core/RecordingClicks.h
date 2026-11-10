#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <functional>

namespace putgd {
// Pointer button presses for a recording's click highlights (#1364), where the
// desktop is what knows them. On X11 the recording helper reads the X server's
// raw button events itself and this is unset. On GNOME the put.gd Shell
// extension reports each press; on Hyprland runtime, non-consuming mouse binds
// do; on Plasma KWin has no event a script can pass on, so its own Mouse Click
// Animation effect is switched on for the recording and draws the ring itself;
// on Budgie labwc mouse binds report that a button went down and the recording
// helper knows where the pointer is.
// Watching never grabs the pointer and never consumes an event.
class RecordingClicks {
public:
    // {x, y, button: 1 left | 2 middle | 3 right, window?: {identity, rect: [x, y, w, h]}}
    // in logical desktop pixels, on the adapter's thread. A route that cannot say
    // where sends {button, pointer: true}: the helper places it at the capture's pointer.
    using Sink = std::function<void(QJsonObject)>;
    using Reply = std::function<void(Error, QJsonObject)>;
    virtual ~RecordingClicks() = default;
    // {available, route, drawnBy: recorder | compositor, targets: [monitor, ...],
    //  targetReasons?: {kind: why not}, reason?, message?}. No prompt, no side effect.
    // Two keys are for the recording service alone and never reported: needsPointer
    // (the helper's capture must offer the pointer's position) and beforeCapture
    // (starting the watch shows on screen, so it starts before the capture does).
    virtual QJsonObject clickCapability() const = 0;
    // Presses from now until unwatchClicks, for a recording of `target`
    // ({kind, identity?}). Replies once the desktop is delivering them.
    virtual void watchClicks(const QJsonObject& target, Sink, Reply) = 0;
    // Stops, and puts back whatever watching changed on the desktop.
    virtual void unwatchClicks() = 0;
};
}
