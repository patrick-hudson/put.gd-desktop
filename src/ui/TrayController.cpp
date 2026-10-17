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
#include <QProcess>
#include <utility>
#include <algorithm>

namespace putgd {
namespace {
bool meaningfulError(const QJsonObject& error) {
 const auto code=error["code"].toString();return !code.isEmpty()&&code!="copy_suppressed";
}
const QString idleTooltip = "put.gd — Share and local capture";
int transferPercent(const QJsonObject& item) {
 const auto progress=UploadProgress::fromJson(item["uploadProgress"].toObject());
 const auto sourceBytes=item["artifact"].toObject()["bytes"].toInteger();
 const bool estimated=!progress||!progress->bytesTotal||*progress->bytesTotal==0;
 const auto total=estimated?sourceBytes:*progress->bytesTotal;
 if(total<=0)return -1;
 if(progress&&progress->phase=="finalizing")return 100;
 const auto sent=progress?progress->bytesTransferred.value_or(0):0;
 // Source size is a useful estimate for providers counting encrypted/network
 // bytes, but overhead must not make that estimate look finished early.
 return int(qBound(0.0L,100.0L*sent/total,estimated?99.0L:100.0L));
}
int uploadPercent(const QJsonArray& observations) {
 long double weighted=0,total=0;
 for(const auto& value:observations)for(const auto& v:value.toObject()["items"].toArray()) {
  const auto item=v.toObject();if(item["uploadStatus"]!="uploading")continue;
  const auto progress=UploadProgress::fromJson(item["uploadProgress"].toObject());
  if(progress&&progress->phase=="preparing")continue;
  const auto percent=transferPercent(item);if(percent<0)continue;
  const auto weight=qMax(qint64(1),item["artifact"].toObject()["bytes"].toInteger());
  weighted+=static_cast<long double>(weight)*percent;total+=weight;
 }
 return total>0?int(weighted/total):0;
}
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
     text+=" transferred · Source size: "+ByteSize::summary(item["artifact"].toObject()["bytes"].toInteger());
     const auto estimate=transferPercent(item);if(estimate>=0)text+=QString(" · Estimated %1%").arg(estimate);
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
  connect(native_.get(), &NativeTray::notificationActivated,this,&TrayController::openNotificationLink);
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
 tray_ = new QSystemTrayIcon(VisualStyle::icon("tray"), this);
 menu_ = std::make_unique<QMenu>();
 tray_->setToolTip(idleTooltip);
 for (const auto& action : Settings::actionNames()) {
  auto* item = menu_->addAction(VisualStyle::icon(action), action == "clipboard" ? QString("Upload Clipboard") : "Capture " + action);
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
 errorFlashTimer_.setInterval(300);
 errorFlashTimer_.setTimerType(Qt::PreciseTimer);
 errorRecoveryTimer_.setSingleShot(true);
 errorRecoveryTimer_.setInterval(3000);
 errorRecoveryTimer_.setTimerType(Qt::PreciseTimer);
 uploadRevealTimer_.setSingleShot(true);uploadRevealTimer_.setInterval(200);
 uploadRevealTimer_.setTimerType(Qt::PreciseTimer);
 connect(&uploadRevealTimer_,&QTimer::timeout,this,[this] {
  if(operations_.storageActive()&&!errorFlashTimer_.isActive()) {uploadVisible_=true;uploadVisibleClock_.start();}
  refreshActivity();
 });
 uploadCompleteTimer_.setSingleShot(true);uploadCompleteTimer_.setTimerType(Qt::PreciseTimer);
 connect(&uploadCompleteTimer_,&QTimer::timeout,this,[this] {
  uploadVisible_=false;uploadCompleted_=false;uploadVisibleClock_.invalidate();refreshActivity();
 });
 connect(&errorFlashTimer_,&QTimer::timeout,this,[this] {
  errorRed_=!errorRed_;
  refreshActivity();
 });
 connect(&errorRecoveryTimer_,&QTimer::timeout,this,[this] {
  errorFlashTimer_.stop();errorRed_=false;refreshActivity();
 });
 connect(&operations_,&OperationService::operationError,this,&TrayController::flashError);
 connect(&operations_,&OperationService::storageActivityChanged,this,&TrayController::refreshActivity);
 connect(&operations_, &OperationService::observationsChanged, this, &TrayController::refreshActivity);
 refreshActivity();
 connect(&operations_, &OperationService::resultReady, this, [this](const QString& id) {
  operations_.inspect(id, [guard = QPointer<TrayController>(this)](Error error, QJsonObject operation) {
   if (!guard || error) return;
   const auto state = operation["state"].toString();
   bool failed=meaningfulError(operation["error"].toObject())||state=="failed"||state=="partial"||state=="interrupted";
   for(const auto& value:operation["items"].toArray()) {
    const auto item=value.toObject();
    failed=failed||meaningfulError(item["error"].toObject())||meaningfulError(item["local"].toObject()["error"].toObject());
   }
   if(failed&&state!="cancelled")guard->flashError();
   if(!guard->available()||!guard->operations_.settingsSnapshot().notifications)return;
   if ((state == "completed" || state == "partial") && operation["copyStatus"] == "pending")
    return;
   QUrl link;
#ifdef Q_OS_LINUX
   const auto items=operation["items"].toArray();
   if(guard->native_&&items.size()==1) {
    const auto item=OperationItem::fromJson(items.first().toObject());
    if(item.linkAvailable())link=QUrl(item.receipt.directUrl);
   }
#endif
   if (QStringList{"completed", "partial", "failed", "interrupted"}.contains(state))
    guard->notifyMessage("put.gd — " + state,
      "Clipboard: " + operation["copyStatus"].toString() + (link.isEmpty()?". Open recent activity for per-item results.":". Click to open in browser."),
      state != "completed",link);
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
 const auto observations=operations_.activeObservations();
 QSet<QString> activeIds;
 for(const auto& value:observations) {
  const auto op=value.toObject();
  activeIds.insert(op["id"].toString());
  if(!op["destination"].toObject()["providerId"].toString().isEmpty())activeUploads_.insert(op["id"].toString());
 }
 auto finished=activeUploads_;finished.subtract(activeIds);activeUploads_.intersect(activeIds);
 const bool working=operations_.storageActive()||!activeUploads_.isEmpty();
 if(errorFlashTimer_.isActive()) {
  uploadRevealTimer_.stop();uploadCompleteTimer_.stop();
  uploadVisible_=false;uploadCompleted_=false;uploadVisibleClock_.invalidate();
 } else if(working) {
  uploadCompleteTimer_.stop();uploadCompleted_=false;
  if(!uploadVisible_&&operations_.storageActive()&&!uploadRevealTimer_.isActive())uploadRevealTimer_.start();
 } else uploadRevealTimer_.stop();
 for(const auto& id:finished) {
  ++pendingUploadFinishes_;
  operations_.inspect(id,[guard=QPointer<TrayController>(this)](Error error,QJsonObject operation) {
   if(!guard)return;
   --guard->pendingUploadFinishes_;
   if(!error&&operation["state"]=="completed"&&guard->uploadVisible_&&!guard->errorFlashTimer_.isActive()&&
      !guard->operations_.storageActive()&&guard->operations_.activeObservations().isEmpty()) {
    for(const auto& value:operation["items"].toArray())if(value.toObject()["uploadStatus"]=="uploaded") {
     guard->uploadCompleted_=true;break;
    }
   }
   guard->refreshActivity();
  });
 }
 if(uploadVisible_&&!working&&pendingUploadFinishes_==0&&!uploadCompleteTimer_.isActive())
  uploadCompleteTimer_.start(int(qMax(qint64(200),600-uploadVisibleClock_.elapsed())));
 const auto text=uploadCompleted_?QString("Upload complete"):
                 uploadVisible_&&!working?QString("Finishing upload"):activityText(observations);
 const auto color=errorFlashTimer_.isActive()?QColor(errorRed_?"#ffb4ab":"#9bbcff"):
                  QColor(uploadVisible_?"#62d69b":"#9bbcff");
 int percent=-1;
 if(uploadVisible_&&!errorFlashTimer_.isActive()) {
  if(uploadCompleted_)percent=100;
  else if(operations_.storageActive())percent=uploadPercent(observations);
  else percent=iconPercent_;
 }
 if(color==iconColor_&&percent==iconPercent_&&text==activityText_)return;
#ifdef Q_OS_LINUX
 if(native_)native_->setActivity(color,text,percent);
 else
#endif
 if(tray_) {
  if(color!=iconColor_||percent!=iconPercent_)tray_->setIcon(VisualStyle::icon("tray",color,percent));
  if(text!=activityText_)tray_->setToolTip(text.isEmpty()?idleTooltip:QString("put.gd — Working\n")+text);
 }
 iconColor_=color;iconPercent_=percent;activityText_=text;
}
void TrayController::flashError() {
 uploadCompleteTimer_.stop();
 errorRed_=true;errorFlashTimer_.start();errorRecoveryTimer_.start();refreshActivity();
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
#ifdef Q_OS_LINUX
void TrayController::openNotificationLink(const QUrl& link) {
 if(!link.isValid()||(link.scheme()!="https"&&link.scheme()!="http"))return;
 QProcess::startDetached("gio",{"open",link.toString(QUrl::FullyEncoded)});
}
#endif
void TrayController::notifyMessage(const QString& title, const QString& message, bool warning,const QUrl& link) {
 Q_UNUSED(link);
#ifdef Q_OS_LINUX
 if (native_) native_->notify(title, message, warning,link);
 else
#endif
 if (tray_) tray_->showMessage(title, message, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information);
}
void TrayController::notifyFailure(const Error& error) {
 if(error)flashError();
 if (error && available()) notifyMessage("put.gd", error.message, true);
}
}
