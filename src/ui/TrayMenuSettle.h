#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <optional>

// #2081: a capture chosen from the tray menu must not picture the menu. The
// tray host draws the menu, so put.gd cannot see it; it can only tell when the
// host closed it and how long the desktop's compositor keeps drawing a closed
// popup while it fades out. Measured on the lab desktops (click to last visible
// frame): Plasma 6.3 and 6.8 about 470 ms (KWin's fadingpopups effect), GNOME on
// Fedora 44 about 150 ms (the shell's popup animation), Cinnamon on Mint 22.3 under
// 30 ms (no fade). Without the wait every Repeat last region, monitor and desktop
// capture from the menu caught it on Plasma and GNOME; Cinnamon never did.
namespace putgd::traysettle {
// Tray rows whose work reads the screen, now or from a frozen selection.
bool readsScreen(const QString& command);
// How long a closed popup stays visible on this desktop, and what said so.
struct Fade { int ms = 0; QString source; };
// The desktop settings fade() decides from. Empty strings and nullopt mean unset.
struct FadeInputs {
 QString desktop;                       // XDG_CURRENT_DESKTOP
 QString kwinFadingPopups;              // kwinrc [Plugins] fadingpopupsEnabled
 QString kdeAnimationFactor;            // kdeglobals [KDE] AnimationDurationFactor
 std::optional<bool> gnomeAnimations;   // org.gnome.desktop.interface enable-animations
};
// One frame at 30 Hz: the compositor's next frame after the popup's last one.
constexpr int frameMarginMs = 34;
// A host that reports closes gets this long to report this one.
constexpr int closeWaitMs = 250;
// Desktops put.gd has not measured.
constexpr int unknownFadeMs = 250;
Fade fade(const FadeInputs&);
// What is left of the fade `sinceCloseMs` after the menu closed (never negative).
int remainingMs(const Fade&, qint64 sinceCloseMs);
}

namespace putgd {
// Holds a tray menu command until the menu it came from is off the screen: the
// host's close, when the host reports closes and has not reported this one yet
// (bounded by closeWaitMs), then the rest of the compositor's fade. A close that
// arrives while a fade still counts from the click starts the fade again from it.
class TrayMenuSettle final : public QObject {
 Q_OBJECT
public:
 explicit TrayMenuSettle(QObject* parent = nullptr);
 // The host is showing the menu: read the desktop's fade settings now, before a row is chosen.
 // `reportsEvents`: the host sent an "opened" event for it, so it reports events and its
 // close is waited for even before put.gd has seen one (AboutToShow alone says nothing).
 void menuShown(bool reportsEvents = false);
 // The host closed the menu.
 void menuClosed();
 // Runs `run` once the menu is gone (see the class comment). `command` names it in state().
 void afterMenu(const QString& command, std::function<void()> run);
 // The last wait, for ui.snapshot diagnostics.trayMenu.settle: the command,
 // whether it waited for the host's close and how long, the fade it allowed
 // (ms and source) and the whole wait from the click, in ms.
 QJsonObject state() const;
 // Tests set the fade instead of reading the desktop.
 void setFade(const traysettle::Fade& fade) { fade_ = fade; fixedFade_ = true; }
 // The desktop's inputs as this session has them; GNOME's setting arrives
 // later, through the portal.
 static traysettle::FadeInputs readInputs();
private:
 struct Waiting { quint64 id = 0; QString command; std::function<void()> run; QElapsedTimer clock; bool awaitedClose = false; };
 void startFade(Waiting waiting);
 void refreshFade();
 traysettle::Fade fade_;
 traysettle::FadeInputs inputs_;
 bool fixedFade_ = false, open_ = false, eventShowing_ = false;
 int closes_ = 0;
 quint64 waits_ = 0;
 QElapsedTimer sinceClose_;
 QList<Waiting> awaitingClose_;
 // Fades counting down; fromClose is false while one counts from the click.
 struct Fading { QTimer* timer = nullptr; QElapsedTimer clock; bool fromClose = false; };
 QList<Fading> fading_;
 QJsonObject last_;
};
}
