#pragma once
#include "core/ActivationToken.h"
#include <QString>
#include <functional>
class QWidget;

namespace putgd {
// Bringing a top-level window forward (#657, #911). On Wayland only the
// compositor decides: GNOME and KWin activate a window for an xdg-activation
// token that came from user input (a tray click hands one over), and otherwise
// leave it where it is and show "… is ready" or flag it for attention. A
// minimised window cannot come back without that activation.
//
// Shows (or restores) `window`, raises it and asks the desktop to activate it,
// with `token` when one is given. Qt's Wayland plugin reads the token from
// XDG_ACTIVATION_TOKEN; this sets it only around the request and clears it.
void activateWindow(QWidget* window, const QString& token = {});
// Calls `done(true)` once `window` is the active window, or `done(false)` after
// `timeoutMs`: the desktop's actual answer. Nothing is called if `window` is
// destroyed first.
void whenActivated(QWidget* window, int timeoutMs, std::function<void(bool)> done);
}
