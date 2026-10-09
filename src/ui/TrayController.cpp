#include "TrayController.h"
#include "VisualStyle.h"
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
   quit_(std::move(quit)), tray_(new QSystemTrayIcon(VisualStyle::icon("brand"), this)),
   menu_(std::make_unique<QMenu>()) {
 tray_->setToolTip("put.gd — Share and local capture");
 for (const auto& action : Settings::actionNames()) {
  auto* item = menu_->addAction(VisualStyle::icon(action), "Capture " + action);
  connect(item, &QAction::triggered, this, [this, action] {
   operations_.invoke(action, [guard = QPointer<TrayController>(this)](Error error, QJsonObject) {
    if (guard && error) {
     guard->showActivity();
     if (guard) guard->notifyFailure(error);
    }
   });
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
 if (available()) tray_->show();
 connect(&operations_, &OperationService::changed, this, [this](const QString& id) {
  if (!available() || !operations_.settingsSnapshot().notifications) return;
  operations_.inspect(id, [guard = QPointer<TrayController>(this)](Error error, QJsonObject operation) {
   if (!guard || error) return;
   const auto state = operation["state"].toString();
   if ((state == "completed" || state == "partial") && operation["copyStatus"] == "pending")
    return;
   if (QStringList{"completed", "partial", "failed", "interrupted"}.contains(state))
    guard->tray_->showMessage("put.gd — " + state,
      "Clipboard: " + operation["copyStatus"].toString() + ". Open recent activity for per-item results.",
      state == "completed" ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning);
  });
 });
}

TrayController::~TrayController() {
 tray_->hide();
 tray_->setContextMenu(nullptr);
}

bool TrayController::available() const { return QSystemTrayIcon::isSystemTrayAvailable(); }
void TrayController::activate() { showSettings(); }
void TrayController::showSettings() { showView("settings"); }
void TrayController::showHistory() { showView("history"); }
void TrayController::showActivity() { showView("activity"); }
void TrayController::showAbout() { showView("about"); }
void TrayController::showView(const QString& view) {
 const auto open = openView_;
 open(view);
}
void TrayController::notifyFailure(const Error& error) {
 if (error && available()) tray_->showMessage("put.gd", error.message, QSystemTrayIcon::Warning);
}
}
