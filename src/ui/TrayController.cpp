#include "TrayController.h"
#include "VisualStyle.h"
#include "ByteSize.h"
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
#include <algorithm>

namespace putgd {
namespace {
const QString idleTooltip = "put.gd — Share and local capture";
QString phaseLabel(const QString& phase) {
 if(phase=="preflight")return "Checking destination";
 if(phase=="saving_local")return "Saving local copy";
 if(phase=="sharing")return "Creating share link";
 if(phase=="reconciling")return "Checking previous upload";
 auto text=phase;text.replace('_',' ');
 return text.isEmpty()?QString("Preparing"):text.left(1).toUpper()+text.mid(1);
}
QString activityLine(const QJsonObject& operation) {
 const auto phase=operation["phase"].toString(operation["state"].toString());
 auto text=phaseLabel(phase);const auto items=operation["items"].toArray();
 if(phase=="uploading"||phase=="finalizing"||phase=="preparing") {
  for(qsizetype index=0;index<items.size();++index) {
   const auto item=items[index].toObject();if(item["uploadStatus"]!="uploading")continue;
   if(items.size()>1)text+=QString(" file %1/%2").arg(index+1).arg(items.size());
   const auto progress=UploadProgress::fromJson(item["uploadProgress"].toObject());
   if(progress&&progress->bytesTransferred) {
    text+=": "+ByteSize::summary(*progress->bytesTransferred);
    if(progress->bytesTotal) {
     text+=" of "+ByteSize::summary(*progress->bytesTotal);
     if(*progress->bytesTotal>0)text+=QString(" (%1%)").arg(int(100.0 * *progress->bytesTransferred / *progress->bytesTotal));
    } else {
     // Providers may count encrypted/network bytes. Source bytes are a separate
     // size, never the denominator of a fabricated transfer percentage.
     text+=" transferred · Source size: "+ByteSize::summary(item["artifact"].toObject()["bytes"].toInteger());
    }
   } else text+=" · Source size: "+ByteSize::summary(item["artifact"].toObject()["bytes"].toInteger());
   break;
  }
 }
 return text;
}
QString activityText(const QJsonArray& observations) {
 auto earlier=[](const auto& a,const auto& b){
  const auto left=a["created"].toString(),right=b["created"].toString();return left==right?a["id"].toString()<b["id"].toString():left<right;
 };
 QList<QJsonObject> displayed;
 for(const auto& value:observations) {
  const auto operation=value.toObject();const auto position=std::lower_bound(displayed.begin(),displayed.end(),operation,earlier);
  if(position==displayed.end()&&displayed.size()==3)continue;
  displayed.insert(position,operation);if(displayed.size()>3)displayed.removeLast();
 }
 QStringList lines;for(const auto& operation:displayed)lines.append(activityLine(operation));
 if(observations.size()>3)lines.append(QString("%1 more active operations").arg(observations.size()-3));
 return lines.join('\n');
}
}

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
 tray_->setToolTip(idleTooltip);
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
 connect(&operations_, &OperationService::observationsChanged, this, &TrayController::refreshActivity);
 refreshActivity();
 connect(&operations_, &OperationService::resultReady, this, [this](const QString& id) {
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
void TrayController::refreshActivity() {
 const auto observations=operations_.activeObservations();const bool working=!observations.isEmpty();
 const auto text=activityText(observations);if(working==working_&&text==activityText_)return;
#ifdef Q_OS_LINUX
 if(native_)native_->setActivity(working,text);
 else
#endif
 if(tray_) {
  if(working!=working_)tray_->setIcon(VisualStyle::icon("brand",QColor(working?"#62d69b":"#9bbcff")));
  tray_->setToolTip(working?QString("put.gd — Working\n")+text:idleTooltip);
 }
 working_=working;activityText_=text;
}
void TrayController::activate() { showSettings(); }
void TrayController::showSettings() { showView("settings"); }
void TrayController::showHistory() { showView("history"); }
void TrayController::showActivity() { showView("activity"); }
void TrayController::showAbout() { showView("about"); }
void TrayController::showView(const QString& view) {
 const auto open = openView_;
 open(view);
}
void TrayController::invokeShortcut(const QString& action) { invokeAction(action, true); }
void TrayController::invokeAction(const QString& action, bool fromShortcut) {
 operations_.invoke(action, [guard = QPointer<TrayController>(this), fromShortcut](Error error, QJsonObject) {
  if (guard && error) {
   qWarning("Action rejected: %s", qPrintable(error.code));
   if (fromShortcut) {
    // Re-entry must not map a view over selection or startup recovery.
    if (error.code == "busy_selecting" || error.code == "initializing") return;
    if (guard->shortcutFeedbackClock_.isValid() && guard->lastShortcutError_ == error.code &&
        guard->shortcutFeedbackClock_.elapsed() < 2000) return;
    guard->lastShortcutError_ = error.code;
    guard->shortcutFeedbackClock_.start();
   }
   if (!fromShortcut) guard->showActivity();
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
