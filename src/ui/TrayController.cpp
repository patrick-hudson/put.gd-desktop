#include "TrayController.h"
#include "VisualStyle.h"
#include "Theme.h"
#include "ByteSize.h"
#include "core/Log.h"
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
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#endif
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
// A recording operation is handed over once Upload is clicked: the helper's
// `encoding` export and every upload-pipeline state after it. Before that
// (capture, pause, review, Save copy) the icon keeps its idle colour.
bool recordingHandedOver(const QJsonObject& operation) {
 return operation["action"]=="record"&&(operation["phase"]=="encoding"||operation["state"]!="recording");
}
QString phaseLabel(const QJsonObject& operation) {
 const auto phase=operation["phase"].toString(operation["state"].toString());
 if(operation["action"]=="record"&&operation["state"]=="recording") {
  if(phase=="preparing")return "Setting up the recording";
  if(phase=="ready")return "Ready to record";
  if(phase=="recording")return "Recording";
  if(phase=="paused")return "Recording paused";
  if(phase=="stopping")return "Stopping the recording";
  if(phase=="reviewing")return "Reviewing the recording";
  if(phase=="exporting")return "Saving a copy of the recording";
  if(phase=="encoding")return "Encoding the recording";
 }
 if(phase=="preflight")return "Checking destination";
 if(phase=="saving_local")return "Saving local copy";
 if(phase=="sharing")return "Creating share link";
 if(phase=="reconciling")return "Checking previous upload";
 auto text=phase;text.replace('_',' ');
 return text.isEmpty()?QString("Preparing"):text.left(1).toUpper()+text.mid(1);
}
struct TransferCounts {
 int running=0,waiting=0,done=0,failed=0;qint64 sent=0,total=0;bool known=false;
 void add(const QJsonArray& items) {
  for(const auto& value:items) {
   const auto item=value.toObject();const auto status=item["uploadStatus"].toString();
   if(status=="failed"||status=="unknown")++failed;else if(QStringList{"uploaded","skipped","conflict"}.contains(status))++done;
   else if(item["phase"]=="queued")++waiting;else if(status=="uploading")++running;
   const auto progress=UploadProgress::fromJson(item["uploadProgress"].toObject());
   if(progress&&progress->bytesTransferred){sent+=*progress->bytesTransferred;known=true;}
   if(progress&&progress->bytesTotal)total+=*progress->bytesTotal;
  }
 }
 // ": 12 MiB of 40 MiB (30%) · 2 uploading, 3 waiting, 1 done"
 QString summary() const {
  QString text;
  if(total>0)text=": "+ByteSize::summary(sent)+" of "+ByteSize::summary(total)+QString(" (%1%)").arg(qBound(0,int(100.0*sent/total),100));
  else if(known)text=": "+ByteSize::summary(sent)+" transferred";
  QStringList parts{QString("%1 uploading").arg(running)};
  if(waiting)parts<<QString("%1 waiting").arg(waiting);
  if(done)parts<<QString("%1 done").arg(done);
  if(failed)parts<<QString("%1 failed").arg(failed);
  return text+" · "+parts.join(", ");
 }
};
QString activityLine(const QJsonObject& operation) {
 const auto phase=operation["phase"].toString(operation["state"].toString());
 auto text=phaseLabel(operation);const auto items=operation["items"].toArray();
 if(phase=="uploading"||phase=="finalizing"||phase=="preparing") {
  // Several files transferring at once (#28, #175): the count and the combined
  // bytes, the way History's active row reads.
  TransferCounts counts;counts.add(items);
  if(counts.running>1)return text+counts.summary();
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
 // A recording that ended itself on its byte budget says why (#225).
 connect(&operations_.recording(),&RecordingService::budgetStopped,this,[this](const QString&,const QJsonObject& stop) {
  if(available())notifyMessage("recording_budget_stopped","put.gd — recording stopped",stop["message"].toString(),true);
 });
 // Uploads an automatic cleanup could not abort, once for each (#226).
 connect(&operations_,&OperationService::incompleteUploadsKept,this,[this](const QJsonObject& notice) {
  if(available())notifyMessage("incomplete_uploads_kept","put.gd — incomplete uploads left",notice["message"].toString(),true);
 });
#ifdef Q_OS_LINUX
 if (!qobject_cast<QApplication*>(QCoreApplication::instance())) {
  native_ = std::make_unique<NativeTray>(this);
  connect(native_.get(), &NativeTray::notificationActivated,this,&TrayController::openNotificationLink);
  connect(native_.get(), &NativeTray::notificationAction,this,[this](const QString& key,const QString& value) {
   if(key=="save-diagnostics")saveDiagnostics(value);
   else if(key=="show-in-folder")showSavedPackage(value);
  });
  connect(native_.get(), &NativeTray::availabilityResolved, this, &TrayController::availabilityResolved);
  connect(native_.get(), &NativeTray::actionActivated, this, [this](const QString& action) {
   if (Settings::invocableActionNames().contains(action)) invokeAction(action);
   else if (action == "edit-clipboard") invokeAction("edit", false, {{"source", "clipboard"}});
   else if (action == "edit-file") showView("editFile");
   else if (action == "settings") showSettings();
   else if (action == "history") showHistory();
   else if (action == "activity") showActivity();
   else if (action == "about") showAbout();
   else if (action == "quit") { const auto quit = quit_; quit(); }
  });
 } else
#endif
 {
 tray_ = new QSystemTrayIcon(VisualStyle::icon("tray",Theme::trayTints(panel_.light()).idle), this);
 menu_ = std::make_unique<QMenu>();
 tray_->setToolTip(idleTooltip);
 // Capture entries first, in the order people reach for them; QAction text
 // treats a single & as a mnemonic marker, so && shows one &.
 const auto capture = [this](const QString& action, const QString& label) {
  auto* entry = menu_->addAction(label);
  entry->setData(action);
  connect(entry, &QAction::triggered, this, [this, action] { invokeAction(action); });
 };
 capture("region", "Capture Region");
 capture("edit", "Capture Region && Edit");
 capture("window", "Capture Window");
 capture("desktop", "Capture Desktop");
 capture("record", "Capture Screen Recording");
 // Other editor sources stay one level down so the menu does not grow.
 auto* editImage = menu_->addMenu("Edit Image");
 editImage->menuAction()->setData("edit");
 connect(editImage->addAction("From clipboard"), &QAction::triggered, this, [this] { invokeAction("edit", false, {{"source", "clipboard"}}); });
 connect(editImage->addAction("From file…"), &QAction::triggered, this, [this] { showView("editFile"); });
 capture("clipboard", "Upload Clipboard");
 menu_->addSeparator();
 auto* preferences = menu_->addAction("Settings and setup");
 preferences->setData("preferences");
 connect(preferences, &QAction::triggered, this, &TrayController::showSettings);
 auto* history = menu_->addAction("Upload history");
 history->setData("history");
 connect(history, &QAction::triggered, this, &TrayController::showHistory);
 auto* activity = menu_->addAction("Recent activity");
 activity->setData("activity");
 connect(activity, &QAction::triggered, this, &TrayController::showActivity);
 menu_->addSeparator();
 connect(menu_->addAction("About put.gd"), &QAction::triggered, this, &TrayController::showAbout);
 connect(menu_->addAction("Quit"), &QAction::triggered, this, [this] {
  const auto quit = quit_;
  quit();
 });
 retintMenu();
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
 // The panel's colour, not the app theme, picks the tint (#60): re-tint the
 // icon and its menu glyphs when the desktop switches.
 connect(&panel_,&PanelTone::changed,this,[this]{iconColor_=QColor();retintMenu();refreshActivity();});
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
   // A failed upload offers a diagnostics package for just that operation (#122).
   const auto diagnostics=state!="completed"&&guard->saveDiagnostics_?operation["id"].toString():QString{};
   if (QStringList{"completed", "partial", "failed", "interrupted"}.contains(state))
    guard->notifyMessage("operation_" + state,"put.gd — " + state,completionMessage(operation,!link.isEmpty()),state != "completed",link,diagnostics);
  });
 });
}

QString TrayController::describeActivity(const QJsonArray& observations) {return activityText(observations);}
// What went wrong, in plain words, from an operation or item error code (#152).
QString TrayController::failureReason(const QJsonObject& error) {
 const auto code=error["code"].toString();
 auto has=[&](std::initializer_list<const char*> parts){for(const auto* part:parts)if(code.contains(QLatin1String(part)))return true;return false;};
 if(code=="diagnostics_no_destination")return "no upload destination is set up";
 if(code=="diagnostics_destination_failing")return "the last upload to the destination failed with a credential or configuration error";
 if(code=="diagnostics_destination_unavailable")return "Test connection to the destination failed";
 if(has({"permission_denied","access_denied","multipart_permission"}))return "access denied by the destination";
 if(has({"authentication_failed","credentials_invalid","credential_invalid"}))return "the destination did not accept the saved credentials";
 if(has({"setup_required","account_changed"}))return "the destination needs to be set up again in Settings";
 if(has({"credential_store_unavailable","keychain_unavailable"}))return "the saved credentials could not be read from the keyring";
 if(has({"mfa_required"}))return "the destination requires multi-factor authentication";
 if(has({"bucket_not_found"}))return "the bucket does not exist";
 if(has({"region_mismatch"}))return "the bucket is in a different region than the destination's settings";
 if(has({"acl_incompatible"}))return "the bucket does not accept the destination's access setting";
 if(has({"object_too_large"}))return "the file is larger than the destination accepts";
 if(has({"folder_unavailable"}))return "the destination folder is not available";
 if(has({"timeout","transport"}))return "the destination could not be reached";
 auto message=error["message"].toString().trimmed();
 while(message.endsWith('.'))message.chop(1);
 if(message.isEmpty())return {};
 return message.left(1).toLower()+message.mid(1);
}
QString TrayController::completionMessage(const QJsonObject& operation,bool hasLink) {
 const auto error=operation["error"].toObject();const auto items=operation["items"].toArray();const auto state=operation["state"].toString();
 if(items.isEmpty()&&meaningfulError(error)&&!error["message"].toString().isEmpty())return error["message"].toString();
 const auto copy=operation["copyStatus"].toString();
 bool localOnly=!items.isEmpty();
 for(const auto& value:items)localOnly=localOnly&&value.toObject()["uploadStatus"]=="local_only";
 const QString copied=copy=="copied"?(localOnly?"Result copied to the clipboard.":"Link copied to the clipboard."):copy=="failed"?(localOnly?"Could not copy the result.":"Could not copy the link."):QString{};
 if(!QStringList{"failed","partial","interrupted"}.contains(state)) {
  if(localOnly)return "Saved locally"+(copy=="copied"?QString("; copied to the clipboard."):copy=="failed"?QString("; could not copy the result."):QString("."));
  QStringList parts{items.size()>1?QString("%1 files uploaded.").arg(items.size()):QString("Uploaded.")};
  if(!copied.isEmpty())parts.append(copied);
  if(hasLink)parts.append("Click to open in browser.");
  return parts.join(' ');
 }
 // Failed or partial: the first item error names the failure; the operation's own
 // error covers a failure before any item.
 int failed=0,uploaded=0,savedLocally=0;QJsonObject itemError;bool localFailed=false;
 for(const auto& value:items) {
  const auto item=value.toObject();const auto status=item["uploadStatus"].toString();
  if(status=="uploaded"){++uploaded;continue;}
  const auto local=item["local"].toObject();
  if(local["status"]=="saved")++savedLocally;
  if(status=="failed"||meaningfulError(item["error"].toObject())) {
   ++failed;if(itemError.isEmpty()&&meaningfulError(item["error"].toObject()))itemError=item["error"].toObject();
  } else if(meaningfulError(local["error"].toObject())) {
   ++failed;localFailed=true;if(itemError.isEmpty())itemError=local["error"].toObject();
  }
 }
 const auto cause=failureReason(!itemError.isEmpty()?itemError:meaningfulError(error)?error:QJsonObject{});
 const auto what=localFailed&&!uploaded&&failed==1?QString("Local save failed"):state=="interrupted"?QString("Upload interrupted"):QString("Upload failed");
 QString text;
 if(items.size()>1)text=QString("%1 of %2 files uploaded. ").arg(uploaded).arg(items.size())+QString("%1 failed").arg(items.size()-uploaded)+(cause.isEmpty()?".":": "+cause+".");
 else text=what+(cause.isEmpty()?".":": "+cause+".");
 if(savedLocally)text+=savedLocally==1?" A local copy was saved.":QString(" %1 local copies were saved.").arg(savedLocally);
 if(!copied.isEmpty())text+=" "+copied;
 if(hasLink)text+=" Click to open in browser.";
 return text;
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
void TrayController::retintMenu() {
 if(!menu_)return;
 const auto tint=Theme::trayTints(panel_.light()).idle;
 for(auto* action:menu_->actions())if(!action->data().toString().isEmpty())action->setIcon(VisualStyle::icon(action->data().toString(),tint));
}
QJsonObject TrayController::trayTint()const {
 return {{"panel",panel_.light()?"light":"dark"},{"source",panel_.source()},{"color",Theme::trayTints(panel_.light()).idle.name()}};
}
void TrayController::refreshActivity() {
 const auto observations=operations_.activeObservations();
 QSet<QString> activeIds;bool encoding=false;
 for(const auto& value:observations) {
  const auto op=value.toObject();
  activeIds.insert(op["id"].toString());
  encoding=encoding||recordingHandedOver(op);
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
 // Amber covers encoding and saving after Upload until storage traffic reveals the green fill.
 const auto& tints=Theme::trayTints(panel_.light());
 const auto color=errorFlashTimer_.isActive()?(errorRed_?tints.error:tints.idle):
                  uploadVisible_?tints.upload:encoding?tints.encoding:tints.idle;
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
 // The tooltip's title and activity lines as the tray now shows them (#250).
 if(text!=activityText_)logging::debug(logUi(),"Tray tooltip changed",{{"title",text.isEmpty()?idleTooltip:QString("put.gd — Working")},{"activity",text}});
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
void TrayController::invokeAction(const QString& action, bool fromShortcut, const QJsonObject& options) {
 // The tray opens or raises a recording; its shortcut toggles it.
 const auto effective = action == Settings::recordingActionName() && options.isEmpty() ? QJsonObject{{"mode", fromShortcut ? "toggle" : "open"}} : options;
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
 }, effective);
}
#ifdef Q_OS_LINUX
void TrayController::openNotificationLink(const QUrl& link) {
 if(!link.isValid()||(link.scheme()!="https"&&link.scheme()!="http"))return;
 QProcess::startDetached("gio",{"open",link.toString(QUrl::FullyEncoded)});
}
#endif
void TrayController::notifyMessage(const QString& code, const QString& title, const QString& message, bool warning,const QUrl& link,const QString& diagnosticsOperation,const QString& revealPath) {
 Q_UNUSED(link);Q_UNUSED(diagnosticsOperation);Q_UNUSED(revealPath);
 // Each toast, so "a toast appeared" can be matched to its cause (#250). The
 // text can name files: diagnostics packages hash it unless --include-names.
 QJsonObject fields{{"notificationId",qint64(++notifications_)},{"summary",code},{"title",title},{"notificationText",message},{"warning",warning}};
 if(!diagnosticsOperation.isEmpty())fields["operationId"]=diagnosticsOperation;
 logging::info(logUi(),"Notification shown",fields);
#ifdef Q_OS_LINUX
 if (native_) {
  QList<NotificationAction> actions;
  if(!diagnosticsOperation.isEmpty())actions.append({"save-diagnostics","Save diagnostics for this upload",diagnosticsOperation});
  if(!revealPath.isEmpty())actions.append({"show-in-folder","Show in folder",revealPath});
  native_->notify(title, message, warning,link,actions);
 }
 else
#endif
 if (tray_) tray_->showMessage(title, message, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information);
}
void TrayController::notifyFailure(const Error& error) {
 if(error)flashError();
 if (error && available()) notifyMessage(error.code,"put.gd", error.message, true);
}
void TrayController::saveDiagnostics(const QString& operationId) {
 if(!saveDiagnostics_||operationId.isEmpty())return;
 saveDiagnostics_(operationId,[guard=QPointer<TrayController>(this)](Error error,QJsonObject result) {
  if(!guard)return;
  if(error)guard->notifyMessage("diagnostics_not_saved","put.gd — diagnostics not saved",error.message,true);
  else guard->notifyDiagnosticsSaved(result);
 });
}
void TrayController::notifyDiagnosticsSaved(const QJsonObject& result) {
 const QFileInfo file(result["path"].toString());if(file.fileName().isEmpty())return;
 // With diagnostics.bundle upload (#123, #158) it also says where the package went.
 QString uploaded=" Nothing was uploaded.";
 const auto uploadError=result["uploadError"].toObject();
 if(result.contains("link"))uploaded=" Uploaded to "+result["upload"].toObject()["label"].toString()+(result["linkCopied"].toBool()?"; link copied.":".");
 else if(uploadError["code"]=="diagnostics_link_unavailable")uploaded=" Uploaded to "+result["upload"].toObject()["label"].toString()+", which gives it no link.";
 else if(!uploadError.isEmpty()){const auto reason=failureReason(uploadError);uploaded=" Not uploaded"+(reason.isEmpty()?QString("."):": "+reason+".");}
 notifyMessage("diagnostics_saved","put.gd — diagnostics saved",file.fileName()+" ("+ByteSize::summary(result["bytes"].toInteger())+") in "+QDir::toNativeSeparators(file.absolutePath())+"."+uploaded,false,{},{},file.absoluteFilePath());
}
// The button carries the path put.gd itself just saved, whatever its name (#223).
void TrayController::showSavedPackage(const QString& path) {
 const auto error=revealPackage(path,true);if(!error)return;
 qWarning("Show in folder refused: %s",qPrintable(error.code));
 notifyMessage("show_in_folder_refused","put.gd — Show in folder",error.message,true);
}
Error TrayController::revealPackage(const QString& path,bool savedHere) {
 static const QRegularExpression name("^putgd-diagnostics-[0-9]{8}-[0-9]{6}Z(-[0-9]+)?\\.zip$");
 const QFileInfo file(path);
 if(!QDir::isAbsolutePath(path)||(!savedHere&&!name.match(file.fileName()).hasMatch()))return {"invalid_request","Show in folder takes the absolute path of a saved put.gd diagnostics package",{}};
 if(!file.isFile()||file.isSymLink())return {"diagnostics_package_missing","The diagnostics package is no longer there",{{"path",path}}};
#ifdef Q_OS_LINUX
 // The file manager selects the package; without one, open its folder instead.
 auto call=QDBusMessage::createMethodCall("org.freedesktop.FileManager1","/org/freedesktop/FileManager1","org.freedesktop.FileManager1","ShowItems");
 call.setArguments({QStringList{QUrl::fromLocalFile(file.absoluteFilePath()).toString(QUrl::FullyEncoded)},QString{}});
 const auto folder=file.absolutePath();
 auto* watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call,5000));
 QObject::connect(watcher,&QDBusPendingCallWatcher::finished,[watcher,folder] {
  if(watcher->isError()){qWarning("FileManager1.ShowItems failed: %s; opening the folder",qPrintable(watcher->error().name()));QProcess::startDetached("gio",{"open",QUrl::fromLocalFile(folder).toString(QUrl::FullyEncoded)});}
  watcher->deleteLater();
 });
 return {};
#else
 return {"diagnostics_reveal_unsupported","Show in folder is not supported on this platform",{}};
#endif
}
}
