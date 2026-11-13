#include "TrayController.h"
#include <QScopeGuard>
#include <QJsonDocument>
#include "app/FileManagerIntegration.h"
#include "Ui.h"
#include "Theme.h"
#include "core/ByteSize.h"
#include "core/Log.h"
#include "platform/HostOpen.h"
#include "core/HostEnvironment.h"
#include "core/NotificationServer.h"
#ifdef Q_OS_LINUX
#include "NativeTray.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#include <QApplication>
#include <QCursor>
#include <QCoreApplication>
#include <QAction>
#include <QActionGroup>
#include <QKeySequence>
#include <QMenu>
#include <QWidgetAction>
#include <QLabel>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QScreen>
#include <QDir>
#include <QHash>
#include <QFileInfo>
#include <QRegularExpression>
#include <utility>
#include <algorithm>

namespace putgd {
namespace {
bool meaningfulError(const QJsonObject& error) {
 const auto code=error["code"].toString();return !code.isEmpty()&&code!="copy_suppressed";
}
constexpr int finishingBoundMs=5000;
const QString idleTooltip = "put.gd";
const QString seasonalTooltip = "put.gd - Happy Holidays!";
// Which way a link or folder opened on the host (#326), never the target itself.
void logOpened(const QString& target,const QJsonObject& result) {
 auto fields=result;fields["target"]=target;
 if(result["opened"].toBool())logging::info(logUi(),"Opened on the host",fields);
 else logging::warning(logUi(),"Could not open on the host",fields);
}
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
 // ": 12 MB of 40 MB (30%) · 2 uploading, 3 waiting, 1 done"
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
// The tooltip's title, its text, the icon's colour and its ring each switch on the
// one Activity, with no default: a new value fails to compile until all four say what it shows.
using Activity=TrayController::Activity;
QString activityTitle(Activity activity,const QString& identity) {
 switch(activity) {
 case Activity::NeedsAttention: return "Needs attention";
 case Activity::Complete: return "Upload complete";
 case Activity::Starting: case Activity::Uploading: case Activity::Finishing: return "Uploading";
 case Activity::Idle: return identity;
 }
 return identity;
}
QString activityStatus(Activity activity,const QString& attentionLabel,const QString& lines) {
 switch(activity) {
 case Activity::NeedsAttention: return attentionLabel+QString(" — open the tray menu to review")+(lines.isEmpty()?QString{}:"\n"+lines);
 case Activity::Complete: return "Upload complete";
 case Activity::Finishing: return "Finishing upload";
 case Activity::Starting: case Activity::Uploading: case Activity::Idle: return lines;
 }
 return lines;
}
// Active work keeps the idle glyph; only a confirmed completion turns green.
QColor activityColor(Activity activity,const Theme::TrayTints& tints) {
 switch(activity) {
 case Activity::NeedsAttention: return tints.error;
 case Activity::Complete: return tints.upload;
 case Activity::Starting: case Activity::Uploading: case Activity::Finishing: case Activity::Idle: return tints.idle;
 }
 return tints.idle;
}
// -2 is the attention badge, -1 no ring. Finishing keeps the ring where it stood;
// so does an upload no storage request is moving at the moment.
int activityPercent(Activity activity,bool storageActive,const QJsonArray& observations,int shown) {
 switch(activity) {
 case Activity::NeedsAttention: return -2;
 case Activity::Complete: case Activity::Starting: case Activity::Idle: return -1;
 case Activity::Uploading: return storageActive?uploadPercent(observations):shown;
 case Activity::Finishing: return shown;
 }
 return -1;
}
}

TrayController::Activity TrayController::activityFor(const ActivityInputs& in) {
 if(in.errorPending)return Activity::NeedsAttention;
 if(in.uploadCompleted)return Activity::Complete;
 if(in.uploadVisible)return in.storageActive||in.uploadsTracked?Activity::Uploading:Activity::Finishing;
 return in.storageActive?Activity::Starting:Activity::Idle;
}

TrayController::TrayController(OperationService& operations,
                               ViewOpener openView,
                               std::function<void()> quit, QObject* parent)
 : QObject(parent), operations_(operations), openView_(std::move(openView)),
   quit_(std::move(quit)) {
 connect(&operations_.scroll(),&ScrollCaptureService::warning,this,[this](const QString&,const QString& message,const QString& reason){
  if(reason=="top_not_reached")notifyMessage("scroll_top_not_reached","put.gd — scrolling capture incomplete",message,true);
  else notifyMessage("scroll_height_cap","put.gd — scrolling capture stopped",message,true);});
 // put.gd's own notices stay out of a scroll capture: those on screen are
 // closed, later ones wait for the capture's end. The capture starts
 // noticeGoneMs after the server has reported them closed (a popup fades
 // out; Omarchy's card takes 200 ms to leave), and no later than that after
 // noticeConfirmMs when the server does not say.
 static constexpr int noticeGoneMs=500,noticeConfirmMs=NativeTray::closeConfirmMs+200;
 connect(&operations_.scroll(),&ScrollCaptureService::preparing,this,[this]{
  holdNotices_=true;
  const auto id=operations_.scroll().activeId();
  const int closed=closeOwnNotifications([this,id]{if(operations_.scroll().activeId()==id)operations_.scroll().holdStart(noticeGoneMs,true);});
  if(closed){
   operations_.scroll().holdStart(noticeConfirmMs+noticeGoneMs);
   logging::info(logUi(),"Notifications closed for a scroll capture",{{"count",closed}});
  }
 });
 connect(&operations_.scroll(),&ScrollCaptureService::ended,this,[this]{
  if(operations_.scroll().active())return;
  holdNotices_=false;
#ifdef Q_OS_LINUX
  for(const auto& n:std::exchange(heldTrayMessages_,{}))sendTrayMessage(n.title,n.message,n.warning,n.actions,n.transient);
#endif
  for(const auto& managers:std::exchange(heldFileManagerNotices_,{}))notifyFileManagerMenu(managers);
  for(const auto& n:std::exchange(heldNotices_,{}))notifyMessage(n.code,n.title,n.message,n.warning,n.link,n.diagnosticsOperation,n.revealPath,n.diagnosticsLabel);
 },Qt::QueuedConnection);
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
   else if(key.startsWith("restart-file-manager-"))restartFileManager(value);
  });
  connect(native_.get(), &NativeTray::menuAboutToShow,this,&TrayController::refreshMenu);
  connect(native_.get(), &NativeTray::availabilityResolved, this, &TrayController::availabilityResolved);
  connect(native_.get(), &NativeTray::actionActivated, this, [this](const QString& action, const QString& token) {
   // Views opened for this click carry the host's token, so Wayland activates them.
   activationToken_ = token;
   const auto clear = qScopeGuard([guard = QPointer<TrayController>(this)] { if (guard) guard->activationToken_.clear(); });
   if (Settings::invocableActionNames().contains(action)) invokeAction(action);
   else if (action.startsWith("delay-")) operations_.setCaptureDelayPreference(action.mid(6).toInt());
   else if (action == "cancel-upload") cancelUploads();
   else if (action == "failed-uploads") acknowledgeFailures();
   else if (action == "edit-clipboard") invokeAction("edit", false, {{"source", "clipboard"}});
   else if (action == "edit-file") showView("editFile");
   else if (action == "capture-settings") showView("captureSettings");
   else if (action == "settings") showSettings();
   else if (action == "destinations") showView("destinations");
   else if (action == "history") showHistory();
   else if (action == "about") showAbout();
   else if (action == "update") showView("updates");
   else if (action == "quit") { const auto quit = quit_; quit(); }
  });
 } else
#endif
 {
 tray_ = new QSystemTrayIcon(Ui::trayIcon({Theme::trayTints(panel_.light()).idle,-1,0,calendar_.glyph()}), this);
 menu_ = std::make_unique<QMenu>();
 tray_->setToolTip(calendar_.glyph()==IconArt::TrayGlyph::Mark?idleTooltip:seasonalTooltip);
 uploadStatus_=menu_->addAction(QString{});uploadStatus_->setObjectName("trayUploadStatus");uploadStatus_->setEnabled(false);uploadStatus_->setProperty("visualRole","information");auto* statusSeparator=menu_->addSeparator();statusSeparator->setObjectName("trayStatusSeparator");cancelUpload_=menu_->addAction("Cancel upload");cancelUpload_->setObjectName("trayCancelUpload");connect(cancelUpload_,&QAction::triggered,this,&TrayController::cancelUploads);uploadSeparator_=menu_->addSeparator();connect(menu_.get(),&QMenu::aboutToShow,this,&TrayController::refreshMenu);
 outputMenu_=menu_->addMenu("Capture output");outputSummary_=outputMenu_->menuAction();outputSummary_->setObjectName("trayOutputSummary");for(const auto& action:Settings::presentationOrder()){auto* entry=new QWidgetAction(outputMenu_);entry->setObjectName("trayOutput"+action);auto* label=new QLabel;label->setTextFormat(Qt::PlainText);label->setWordWrap(true);const auto* screen=outputMenu_->screen();const int available=screen?screen->availableGeometry().width():Ui::scaled(480);label->setMinimumWidth(qMin(Ui::scaled(280),available));label->setMaximumWidth(qMin(Ui::scaled(480),available));label->setContentsMargins(Ui::space(Ui::Space::m),Ui::space(Ui::Space::s),Ui::space(Ui::Space::m),Ui::space(Ui::Space::s));Ui::role(label,"muted");entry->setDefaultWidget(label);connect(entry,&QAction::changed,label,[entry,label]{label->setText(entry->text());});outputMenu_->addAction(entry);}auto* change=menu_->addAction("Change output…");change->setObjectName("trayChangeOutput");connect(change,&QAction::triggered,this,[this]{showView("captureSettings");});menu_->addSeparator();
 // Capture entries first, in the order people reach for them; QAction text
 // treats a single & as a mnemonic marker, so && shows one &.
 const auto capture = [this](const QString& action, const QString& label) {
  auto* entry = menu_->addAction(label);
  entry->setData(action);entry->setProperty("commandLabel",label);
  connect(entry, &QAction::triggered, this, [this, action] { invokeAction(action); });
 };
 const QHash<QString,QString> labels{{"region","Capture region"},{"repeat","Repeat last region"},{"window","Capture window"},{"monitor","Capture monitor"},{"desktop","Capture desktop"},{"edit","Capture region && edit"},{"scroll","Capture scrolling region"},{"text","Copy text from region"},{"record","Record screen"},{"clipboard","Upload clipboard"}};
 for(const auto& action:Settings::shortcutOrder()){
  capture(action,labels.value(action));
  if(action!="desktop")continue;
  // The capture delay (#1363): a radio group that the capture entries above use.
  delayMenu_=menu_->addMenu("Capture delay");delayMenu_->menuAction()->setObjectName("trayCaptureDelay");auto* group=new QActionGroup(delayMenu_);
  for(const int seconds:Settings::captureDelayChoices()){
   auto* choice=delayMenu_->addAction(seconds?QStringLiteral("%1 seconds").arg(seconds):QStringLiteral("None"));choice->setCheckable(true);choice->setData(seconds);choice->setObjectName(QStringLiteral("trayCaptureDelay%1").arg(seconds));group->addAction(choice);
   connect(choice,&QAction::triggered,this,[this,seconds]{operations_.setCaptureDelayPreference(seconds);});
  }
 }
 // Other editor sources stay one level down so the menu does not grow.
 auto* editImage = menu_->addMenu("Edit image");
 editImage->menuAction()->setData("edit-sources");auto* before=menu_->actions().value(menu_->actions().indexOf(editImage->menuAction()));for(auto* entry:menu_->actions())if(entry->data()=="record")before=entry;menu_->insertMenu(before,editImage);
 connect(editImage->addAction("Edit clipboard image"), &QAction::triggered, this, [this] { invokeAction("edit", false, {{"source", "clipboard"}}); });
 connect(editImage->addAction("Edit an image file…"), &QAction::triggered, this, [this] { showView("editFile"); });
 menu_->addSeparator();
 for(const auto& name:QStringList{"History","Destinations","Settings"}){
  auto* entry=menu_->addAction(name);const auto view=name.toLower();entry->setData(view=="settings"?"preferences":view);connect(entry,&QAction::triggered,this,[this,view]{showView(view);});
 }
 failedUploads_=menu_->addAction("Failed uploads");failedUploads_->setObjectName("trayFailedUploads");failedUploads_->setData("failed-uploads");failedUploads_->setVisible(false);connect(failedUploads_,&QAction::triggered,this,&TrayController::acknowledgeFailures);
 updateAction_ = menu_->addAction("Update available");
 updateAction_->setData("update");updateAction_->setVisible(false);
 // Lands on Settings → General → Updates with its action focused, not the first settings page.
 connect(updateAction_, &QAction::triggered, this, [this] { showView("updates"); });
 menu_->addSeparator();
 connect(menu_->addAction("About put.gd"), &QAction::triggered, this, &TrayController::showAbout);
 connect(menu_->addAction("Quit put.gd"), &QAction::triggered, this, [this] {
  const auto quit = quit_;
  quit();
 });
 retintMenu();
 tray_->setContextMenu(menu_.get());
 connect(tray_, &QSystemTrayIcon::activated, this, [this](auto reason) {
  if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
   showHistory();
 });
 // Qt registers a visible icon when a desktop tray becomes available later.
 tray_->show();
 }
 uploadRevealTimer_.setSingleShot(true);uploadRevealTimer_.setInterval(200);
 uploadRevealTimer_.setTimerType(Qt::PreciseTimer);
 connect(&uploadRevealTimer_,&QTimer::timeout,this,[this] {
  if(operations_.storageActive()&&!errorPending_) {uploadVisible_=true;uploadVisibleClock_.start();}
  refreshActivity();
 });
 uploadCompleteTimer_.setSingleShot(true);uploadCompleteTimer_.setTimerType(Qt::PreciseTimer);
 connect(&uploadCompleteTimer_,&QTimer::timeout,this,[this] {
  uploadVisible_=false;uploadCompleted_=false;uploadVisibleClock_.invalidate();refreshActivity();
 });
 // The panel's colour, not the app theme, picks the tint (#60): re-tint the
 // icon and its menu glyphs when the desktop switches.
 connect(&panel_,&PanelTone::changed,this,[this]{iconColor_=QColor();retintMenu();refreshActivity();});
 // The calendar's glyph takes over at local midnight with no restart.
 connect(&calendar_,&TrayCalendar::glyphChanged,this,&TrayController::refreshActivity);
 connect(&operations_,&OperationService::operationError,this,[this](const QString& id,QJsonObject operation){operation["id"]=id;latchFailure({"archive_failed","Local save failed",{}},operation);});
 // Copy text (#1428): one notification after the clipboard has the text. It
 // carries counts only, never what was recognized, and has no action buttons.
 connect(&operations_,&OperationService::textFinished,this,[this](const QString& id,const QJsonObject& record,const QString& notification){
  refreshActivity();
  if(record["state"]=="failed") {
   const auto error=record["error"].toObject();const Error failure{error["code"].toString(),error["message"].toString(),error["details"].toObject()};
   latchFailure(failure,{{"id",id},{"action",Settings::textActionName()},{"state","failed"},{"error",error}});
   if(operations_.settingsSnapshot().notifications)notifyMessage("operation_failed","Text recognition failed",failure.message,true);
   return;
  }
  if(!notification.isEmpty()&&operations_.settingsSnapshot().notifications)notifyMessage("text_"+record["copyStatus"].toString(),"Copy text",notification,false);
 });
 connect(&operations_,&OperationService::storageActivityChanged,this,&TrayController::refreshActivity);
 // Not every tray host asks before it shows the menu (Cinnamon's does not), so the
 // output summary, the shortcuts and the capture delay follow the configuration
 // when it changes, not when the menu opens.
 connect(&operations_,&OperationService::settingsAccepted,this,&TrayController::refreshMenu);
 connect(&operations_, &OperationService::observationsChanged, this, &TrayController::refreshActivity);
 refreshActivity();
 connect(&operations_, &OperationService::resultReady, this, [this](const QString& id) {
  operations_.inspect(id, [guard = QPointer<TrayController>(this)](Error error, QJsonObject operation) {
   if (!guard || error) return;
   for(const auto& value:operation["items"].toArray()){const auto item=value.toObject();if(item["uploadStatus"]=="uploaded")guard->failedItems_.remove(item["artifact"].toObject()["id"].toString());}if(guard->failedItems_.isEmpty()&&guard->attentionLabel_=="Failed uploads")guard->errorPending_=false;
   const auto state = operation["state"].toString();
   bool failed=meaningfulError(operation["error"].toObject())||state=="failed"||state=="partial"||state=="interrupted";
   for(const auto& value:operation["items"].toArray()) {
    const auto item=value.toObject();
    failed=failed||meaningfulError(item["error"].toObject())||meaningfulError(item["local"].toObject()["error"].toObject());
   }
   if(failed&&state!="cancelled"){const auto error=operation["error"].toObject();guard->latchFailure({error["code"].toString(),error["message"].toString(),error["details"].toObject()},operation);}
   guard->refreshActivity();
   // A result is announced whether or not a tray shows (#1105), unless notifications are off.
   if(!guard->operations_.settingsSnapshot().notifications)return;
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
    guard->notifyMessage("operation_" + state,completionTitle(operation),completionMessage(operation,!link.isEmpty()),failed,link,diagnostics,{},diagnosticsActionText(operation));
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
 if(code=="diagnostics_destination_unavailable")return "the destination's connection test failed";
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
QString TrayController::diagnosticsActionText(const QJsonObject& operation) {
 // A failed acquisition has no items and never reached a destination. Local-only
 // captures also have items, so use their upload state rather than their count.
 bool uploaded=false;
 for(const auto& value:operation["items"].toArray()){
  const auto status=value.toObject()["uploadStatus"].toString();
  uploaded|=QStringList{"uploading","uploaded","failed","unknown","conflict","skipped"}.contains(status);
 }
 if(uploaded)return "Save diagnostics for this upload";
 const auto action=operation["action"].toString();
 return QStringList{"region","window","desktop","edit"}.contains(action)?QString("Save diagnostics for this capture"):action=="record"?QString("Save diagnostics for this recording"):QString("Save diagnostics for this operation");
}
QString TrayController::completionTitle(const QJsonObject& operation) {
 const auto items=operation["items"].toArray();bool localOnly=!items.isEmpty(),problem=operation["state"]!="completed"||meaningfulError(operation["error"].toObject()),localFailed=false;
 for(const auto& value:items){const auto item=value.toObject();const auto local=item["local"].toObject();localOnly&=item["uploadStatus"]=="local_only";localFailed|=local["status"]=="failed"||meaningfulError(local["error"].toObject());problem|=item["uploadStatus"]=="failed"||meaningfulError(item["error"].toObject())||localFailed;}
 if(!problem)return localOnly?QString("Saved locally"):QString("Upload complete");
 if((localFailed&&localOnly)||operation["error"].toObject()["code"]=="archive_failed")return "Local save failed";
 return "Completed with a problem";
}
QString TrayController::completionMessage(const QJsonObject& operation,bool hasLink) {
 const auto error=operation["error"].toObject();const auto items=operation["items"].toArray();const auto state=operation["state"].toString();
 if(items.isEmpty()&&meaningfulError(error)&&!error["message"].toString().isEmpty())return error["message"].toString();
 const auto copy=operation["copyStatus"].toString();
 bool localOnly=!items.isEmpty();
 for(const auto& value:items)localOnly=localOnly&&value.toObject()["uploadStatus"]=="local_only";
 // A suppressed copy (#473) is deliberate: the clipboard moved on, or a newer
 // copy took its place, between the capture and the link. Say so, and where
 // the link still is.
 const QString copied=copy=="copied"?(localOnly?"Result copied to the clipboard.":"Link copied to the clipboard."):copy=="failed"?(localOnly?"Could not copy the result.":"Could not copy the link."):
  copy=="suppressed"?(localOnly?"Result not copied: the clipboard changed meanwhile. Copy it from History.":"Link not copied: the clipboard changed during the upload. Copy it from History."):QString{};
 if(!QStringList{"failed","partial","interrupted"}.contains(state)) {
  if(localOnly)return "Saved locally"+(copy=="copied"?QString("; copied to the clipboard."):copy=="failed"?QString("; could not copy the result."):copy=="suppressed"?QString("; not copied because the clipboard changed meanwhile. Copy it from History."):QString("."));
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
 // An uploaded file whose link step did not finish is on the storage: say that,
 // and name the History action its link state offers (#1399).
 int unconfirmed=0,unmade=0;QString linkCause;
 for(const auto& value:items){const auto item=value.toObject();if(item["uploadStatus"]!="uploaded"||state=="interrupted")continue;const auto share=item["shareStatus"].toString();
  if(share=="unknown"||share=="changed")++unconfirmed;
  else if(share=="failed"||(share=="private"&&item["shareIntent"]=="create")){++unmade;if(linkCause.isEmpty()&&share=="failed")linkCause=failureReason(item["error"].toObject());}}
 QString link;
 if(const int links=unconfirmed+unmade){
  const auto subject=items.size()>1?QString(links==1?"1 share link ":"%1 share links ").arg(links):QString("the share link ");
  const auto outcome=unconfirmed&&unmade?QString("were not made or could not be confirmed"):unconfirmed?QString("could not be confirmed"):QString(links==1?"was not made":"were not made");
  const auto action=unconfirmed&&unmade?QString("Inspect link or Create share link"):unconfirmed?QString("Inspect link"):QString("Create share link");
  link=subject+outcome+(links==1&&unmade&&!linkCause.isEmpty()?": "+linkCause:QString{})+". Use "+action+" in History.";
 }
 QString text;
 if(!link.isEmpty()&&uploaded==items.size())text=(items.size()>1?QString("%1 files uploaded, but ").arg(uploaded):QString("Uploaded, but "))+link;
 else if(items.size()>1)text=QString("%1 of %2 files uploaded. ").arg(uploaded).arg(items.size())+QString("%1 failed").arg(items.size()-uploaded)+(cause.isEmpty()?".":": "+cause+".")+(link.isEmpty()?QString{}:" "+link);
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
 // Menu entry IDs name actions, not glyphs: map each one, so an ID without its
 // own IconArt glyph never falls back to the generic file.
 static const QHash<QString,QString> glyphs{{"region","region"},{"edit","edit"},{"edit-sources","edit"},{"window","window"},{"desktop","desktop"},{"scroll","scroll"},{"text","copy-text"},{"record","record"},{"clipboard","clipboard"},{"preferences","preferences"},{"history","history"},{"destinations","destinations"},{"update","download"}};
 for(auto* action:menu_->actions())if(const auto glyph=glyphs.value(action->data().toString());!glyph.isEmpty())action->setIcon(Ui::icon(glyph,tint));
}
QJsonObject TrayController::trayTint()const {
 return {{"panel",panel_.light()?"light":"dark"},{"source",panel_.source()},{"color",Theme::trayTints(panel_.light()).idle.name()}};
}
QJsonObject TrayController::trayIconState()const {
 const QHash<IconArt::TrayGlyph,QString> names{{IconArt::TrayGlyph::Mark,"mark"},{IconArt::TrayGlyph::Turkey,"turkey"},{IconArt::TrayGlyph::Tree,"tree"}};
 return {{"glyph",names.value(calendar_.glyph())},{"shown",names.value(iconRecording_||iconPercent_==-2?IconArt::TrayGlyph::Mark:iconGlyph_)},{"localDate",calendar_.date().toString(Qt::ISODate)},{"calendarOverride",calendar_.overridden()}};
}
void TrayController::cancelUploads() {
 QStringList ids;for(const auto& value:operations_.activeObservations()){
  const auto op=value.toObject();if(op["destination"].toObject()["providerId"].toString().isEmpty()||(op["action"]=="record"&&!recordingHandedOver(op)))continue;ids<<op["id"].toString();
 }
 if(ids.size()>1){showView("historyCancelAll:"+QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(ids)).toJson(QJsonDocument::Compact)));return;}
 if(ids.size()==1)operations_.cancel(ids.first(),[guard=QPointer<TrayController>(this)](Error error,QJsonObject){if(guard&&error)guard->notifyFailure(error);});
}
void TrayController::acknowledgeFailures(){const auto view=attentionView_;failedItems_.clear();errorPending_=false;attentionOperation_.clear();refreshActivity();showView(view);}
void TrayController::refreshMenu() {
 QHash<QString,QString> shortcuts;const auto settings=operations_.settingsSnapshot();const auto shortcutStatus=operations_.shortcutStatus();const auto bindings=shortcutStatus["bindings"].toObject();for(const auto& action:Settings::shortcutOrder()){const auto binding=bindings[action].toObject();if(!shortcutStatus["available"].toBool()||!binding["bound"].toBool()||!binding["error"].toString().isEmpty()||!binding["error"].toObject().isEmpty()||binding["requested"].toString()!=settings.actions.value(action).shortcut)continue;QStringList actual;if(binding["actual"].isString())actual<<binding["actual"].toString();else for(const auto& value:binding["actual"].toArray())actual<<value.toString();actual.removeAll(QString{});if(!actual.isEmpty())shortcuts.insert(action,actual.first());}
 const auto outputName=[&](const QString& action){const auto route=settings.actions.value(action);if(route.destination=="local"||route.profileId.isEmpty())return settings.archive.keepUploads?QString("Local only"):QString("Nothing is saved or uploaded");const auto profile=settings.profiles.value(route.profileId);const bool privateUpload=route.publicationMode=="private"||(route.publicationMode=="default"&&profile["provider"]=="proton-drive"&&!profile["settings"].toObject()["shareByDefault"].toBool(true));return QString(privateUpload?"Private upload":"Shared link")+" · "+profile["label"].toString()+(settings.archive.keepUploads?" + local copy":"");};
 const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"monitor","Capture monitor"},{"desktop","Capture desktop"},{"edit","Capture region & edit"},{"scroll","Capture scrolling region"},{"record","Record screen"},{"clipboard","Upload clipboard"}};QStringList outputs;const auto first=outputName("region");bool mixed=false;for(const auto& action:Settings::presentationOrder()){const auto output=outputName(action);mixed|=output!=first;outputs<<labels.value(action)+": "+output;}const auto output=mixed?QString("Mixed outputs"):first;
 const auto attention=attentionLabel_=="Failed uploads"?QString("Failed uploads (%1)").arg(failedItems_.size()):attentionLabel_;
 int files=0,uploads=0;bool uploading=false;for(const auto& value:operations_.activeObservations()){
  const auto op=value.toObject();if(op["destination"].toObject()["providerId"].toString().isEmpty()||(op["action"]=="record"&&!recordingHandedOver(op)))continue;uploading=true;++uploads;
  for(const auto& v:op["items"].toArray()){const auto item=v.toObject();if(item["uploadStatus"]=="uploading"||item["uploadStatus"]=="pending")++files;}
 }
 QString recording;for(const auto& value:operations_.activeObservations()){const auto op=value.toObject();if(op["action"]=="record"&&op["state"]=="recording")recording=phaseLabel(op);}
 const auto status=!uploading?QString{}:files?QString("Uploading %1 %2…").arg(files).arg(files==1?"file":"files"):QString("Preparing upload…");
#ifdef Q_OS_LINUX
 if(native_){native_->setOutput(output,outputs);native_->setCaptureDelay(settings.captureDelaySeconds);native_->setCommands(shortcuts,status,uploads,errorPending_,recording,attention);return;}
#endif
 if(!menu_)return;
 outputSummary_->setText(QString(output).replace('&',QStringLiteral("&&")));int outputIndex=0;for(auto* entry:outputMenu_->actions())entry->setText(outputs.value(outputIndex++));
 for(auto* entry:menu_->actions())if(entry->property("commandLabel").isValid()){const auto key=QKeySequence(shortcuts.value(entry->data().toString())).toString(QKeySequence::NativeText);entry->setText(entry->property("commandLabel").toString()+(entry->data()=="record"&&!recording.isEmpty()?" — "+recording:QString{})+(key.isEmpty()?QString{}:"\t"+key));}
 // Tab-separated menu text displays a shortcut without registering another key handler.
 if(delayMenu_){const int delay=settings.captureDelaySeconds;delayMenu_->setTitle(delay>0?QStringLiteral("Capture delay: %1 seconds").arg(delay):QStringLiteral("Capture delay: none"));for(auto* choice:delayMenu_->actions())choice->setChecked(choice->data().toInt()==delay);}
 failedUploads_->setProperty("operationId",attentionOperation_);failedUploads_->setText(attention);failedUploads_->setVisible(errorPending_);cancelUpload_->setText(uploads>1?"Cancel all uploads…":"Cancel upload");
 uploadStatus_->setText(status);for(auto* entry:{uploadStatus_,menu_->findChild<QAction*>("trayStatusSeparator"),cancelUpload_,uploadSeparator_})entry->setVisible(uploading);
}
void TrayController::refreshActivity() {
 calendar_.check();
 refreshMenu();
 const auto observations=operations_.activeObservations();
 QSet<QString> activeIds;
 for(const auto& value:observations) {
  const auto op=value.toObject();
  activeIds.insert(op["id"].toString());
  if(!op["destination"].toObject()["providerId"].toString().isEmpty())activeUploads_.insert(op["id"].toString());
 }
 auto finished=activeUploads_;finished.subtract(activeIds);activeUploads_.intersect(activeIds);
 const bool working=operations_.storageActive()||!activeUploads_.isEmpty();
 if(errorPending_) {
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
   if(--guard->pendingUploadFinishes_==0)guard->uploadCompleteTimer_.stop();
   if(!error&&operation["state"]=="completed"&&guard->uploadVisible_&&!guard->errorPending_&&
      !guard->operations_.storageActive()&&guard->operations_.activeObservations().isEmpty()) {
    for(const auto& value:operation["items"].toArray())if(value.toObject()["uploadStatus"]=="uploaded") {
     guard->uploadCompleted_=true;break;
    }
   }
   guard->refreshActivity();
  });
 }
 // "Finishing upload" ends when the finished operations have been inspected. An
 // inspection that never answers (a journal read stuck on a worker) must not leave it
 // showing: the same timer ends it after finishingBoundMs. Each answer restarts the timer.
 if(uploadVisible_&&!working&&!uploadCompleteTimer_.isActive())
  uploadCompleteTimer_.start(pendingUploadFinishes_==0?int(qMax(qint64(200),600-uploadVisibleClock_.elapsed())):finishingBoundMs);
 QString recordingStatus;int recording=0;for(const auto& value:observations){const auto op=value.toObject();if(op["action"]=="record"&&op["state"]=="recording"){recordingStatus=phaseLabel(op);if(op["phase"]=="recording")recording=1;else if(op["phase"]=="paused"&&!recording)recording=2;}}
 auto activity=activityText(observations);if(!recordingStatus.isEmpty()&&!activity.contains(recordingStatus))activity+=(activity.isEmpty()?QString{}:"\n")+recordingStatus;
 const auto shown=activityFor({errorPending_,uploadCompleted_,uploadVisible_,operations_.storageActive(),!activeUploads_.isEmpty()});
 auto text=activityStatus(shown,attentionLabel_,activity);
 const auto color=activityColor(shown,Theme::trayTints(panel_.light()));
 const int percent=activityPercent(shown,operations_.storageActive(),observations,iconPercent_);
 const auto glyph=calendar_.glyph();const bool seasonal=glyph!=IconArt::TrayGlyph::Mark;
 // Only an idle tray makes the greeting its title. Preparation, recording,
 // review and local saving keep their ordinary title above the activity rows.
 const auto& identity=seasonal&&text.isEmpty()?seasonalTooltip:idleTooltip;
 const auto tooltipTitle=activityTitle(shown,identity);
 // Status keeps the title; the greeting stays on as the last line, so the season doesn't hide it.
 if(seasonal&&tooltipTitle!=seasonalTooltip)text=text.isEmpty()||text==tooltipTitle?seasonalTooltip:text+"\n"+seasonalTooltip;
 if(tooltipTitle==activityTitle_&&color==iconColor_&&percent==iconPercent_&&recording==iconRecording_&&text==activityText_&&glyph==iconGlyph_)return;
#ifdef Q_OS_LINUX
 if(native_)native_->setActivity(color,text,percent,recording,tooltipTitle,glyph);
 else
#endif
 if(tray_) {
  if(color!=iconColor_||percent!=iconPercent_||recording!=iconRecording_||glyph!=iconGlyph_)tray_->setIcon(Ui::trayIcon({color,percent,recording,glyph}));
  if(text!=activityText_||tooltipTitle!=activityTitle_)tray_->setToolTip(tooltipTitle+(text.isEmpty()||text==tooltipTitle?QString{}:QString("\n")+text));
 }
 // The tooltip's title and activity lines as the tray now shows them (#250).
 if(text!=activityText_||tooltipTitle!=activityTitle_)logging::debug(logUi(),"Tray tooltip changed",{{"title",tooltipTitle},{"activity",text}});
 iconColor_=color;iconPercent_=percent;iconRecording_=recording;iconGlyph_=glyph;activityText_=text;activityTitle_=tooltipTitle;
}
void TrayController::latchFailure(const Error& error,const QJsonObject& operation) {
 auto cause=error;bool uploadFailed=false,localFailed=error.code=="archive_failed";
 for(const auto& value:operation["items"].toArray()){const auto item=value.toObject();const auto itemError=item["error"].toObject(),local=item["local"].toObject();uploadFailed|=item["uploadStatus"]!="uploaded"&&(item["uploadStatus"]=="failed"||meaningfulError(itemError)||operation["state"]=="interrupted")&&!operation["destination"].toObject()["providerId"].toString().isEmpty();localFailed|=local["status"]=="failed"||meaningfulError(local["error"].toObject());if(!cause&&meaningfulError(itemError))cause={itemError["code"].toString(),itemError["message"].toString(),itemError["details"].toObject()};}
 attentionOperation_=operation["id"].toString();attentionLabel_="Action needs attention";attentionView_="history";
 const bool setup=QStringList{"output_required","profile_not_found","provider_unavailable","invalid_destination","destination_unavailable","s3_settings_invalid","b2_settings_invalid","config_invalid","credential_setup_required"}.contains(cause.code);
 if(uploadFailed){for(const auto& value:operation["items"].toArray()){const auto item=value.toObject();if(item["uploadStatus"]!="uploaded"&&(item["uploadStatus"]=="failed"||item["uploadStatus"]=="unknown"||meaningfulError(item["error"].toObject())||operation["state"]=="interrupted"))failedItems_.insert(item["artifact"].toObject()["id"].toString(operation["id"].toString()));}attentionLabel_="Failed uploads";attentionView_="historyFailed";}
 else if(localFailed||setup){attentionLabel_=localFailed?"Local save needs attention":"Destination needs attention";const QJsonObject recovery{{"action",operation["action"].toString("region")},{"code",localFailed?"archive_failed":cause.code},{"message",localFailed&&cause.message.isEmpty()?"Couldn't save a local copy. Review the local-copy folder.":cause.message}};attentionView_="actionRecovery:"+QString::fromUtf8(QJsonDocument(recovery).toJson(QJsonDocument::Compact));}
 else if(!operation["id"].toString().isEmpty()&&operation["items"].toArray().isEmpty()&&QStringList{"region","window","desktop","edit","record"}.contains(operation["action"].toString()))attentionLabel_="Capture needs attention";
 flashError();
}
void TrayController::flashError() {
 uploadCompleteTimer_.stop();
 errorPending_=true;refreshActivity();
}
void TrayController::activate() { showSettings(); }
void TrayController::showSettings() { showView("settings"); }
void TrayController::showHistory() { showView("history"); }
void TrayController::showAbout() { showView("about"); }
void TrayController::showView(const QString& view) {
 const auto open = openView_;
 open(view, activationToken_);
}
void TrayController::invokeShortcut(const QString& action) {
 // This tray's own open menu holds the keyboard and mouse a selection needs,
 // so close it first. A desktop or window capture keeps it in the picture (#341).
 if (menu_ && menu_->isVisible() && !QStringList{"desktop", "window", "monitor", "repeat"}.contains(action)) menu_->hide();
 invokeAction(action, true);
}
void TrayController::invokeAction(const QString& action, bool fromShortcut, const QJsonObject& options) {
 // The tray opens or raises a recording; its shortcut toggles it.
 auto effective = action == Settings::recordingActionName() && options.isEmpty() ? QJsonObject{{"mode", fromShortcut ? "toggle" : "open"}} : options;
 // A click's token raises the editor that is already open (#657).
 if (action == "edit" && !activationToken_.isEmpty()) effective["activationToken"] = activationToken_;
 // The tray's captures start after the chosen delay (#1363). A shortcut waits
 // only when preferences.captureDelayShortcuts says so, and an edit only when it
 // captures the screen (the clipboard and a file have nothing to wait for).
 if (const auto settings = operations_.settingsSnapshot(); (!fromShortcut || settings.captureDelayShortcuts) && settings.captureDelaySeconds > 0 && Settings::delayableAction(action) && !effective.contains("delaySeconds") && (action != "edit" || Settings::delayableEditSource(effective.value("source").toString("region"))))
  effective["delaySeconds"] = settings.captureDelaySeconds;
 operations_.invoke(action, [guard = QPointer<TrayController>(this), fromShortcut,action](Error error, QJsonObject) {
  if (guard && error) {
   qWarning("Action rejected: %s", qPrintable(error.code));
   // A second Copy text press while one runs: say so and change nothing else.
   if (error.code == "text_busy") { if (guard->operations_.settingsSnapshot().notifications) guard->notifyMessage("text_busy", "Copy text", error.message, false); return; }
   if (fromShortcut) {
    // Re-entry must not map a view over selection or startup recovery.
    if (error.code == "busy_selecting" || error.code == "initializing") return;
    if (guard->shortcutFeedbackClock_.isValid() && guard->lastShortcutError_ == error.code &&
        guard->shortcutFeedbackClock_.elapsed() < 2000) return;
    guard->lastShortcutError_ = error.code;
    guard->shortcutFeedbackClock_.start();
   }
   const bool configuration=QStringList{"output_required","profile_not_found","provider_unavailable","invalid_destination","destination_unavailable","s3_settings_invalid","b2_settings_invalid","config_invalid","archive_failed"}.contains(error.code);
   if(configuration&&!fromShortcut){const QJsonObject recovery{{"action",action},{"code",error.code},{"message",error.message}};guard->showView("actionRecovery:"+QString::fromUtf8(QJsonDocument(recovery).toJson(QJsonDocument::Compact)));}
   else if (!fromShortcut) guard->showHistory();
   if (guard){guard->notifyFailure(error);guard->latchFailure(error,{{"action",action}});}
  }
 }, effective);
}
void TrayController::openLink(const QUrl& link,const QString& target,std::function<void(const QJsonObject&)> done) {
 host::openLink(link,[target,done=std::move(done)](const QJsonObject& result){logOpened(target,result);if(done)done(result);});
}
#ifdef Q_OS_LINUX
void TrayController::openNotificationLink(const QUrl& link) {
 if(!link.isValid()||(link.scheme()!="https"&&link.scheme()!="http"))return;
 openLink(link,"notification-link");
}
#endif
int TrayController::closeOwnNotifications(std::function<void()> gone) {
#ifdef Q_OS_LINUX
 if(native_)return native_->closeNotifications(std::move(gone));
 ++trayNoticeGeneration_;
 auto bus=QDBusConnection::sessionBus();
 const auto open=std::exchange(trayOpen_,{});
 const bool balloon=std::exchange(trayBalloon_,false);
 if(balloon&&tray_){
  // Qt's X11 fallback removes its balloon when its tray icon is hidden.
  const bool visible=tray_->isVisible();tray_->hide();if(visible)tray_->show();disconnect(trayRestartClick_);
 }
 if(!bus.isConnected()&&!balloon)return 0;
 for(const uint id:open)closeTrayNotification(id);
 const int pending=trayMessagesInFlight_+trayClosesInFlight_+int(balloon);
 if(pending){
  const auto id=operations_.scroll().activeId();
  operations_.scroll().holdStart(5000,true);
  auto* waiting=new QTimer(this);waiting->setInterval(50);
  connect(waiting,&QTimer::timeout,this,[this,waiting,id,gone]{
   if(operations_.scroll().activeId()!=id){waiting->stop();waiting->deleteLater();return;}
   if(trayMessagesInFlight_||trayClosesInFlight_){operations_.scroll().holdStart(5000,true);return;}
   waiting->stop();waiting->deleteLater();if(gone)gone();
  });
  waiting->start();
 }
 return pending;
#else
 Q_UNUSED(gone);
 return 0;
#endif
}
void TrayController::notifyMessage(const QString& code, const QString& title, const QString& message, bool warning,const QUrl& link,const QString& diagnosticsOperation,const QString& revealPath,const QString& diagnosticsLabel) {
 Q_UNUSED(link);Q_UNUSED(diagnosticsOperation);Q_UNUSED(revealPath);Q_UNUSED(diagnosticsLabel);
 if(holdNotices_&&operations_.scroll().active()){
  if(heldNotices_.size()<32)heldNotices_.append({code,title,message,warning,link,diagnosticsOperation,revealPath,diagnosticsLabel});
  return;
 }
 // Each toast, so "a toast appeared" can be matched to its cause (#250). The
 // text can name files: diagnostics packages hash it unless --include-names.
 QJsonObject fields{{"notificationId",qint64(++notifications_)},{"summary",code},{"title",title},{"notificationText",message},{"warning",warning}};
 if(!diagnosticsOperation.isEmpty())fields["operationId"]=diagnosticsOperation;
 logging::info(logUi(),"Notification shown",fields);
#ifdef Q_OS_LINUX
 if (native_) {
  QList<NotificationAction> actions;
  if(!diagnosticsOperation.isEmpty())actions.append({"save-diagnostics",diagnosticsLabel.isEmpty()?QString("Save diagnostics for this operation"):diagnosticsLabel,diagnosticsOperation});
  if(!revealPath.isEmpty())actions.append({"show-in-folder","Show in folder",revealPath});
  native_->notify(title, message, warning,link,actions);
 }
 else if (tray_) showTrayMessage(title, message, warning, {});
#else
 if (tray_) tray_->showMessage(title, message, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information);
#endif
}
#ifdef Q_OS_LINUX
// Qt 6.11.2+'s D-Bus tray waits on this thread for Notify's reply. On Plasma X11 the
// server is plasmashell, whose Klipper is meanwhile waiting for this thread to
// hand over the link just copied: both stall until Klipper's clipboard reads
// time out, 10-20 s with every session request unanswered (#597). Send Qt's own
// request without waiting; only a bus with no notification server falls back to
// Qt, whose tray then shows the message itself.
// The urgency is a D-Bus byte: GNOME Shell shows no banner for an int or for 0 (#1105).
void TrayController::closeTrayNotification(uint id,int attempt) {
 if(trayKnownClosed_.contains(id)){trayClosingIds_.removeAll(id);return;}
 if(!trayClosingIds_.contains(id))trayClosingIds_.append(id);
 trayActions_.remove(id);
 auto close=QDBusMessage::createMethodCall("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","CloseNotification");
 close<<id;++trayClosesInFlight_;
 auto* pending=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(close,1000),this);
 connect(pending,&QDBusPendingCallWatcher::finished,this,[this,id,attempt](QDBusPendingCallWatcher* p){
  const QDBusPendingReply<> reply=*p;p->deleteLater();
  if(reply.isError()&&!trayKnownClosed_.contains(id)&&operations_.scroll().active()){
   if(attempt>=2){
    --trayClosesInFlight_;trayClosingIds_.removeAll(id);trayOpen_.append(id);
    operations_.scroll().failPreparation({"notification_close_failed","A put.gd notification could not be closed. Dismiss it and try capture again.",{}});
    return;
   }
   QTimer::singleShot(500,this,[this,id,attempt]{--trayClosesInFlight_;if(operations_.scroll().active())closeTrayNotification(id,attempt+1);else {trayClosingIds_.removeAll(id);if(!trayKnownClosed_.contains(id))trayOpen_.append(id);}});
   return;
  }
  --trayClosesInFlight_;trayClosingIds_.removeAll(id);if(reply.isError()&&!trayKnownClosed_.contains(id))trayOpen_.append(id);
 });
}
void TrayController::trayNotificationClosed(uint id,uint reason) {
 Q_UNUSED(reason);
 trayOpen_.removeAll(id);trayActions_.remove(id);
 if(trayClosingIds_.contains(id)||trayMessagesInFlight_){
  if(!trayKnownClosed_.contains(id))trayKnownClosed_.append(id);
  if(trayKnownClosed_.size()>256)trayKnownClosed_.removeFirst();
 }
}
void TrayController::showTrayMessage(const QString& title, const QString& message, bool warning, const QList<NotificationAction>& actions) {
 if(!trayMessagesInFlight_&&!trayClosesInFlight_)trayKnownClosed_.clear();
 auto bus=QDBusConnection::sessionBus();
 if(!trayClosedConnected_&&bus.isConnected())trayClosedConnected_=bus.connect("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","NotificationClosed",this,SLOT(trayNotificationClosed(uint,uint)));
 // A warning is transient only where the server would restore it after a restart (#1298): ask its name first.
 if(warning&&bus.isConnected()){
  auto* named=new QDBusPendingCallWatcher(bus.asyncCall(QDBusMessage::createMethodCall("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","GetServerInformation")),this);
  connect(named,&QDBusPendingCallWatcher::finished,this,[this,title,message,actions](QDBusPendingCallWatcher* p) {
   p->deleteLater();
   const QDBusPendingReply<QString,QString,QString,QString> server=*p;
   askCapabilities(title,message,true,actions,server.isValid()&&putgd::notifications::failuresTransient(server.argumentAt<0>()));
  });
  return;
 }
 askCapabilities(title,message,warning,actions,false);
}
void TrayController::askCapabilities(const QString& title, const QString& message, bool warning, const QList<NotificationAction>& actions, bool transient) {
 auto bus=QDBusConnection::sessionBus();
 if(actions.isEmpty()||!bus.isConnected()){sendTrayMessage(title,message,warning,actions,transient);return;}
 // A server with no buttons, like UKUI's, would keep the notice up with nothing to press.
 auto* asked=new QDBusPendingCallWatcher(bus.asyncCall(QDBusMessage::createMethodCall("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","GetCapabilities")),this);
 connect(asked,&QDBusPendingCallWatcher::finished,this,[this,title,message,warning,actions,transient](QDBusPendingCallWatcher* p) {
  p->deleteLater();
  const QDBusPendingReply<QStringList> capabilities=*p;
  sendTrayMessage(title,message,warning,capabilities.isValid()&&!capabilities.value().contains("actions")?QList<NotificationAction>{}:actions,transient);
 });
}
void TrayController::sendTrayMessage(const QString& title, const QString& message, bool warning, const QList<NotificationAction>& actions, bool transient) {
 if(holdNotices_){if(heldTrayMessages_.size()<32)heldTrayMessages_.append({title,message,warning,actions,transient});return;}
 static constexpr int balloonMs=30000;
 const auto fallback=[this,tray=QPointer<QSystemTrayIcon>(tray_),title,message,warning,actions]{
  if(!tray||holdNotices_)return;
  trayBalloon_=true;disconnect(trayRestartClick_);
  const auto icon=warning?QSystemTrayIcon::Warning:QSystemTrayIcon::Information;
  if(actions.isEmpty()){tray->showMessage(title,message,icon);return;}
  // No notification server, as on a plain LXDE: Qt's balloon has no buttons, so a click on it restarts.
  // Qt reports a click on the tray icon as a click on the balloon too; that one only dismisses it.
  const auto clicked=trayRestartClick_=connect(tray,&QSystemTrayIcon::messageClicked,this,[tray,name=actions.first().value]{if(tray&&!tray->geometry().contains(QCursor::pos()))restartFileManager(name);},Qt::SingleShotConnection);
  tray->showMessage(title,message+" Click here to restart it.",icon,balloonMs);
  QTimer::singleShot(balloonMs,this,[clicked]{disconnect(clicked);});
 };
 auto bus=QDBusConnection::sessionBus();
 if(!bus.isConnected()){fallback();return;}
 if(trayMessagesInFlight_>=8)return;
 auto call=QDBusMessage::createMethodCall("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","Notify");
 QStringList names;QHash<QString,QString> values;QVariantMap hints{{"urgency",QVariant::fromValue(static_cast<uchar>(warning?2:1))}};
 if(transient)hints.insert("transient",true);
 for(const auto& action:actions){names<<action.key<<action.label;values.insert(action.key,action.value);}
 // Buttons stay until used, as NativeTray's do where the server has no list to keep them in.
 if(!values.isEmpty()){
  hints.insert("resident",true);
  if(!trayActionsConnected_)trayActionsConnected_=bus.connect("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","ActionInvoked",this,SLOT(trayNotificationAction(uint,QString)));
 }
 call.setArguments({QCoreApplication::applicationName(),uint(0),QString(warning?"dialog-warning":"dialog-information"),title,message,names,hints,values.isEmpty()?10000:0});
 const auto generation=trayNoticeGeneration_;
 ++trayMessagesInFlight_;
 auto* pending=new QDBusPendingCallWatcher(bus.asyncCall(call),this);
 connect(pending,&QDBusPendingCallWatcher::finished,this,[this,fallback,values,generation](QDBusPendingCallWatcher* p) {
  --trayMessagesInFlight_;p->deleteLater();
  const QDBusPendingReply<uint> reply=*p;
  if(generation!=trayNoticeGeneration_||holdNotices_){
   if(reply.isValid())closeTrayNotification(reply.value());
   return;
  }
  if(reply.isValid()&&trayKnownClosed_.contains(reply.value()))return;
  if(reply.isValid()&&!values.isEmpty())trayActions_.insert(reply.value(),values);
  if(reply.isValid()){trayOpen_.append(reply.value());if(trayOpen_.size()>16)trayOpen_.removeFirst();}
  const auto error=p->error().type();
  if(error==QDBusError::ServiceUnknown||error==QDBusError::UnknownObject||error==QDBusError::UnknownInterface||error==QDBusError::UnknownMethod)fallback();
 });
}
void TrayController::trayNotificationAction(uint id,const QString& key) {
 if(!trayActions_.value(id).contains(key))return;
 const auto value=trayActions_.take(id).value(key);
 // A resident notification isn't removed by its server when used; put.gd closes it.
 auto close=QDBusMessage::createMethodCall("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","CloseNotification");
 close<<id;QDBusConnection::sessionBus().asyncCall(close);
 if(key.startsWith("restart-file-manager-"))restartFileManager(value);
}
#endif
void TrayController::setUpdateStatus(const QJsonObject& status) {
 const auto result=status["result"].toObject();
 // Stable only; a repository install keeps the item although put.gd does not notify.
 const auto label=status["channel"]=="stable"&&result["updateAvailable"].toBool()?"Update available: "+result["latest"].toString():QString();
 if(label==updateLabel_)return;
 updateLabel_=label;
#ifdef Q_OS_LINUX
 if(native_)native_->setUpdateLabel(label);
#endif
 if(updateAction_){updateAction_->setText(label);updateAction_->setVisible(!label.isEmpty());}
}
void TrayController::notifyUpdate(const QJsonObject& result) {
 if(!available())return;
 notifyMessage("update_available","put.gd — update available","put.gd "+result["latest"].toString()+" is available. Settings → General → Updates has the download.",false,QUrl(result["notesUrl"].toString()));
}
void TrayController::notifyDesktopSetup(const QString& code, const QString& message) {
 notifyMessage(code, "put.gd", message, code != "gnome_login_required" && code != "gnome_extension_updated");
}
void TrayController::notifyFailure(const Error& error) {
 if(error)latchFailure(error);
 if (error) notifyMessage(error.code,"put.gd", error.message, true);
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
namespace {
QString fileManagerName(const QString& name){
 static const QHash<QString,QString> names{{"pcmanfm","PCManFM"},{"pcmanfm-qt","PCManFM-Qt"}};
 return names.value(name,name);
}
}
void TrayController::notifyFileManagerMenu(const QStringList& restartable) {
 if(holdNotices_){heldFileManagerNotices_.append(restartable);return;}
 QString message=QStringLiteral("Upload with put.gd is now in your file manager's right-click menu.");
 if(!restartable.isEmpty()){
  // Short, what matters first: some servers cut a body at about 80 characters.
  // "If it's missing": a file manager that hasn't shown a menu yet needs no restart.
  message=QStringLiteral("Upload with put.gd is in your right-click menu. Restart %1 if it's missing; that closes its windows.").arg(fileManagerName(restartable.first()));
 }
 logging::info(logUi(),"Notification shown",{{"notificationId",qint64(++notifications_)},{"summary","file_manager_menu"},{"title","put.gd"},{"notificationText",message},{"warning",false}});
#ifdef Q_OS_LINUX
 // One button per file manager; each key is unique, its value names the file manager.
 QList<NotificationAction> actions;for(const auto& name:restartable)actions.append({"restart-file-manager-"+name,"Restart "+fileManagerName(name),name});
 if(native_){native_->notify("put.gd",message,false,{},actions);return;}
 if(tray_)showTrayMessage("put.gd",message,false,actions);
#else
 if(tray_)tray_->showMessage("put.gd",message,QSystemTrayIcon::Information);
#endif
}
void TrayController::notifyFileManagerProblem(const QString& message) {notifyMessage("file_manager_menu_problem","put.gd — file manager menu",message,true);}
bool TrayController::restartFileManager(const QString& name) {
 const auto script=FileManagerIntegration::restartScript(name);
 if(script.isEmpty())return false;
 logging::info(logUi(),"File manager restart",{{"fileManager",name}});
 return host::startDetached("/bin/sh",{"-c",script,"putgd-restart",name});
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
 // The file manager selects the package; without one, the portal or gio opens its folder.
 host::showInFolder(file.absoluteFilePath(),[](const QJsonObject& result){logOpened("diagnostics-folder",result);});
 return {};
#else
 return {"diagnostics_reveal_unsupported","Show in folder is not supported on this platform",{}};
#endif
}
}
