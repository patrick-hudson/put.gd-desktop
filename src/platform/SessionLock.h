#pragma once
#include "core/Error.h"
#include <QObject>
#include <functional>
namespace putgd {
// Asks the session's screen lockers whether the screen is locked (#341).
// XInput2 raw keys reach put.gd even while a locker holds the keyboard grab,
// so a raw-only shortcut must not capture or upload from a locked session.
// Cinnamon's and the freedesktop screensaver services and logind's LockedHint
// are asked without activating anything. A service that is absent counts as
// unlocked; one that does not answer in time counts as locked.
// Every desktop adapter also asks when a capture delay ends (#1363): Plasma
// answers on the freedesktop service, and lockers that only set logind's hint
// answer there.
void querySessionLocked(QObject* context,std::function<void(bool locked)> done);
// The countdown was not on screen when it ended (#1363): a compositor stops
// presenting every ordinary surface while the session is locked or the output
// is off, and not every desktop has a lock query (Budgie's gtklock sets none).
// The layer-shell countdown reports it, and nothing is captured.
inline Error countdownNotShown() { return {QStringLiteral("session_locked"),QStringLiteral("The countdown was not on screen when the capture delay ended (a locked or blanked screen), so nothing was captured."),{{QStringLiteral("check"),QStringLiteral("countdown_presented")}}}; }
}
