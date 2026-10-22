#pragma once
#include <QObject>
#include <functional>
namespace putgd {
// Asks the session's screen lockers whether the screen is locked (#341).
// XInput2 raw keys reach put.gd even while a locker holds the keyboard grab,
// so a raw-only shortcut must not capture or upload from a locked session.
// Cinnamon's and the freedesktop screensaver services and logind's LockedHint
// are asked without activating anything. A service that is absent counts as
// unlocked; one that does not answer in time counts as locked.
void querySessionLocked(QObject* context,std::function<void(bool locked)> done);
}
