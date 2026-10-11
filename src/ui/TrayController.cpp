#include "TrayController.h"
#include "VisualStyle.h"
#ifdef Q_OS_LINUX
#include "NativeTray.h"
#endif
#include <QApplication>
#include <QCoreApplication>
#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QSystemTrayIcon>
#include <utility>

namespace putgd {
TrayController::TrayController(OperationService& operations,
                               std::function<void(const QString&)> openView,
                               std::function<void()> quit, QObject* parent)
 : QObject(parent), operations_(operations), openView_(std::move(openView)),
   quit_(std::move(quit)) {
#ifdef Q_OS_LINUX
 if (!qobject_cast<QApplication*>(QCoreApplication::instance())) {
  native_ = std::make_unique<NativeTray>(this);
  connect(native_.get(), &NativeTray::availabilityResolved, this, &TrayController::availabilityResolved);
  connect(native_.get(), &NativeTray::actionActivated, this, [this](const QString& action) {
   if (Settings::actionNames().contains(action)) invokeAction(action);
   else if (action == "settings") showSettings();
   else if (action == "history") showHistory();
   else if (action == "activity") showActivity();
   else if (action == "about") showAbout();
   else if (action == "quit") { const auto quit = quit_; quit(); }
  });
 } else
#endif
 {
 tray_ = new QSystemTrayIcon(VisualStyle::icon("brand"), this);
 menu_ = std::make_unique<QMenu>();
 tray_->setToolTip("put.gd — Share and local capture");
 for (const auto& action : Settings::actionNames()) {
  auto* item = menu_->addAction(VisualStyle::icon(action), "Capture " + action);
  connect(item, &QAction::triggered, this, [this, action] {
   invokeAction(action);
  });
 }
 menu_->addSeparator();
 connect(menu_->addAction(VisualStyle::icon("preferences"), "Settings and setup"),
         &QAction::triggered, this, &TrayController::showSettings);
 connect(menu_->addAction(VisualStyle::icon("history"), "Upload history"),
         &QAction::triggered, this, &TrayController::showHistory);
 connect(menu_->addAction(VisualStyle::icon("activity"), "Recent activity"),
         &QAction::triggered, this, &TrayController::showActivity);
 menu_->addSeparator();
 connect(menu_->addAction("About put.gd"), &QAction::triggered, this, &TrayController::showAbout);
 connect(menu_->addAction("Quit"), &QAction::triggered, this, [this] {
  const auto quit = quit_;
  quit();
 });
 tray_->setContextMenu(menu_.get());
 connect(tray_, &QSystemTrayIcon::activated, this, [this](auto reason) {
  if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
   showActivity();
 });
 // Qt registers a visible icon when a desktop tray becomes available later.
 tray_->show();
 }
 connect(&operations_, &OperationService::changed, this, [this](const QString& id) {
  if (!available() || !operations_.settingsSnapshot().notifications) return;
  operations_.inspect(id, [guard = QPointer<TrayController>(this)](Error error, QJsonObject operation) {
   if (!guard || error) return;
   const auto state = operation["state"].toString();
   if ((state == "completed" || state == "partial") && operation["copyStatus"] == "pending")
    return;
   if (QStringList{"completed", "partial", "failed", "interrupted"}.contains(state))
    guard->notifyMessage("put.gd — " + state,
      "Clipboard: " + operation["copyStatus"].toString() + ". Open recent activity for per-item results.",
      state != "completed");
  });
 });
}

TrayController::~TrayController() {
 if (tray_) {
  tray_->hide();
  tray_->setContextMenu(nullptr);
 }
}

#ifdef Q_OS_LINUX
bool TrayController::available() const { return native_ ? native_->available() : QSystemTrayIcon::isSystemTrayAvailable(); }
bool TrayController::availabilityKnown() const { return !native_ || native_->availabilityKnown(); }
#else
bool TrayController::available() const { return QSystemTrayIcon::isSystemTrayAvailable(); }
bool TrayController::availabilityKnown() const { return true; }
#endif
void TrayController::activate() { showSettings(); }
void TrayController::showSettings() { showView("settings"); }
void TrayController::showHistory() { showView("history"); }
void TrayController::showActivity() { showView("activity"); }
void TrayController::showAbout() { showView("about"); }
void TrayController::showView(const QString& view) {
 const auto open = openView_;
 open(view);
}
void TrayController::invokeAction(const QString& action) {
 operations_.invoke(action, [guard = QPointer<TrayController>(this)](Error error, QJsonObject) {
  if (guard && error) {
   guard->showActivity();
   if (guard) guard->notifyFailure(error);
  }
 });
}
void TrayController::notifyMessage(const QString& title, const QString& message, bool warning) {
#ifdef Q_OS_LINUX
 if (native_) native_->notify(title, message, warning);
 else
#endif
 if (tray_) tray_->showMessage(title, message, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information);
}
void TrayController::notifyFailure(const Error& error) {
 if (error && available()) notifyMessage("put.gd", error.message, true);
}
}
