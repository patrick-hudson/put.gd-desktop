#pragma once
#include "app/OperationService.h"
#include "PanelTone.h"
#include "TrayCalendar.h"
#include <QObject>
#include <QElapsedTimer>
#include <QColor>
#include <QTimer>
#include <QSet>
#include <QHash>
#include <QUrl>
#include <QJsonArray>
#include <functional>
#include <memory>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace putgd {
#ifdef Q_OS_LINUX
class NativeTray;
struct NotificationAction;
#endif
class TrayController final : public QObject {
 Q_OBJECT
public:
 // openView(view, activationToken): the token is the tray host's xdg-activation
 // token for the click that asked for the view, or empty (#657).
 using ViewOpener = std::function<void(const QString& view, const QString& activationToken)>;
 TrayController(OperationService&, ViewOpener openView,
                std::function<void()> quit, QObject* parent = nullptr);
 TrayController(OperationService& operations, std::function<void(const QString&)> openView,
                std::function<void()> quit, QObject* parent = nullptr)
  : TrayController(operations, ViewOpener([open = std::move(openView)](const QString& view, const QString&) { open(view); }), std::move(quit), parent) {}
 ~TrayController() override;
 bool available() const;
 bool availabilityKnown() const;
 void activate();
 void showSettings();
 void showHistory();
 void notifyFailure(const Error&);
 // Completion toast text: the operation's own error when it failed before any
 // item; for a failed or partial upload, the failure in plain words, whether a
 // local copy was kept and what was copied (#152).
 static QString completionTitle(const QJsonObject& operation);
 static QString diagnosticsActionText(const QJsonObject& operation);
 static QString completionMessage(const QJsonObject& operation,bool hasLink);
 // A provider or operation error as a short lower-case phrase.
 static QString failureReason(const QJsonObject& error);
 // What the tray icon and tooltip are showing, as one value. The tooltip's title
 // and text, the icon's colour, its fill and its badge each switch on it.
 // Capturing, Processing and Uploading are working stages, in pipeline order:
 // a hotkey or tray action is taking its picture; the capture is being edited,
 // encoded, saved or read; bytes are going to the destination. Complete and
 // Failed show briefly after the work ends, then the icon returns to Idle.
 enum class Activity {Idle,Capturing,Processing,Uploading,Complete,Failed};
 // The working stage one active operation observation has reached; Idle when it
 // shows none (a recording before Upload is clicked has its own badge).
 static Activity stageOf(const QJsonObject& observation);
 struct ActivityInputs {
  // The furthest working stage across active operations and a press not yet answered.
  Activity working=Activity::Idle;
  // A failure or a completion still within its time on the icon.
  bool failure=false,complete=false;
 };
 // New work always shows: it hides a failure or completion still on the icon.
 static Activity activityFor(const ActivityInputs&);
 // Each working stage stays at least this long, so a short pipeline reads as
 // Capturing, Processing, Uploading, Complete instead of skipping a stage.
 static constexpr int stageDwellMs=250,completeMs=1500,failureMs=8000;
 void invokeShortcut(const QString&);
 // Builds a diagnostics package for one operation (#122); main wires it to
 // DiagnosticsBundles. Failure notifications offer it when it is set.
 void setDiagnosticsSaver(std::function<void(const QString& operationId,JsonReply)> saver) {saveDiagnostics_=std::move(saver);}
 void saveDiagnostics(const QString& operationId);
 // "Diagnostics package saved" with a Show in folder button.
 void notifyDiagnosticsSaved(const QJsonObject& bundleResult);
 // "Upload with put.gd" in file managers (#1105): the AppImage's notice once per install,
 // naming the session's file manager if it may need a restart, with a button that
 // restarts it (closing its windows, which the notice says first).
 void notifyFileManagerMenu(const QStringList& restartable);
 // A desktop setup step the user has to take (GNOME: log out once, or the reason).
 void notifyDesktopSetup(const QString& code, const QString& message);
 // A problem setting the menu up, e.g. COSMIC's action list could not be read.
 void notifyFileManagerProblem(const QString& message);
 // Quits a file manager FileManagerIntegration::restartScript knows; a PCManFM
 // drawing the desktop starts again in the background. False for other names.
 static bool restartFileManager(const QString& name);
 // The update check's state (#293): a stable update shows a menu entry that
 // opens Settings; nightly updates show only in Settings.
 void setUpdateStatus(const QJsonObject& status);
 // "put.gd X is available", once per version; main wires UpdateService to it.
 void notifyUpdate(const QJsonObject& result);
 QString updateLabel() const {return updateLabel_;}
 // The panel tone the tray tint follows, and which input decided it (#60).
 QJsonObject trayTint()const;
 // The glyph the calendar picked, its local date, and whether a lab override set it.
 QJsonObject trayIconState()const;
 // The StatusNotifier menu's host activity and rows (NativeTray::menuActivity()); {"native": false} for the Qt fallback tray.
 QJsonObject trayMenuState()const;
 // Opens the file manager with a saved package selected (FileManager1.ShowItems),
 // or its folder when no file manager answers. Only put.gd's own package names,
 // unless savedHere: diagnostics.bundle wrote that path in this resident (#223).
 static Error revealPackage(const QString& path,bool savedHere=false);
 // An http(s) link in the default browser by host::openLink, logged as target (#326).
 static void openLink(const QUrl& link,const QString& target,std::function<void(const QJsonObject&)> done={});

signals:
 void availabilityResolved();

private:
 void invokeAction(const QString&, bool fromShortcut = false, const QJsonObject& options = {});
#ifdef Q_OS_LINUX
 void openNotificationLink(const QUrl&);
 // The graphical resident's toast, sent without waiting for the server (#597).
 void showTrayMessage(const QString& title, const QString& message, bool warning, const QList<NotificationAction>& actions);
 void askCapabilities(const QString& title, const QString& message, bool warning, const QList<NotificationAction>& actions, bool transient);
 void sendTrayMessage(const QString& title, const QString& message, bool warning, const QList<NotificationAction>& actions, bool transient);
 struct HeldTrayMessage { QString title,message; bool warning; QList<NotificationAction> actions; bool transient; };
 QList<HeldTrayMessage> heldTrayMessages_;
 void closeTrayNotification(uint id,int attempt=0);
 int trayMessagesInFlight_ = 0, trayClosesInFlight_ = 0;
 // Its buttons, by notification id: the server shows them and reports the click.
 QHash<uint,QHash<QString,QString>> trayActions_;
 bool trayBalloon_ = false;
 quint64 trayNoticeGeneration_ = 0;
 QList<uint> trayOpen_; // the newest few it sent, for closeOwnNotifications()
 bool trayActionsConnected_ = false, trayClosedConnected_ = false;
 QList<uint> trayKnownClosed_,trayClosingIds_;
 Q_SLOT void trayNotificationClosed(uint id,uint reason);
 QMetaObject::Connection trayRestartClick_; // Qt's balloon has one click signal, whichever balloon is showing
 Q_SLOT void trayNotificationAction(uint id, const QString& key);
#endif
 QString lastShortcutError_;
 QElapsedTimer shortcutFeedbackClock_;
 // The Show in folder button: a refusal is logged and said, never silent.
 void showSavedPackage(const QString& path);
 // `code` names the notification in the log, which records each one (#250).
 void notifyMessage(const QString& code, const QString& title, const QString& message, bool warning, const QUrl& link = {}, const QString& diagnosticsOperation = {}, const QString& revealPath = {}, const QString& diagnosticsLabel = {});
 quint64 notifications_ = 0;
 // A scroll capture reads the screen for seconds, and a notice of put.gd's own
 // on it ends up in the picture (#1376; seen on Plasma and GNOME). When one is
 // prepared, the notices put.gd has open are closed and the capture is held a
 // moment while they go; notices that come up while it runs are kept back
 // and shown when it has ended. Other applications' notices are not put.gd's to close.
 struct HeldNotice { QString code, title, message; bool warning = false; QUrl link; QString diagnosticsOperation, revealPath, diagnosticsLabel; };
 QList<HeldNotice> heldNotices_;
 QList<QStringList> heldFileManagerNotices_;
 bool holdNotices_ = false;
 // `gone`: see NativeTray::closeNotifications.
 int closeOwnNotifications(std::function<void()> gone={});
 std::function<void(const QString&,JsonReply)> saveDiagnostics_;
 void refreshActivity();void refreshMenu();void cancelUploads();
 void flashError();void acknowledgeFailures();
 void latchFailure(const Error&,const QJsonObject& operation = {});
 // Sets shown_ and records the change for diagnostics.
 void showStage(Activity stage);
 QString attentionLabel_="Action needs attention",attentionView_="history",attentionOperation_;
 // errorPending_ keeps the menu's attention row until it is reviewed; the icon's
 // failure badge is failureShown_, which clears after failureMs or with new work.
 bool errorPending_=false,failureShown_=false,completeShown_=false,completeUploaded_=false;
 // Hotkey or tray presses whose operations the service has not yet answered.
 int startPending_=0;
 QTimer startTimer_,dwellTimer_,completeTimer_,failureTimer_,finishingTimer_;
 Activity shown_=Activity::Idle;
 // The furthest working stage the current run has reached, so a stage that
 // passed between two refreshes still shows for its dwell.
 Activity reached_=Activity::Idle;
 QElapsedTimer shownClock_;
 // The last few stage changes, for diagnostics.trayIcon (lab checks the sequence).
 QJsonArray stageHistory_;
 // Each active operation's furthest stage (an operation never shows a stage it
 // has passed, so a copy after the upload stays green), and finished ones still
 // being inspected for Complete.
 QHash<QString,Activity> opStages_;
 // Operations whose live attempt is a Retry local save.
 QSet<QString> localRetries_;
 int pendingFinishes_=0;
 // Unset until the first refresh, which tints for the panel.
 QColor iconColor_;
 PanelTone panel_;
 void retintMenu();
 int iconPercent_=-1;
 int iconRecording_=0;
 bool iconDone_=false;
 TrayCalendar calendar_;
 IconArt::TrayGlyph iconGlyph_=IconArt::TrayGlyph::Mark;
 QString activityText_,activityTitle_="put.gd";
 QSet<QString> failedItems_;
 // The fallback QMenu's rows by native menu ID (TrayMenu), for refreshMenu.
 QHash<int,QAction*> menuRows_;
 // A tray row's command, from either tray: an action ID or a TrayMenu command.
 void runCommand(const QString& action);
 void showView(const QString&);
 OperationService& operations_;
 ViewOpener openView_;
 // The host's token while a native tray click is dispatched; empty otherwise.
 QString activationToken_;
 std::function<void()> quit_;
 QSystemTrayIcon* tray_ = nullptr;
#ifdef Q_OS_LINUX
 std::unique_ptr<NativeTray> native_;
#endif
 std::unique_ptr<QMenu> menu_;
 QString updateLabel_;
};
}
