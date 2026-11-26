#include "TrayMenuSettle.h"
#include "PanelTone.h"
#include "ChildProcess.h"
#include "core/HostEnvironment.h"
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <cmath>
#include <utility>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#endif

namespace putgd::traysettle {
namespace {
QStringList desktops(const QString& desktop) { return desktop.toLower().split(':', Qt::SkipEmptyParts); }
std::optional<bool> kdeBool(const QString& value) {
 const auto v = value.trimmed().toLower();
 if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
 if (v == "false" || v == "0" || v == "no" || v == "off") return false;
 return {};
}
}
bool readsScreen(const QString& command) {
 // "edit" is Capture region & edit; the Edit image submenu's clipboard and file rows read no screen.
 return QStringList{"region", "repeat", "window", "monitor", "desktop", "scroll", "edit", "record", "text"}.contains(command);
}
Fade fade(const FadeInputs& in) {
 const auto names = desktops(in.desktop);
 if (names.contains("kde")) {
  // KWin's fadingpopups effect (on by default) fades a closed popup out over
  // animationTime(150) * 4 ms with an OutQuart curve, so (1 - t)^4 of it is
  // left at t. It is invisible once under 2/255: at t = 1 - (2/255)^(1/4).
  // AnimationDurationFactor scales every KWin animation; 0 is "Instant".
  if (kdeBool(in.kwinFadingPopups) == false) return {frameMarginMs, "kwin-fadingpopups-off"};
  bool ok = false; double factor = in.kdeAnimationFactor.trimmed().toDouble(&ok);
  if (!ok || !std::isfinite(factor) || factor < 0) factor = 1.0;
  factor = qMin(factor, 8.0);
  if (factor == 0) return {frameMarginMs, "kwin-animations-instant"};
  const double visible = 1.0 - std::pow(2.0 / 255.0, 0.25);
  return {int(std::ceil(600.0 * factor * visible)) + frameMarginMs, "kwin-fadingpopups"};
 }
 if (names.contains("gnome") || names.contains("ubuntu")) {
  // GNOME Shell's popup menus close with a 150 ms BoxPointer animation, none when animations are off.
  if (in.gnomeAnimations == false) return {frameMarginMs, "gnome-animations-off"};
  return {150 + frameMarginMs, "gnome-popup-animation"};
 }
 if (names.contains("x-cinnamon") || names.contains("cinnamon")) {
  // Cinnamon's applet menus fade out over 150 ms when "effects on menus" is on.
  if (in.cinnamonMenuEffects == false) return {frameMarginMs, "cinnamon-menu-effects-off"};
  return {150 + frameMarginMs, "cinnamon-menu-effects"};
 }
 return {unknownFadeMs, "unmeasured-desktop"};
}
int remainingMs(const Fade& fade, qint64 sinceCloseMs) { return int(qBound<qint64>(0, qint64(fade.ms) - qMax<qint64>(0, sinceCloseMs), qint64(fade.ms))); }
}

namespace putgd {
using namespace traysettle;
TrayMenuSettle::TrayMenuSettle(QObject* parent) : QObject(parent) { refreshFade(); }
FadeInputs TrayMenuSettle::readInputs() {
 FadeInputs in; in.desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
 if (desktops(in.desktop).contains("kde")) {
  in.kwinFadingPopups = PanelTone::kdeSetting("kwinrc", "Plugins", "fadingpopupsEnabled");
  in.kdeAnimationFactor = PanelTone::kdeSetting("kdeglobals", "KDE", "AnimationDurationFactor");
 }
 return in;
}
void TrayMenuSettle::refreshFade() {
 if (fixedFade_) return;
 const auto known = inputs_;
 inputs_ = readInputs();
 inputs_.gnomeAnimations = known.gnomeAnimations; inputs_.cinnamonMenuEffects = known.cinnamonMenuEffects;
 fade_ = fade(inputs_);
 const auto names = desktops(inputs_.desktop);
#ifdef Q_OS_LINUX
 if (names.contains("gnome") || names.contains("ubuntu")) {
  // The desktop portal serves GNOME's interface settings; the reply lands long before a row is chosen.
  auto call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings", "Read");
  call << QStringLiteral("org.gnome.desktop.interface") << QStringLiteral("enable-animations");
  auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 2000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
   w->deleteLater(); const QDBusPendingReply<QVariant> reply = *w; if (reply.isError()) return;
   QVariant value = reply.value();
   // Read wraps the value in a second variant.
   while (value.userType() == qMetaTypeId<QDBusVariant>()) value = value.value<QDBusVariant>().variant();
   if (value.typeId() != QMetaType::Bool) return;
   inputs_.gnomeAnimations = value.toBool(); if (!fixedFade_) fade_ = fade(inputs_);
  });
 }
 if (names.contains("x-cinnamon") || names.contains("cinnamon")) {
  // Cinnamon's portal does not serve org.cinnamon; the host's gsettings does (as PanelTone reads its theme).
  auto* process = new QProcess(this); process->setStandardErrorFile(QProcess::nullDevice());
  process->setProcessEnvironment(host::environment());
  connect(process, &QProcess::finished, this, [this, process] {
   const auto text = QString::fromUtf8(process->readAllStandardOutput().left(64)).trimmed(); process->deleteLater();
   if (text == "true" || text == "false") { inputs_.cinnamonMenuEffects = text == "true"; if (!fixedFade_) fade_ = fade(inputs_); }
  });
  connect(process, &QProcess::errorOccurred, this, [process](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) process->deleteLater(); });
  QTimer::singleShot(3000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
  terminateWithParent(*process);
  process->start("gsettings", {"get", "org.cinnamon", "desktop-effects-on-menus"});
 }
#endif
}
void TrayMenuSettle::menuShown() { open_ = true; refreshFade(); }
void TrayMenuSettle::menuClosed() {
 open_ = false; ++closes_; sinceClose_.start();
 for (auto& waiting : std::exchange(awaitingClose_, {})) startFade(std::move(waiting));
}
void TrayMenuSettle::afterMenu(const QString& command, std::function<void()> run) {
 Waiting waiting{++waits_, command, std::move(run), {}, false}; waiting.clock.start();
 // Only a host that has reported a close before is waited for: some send "opened" and never "closed".
 if (open_ && closes_ > 0) {
  waiting.awaitedClose = true;
  const auto id = waiting.id;
  awaitingClose_.append(std::move(waiting));
  QTimer::singleShot(closeWaitMs, this, [this, id] {
   for (int i = 0; i < awaitingClose_.size(); ++i)
    if (awaitingClose_[i].id == id) { startFade(awaitingClose_.takeAt(i)); return; }
  });
  return;
 }
 startFade(std::move(waiting));
}
void TrayMenuSettle::startFade(Waiting waiting) {
 // The fade starts when the host closed the menu; without a close report, at the click.
 const qint64 waited = waiting.clock.elapsed();
 const qint64 since = !open_ && sinceClose_.isValid() ? sinceClose_.elapsed() : waited;
 const int rest = remainingMs(fade_, since);
 last_ = {{"command", waiting.command}, {"awaitedClose", waiting.awaitedClose}, {"closeWaitMs", waiting.awaitedClose ? waited : 0},
          {"fadeMs", fade_.ms}, {"fadeSource", fade_.source}, {"waitMs", waited + rest}};
 QTimer::singleShot(rest, this, [run = std::move(waiting.run)] { run(); });
}
QJsonObject TrayMenuSettle::state() const { return last_; }
}
