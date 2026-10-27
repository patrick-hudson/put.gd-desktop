#pragma once
#include "app/OperationService.h"
#include "PanelTone.h"
#include <QObject>
#include <QElapsedTimer>
#include <QColor>
#include <QTimer>
#include <QSet>
#include <QUrl>
#include <functional>
#include <memory>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace putgd {
#ifdef Q_OS_LINUX
class NativeTray;
#endif
class TrayController final : public QObject {
 Q_OBJECT
public:
 TrayController(OperationService&, std::function<void(const QString&)> openView,
                std::function<void()> quit, QObject* parent = nullptr);
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
 static QString completionMessage(const QJsonObject& operation,bool hasLink);
 // A provider or operation error as a short lower-case phrase.
 static QString failureReason(const QJsonObject& error);
 // The tooltip's activity lines for OperationService::activeObservations().
 static QString describeActivity(const QJsonArray& observations);
 void invokeShortcut(const QString&);
 // Builds a diagnostics package for one operation (#122); main wires it to
 // DiagnosticsBundles. Failure notifications offer it when it is set.
 void setDiagnosticsSaver(std::function<void(const QString& operationId,JsonReply)> saver) {saveDiagnostics_=std::move(saver);}
 void saveDiagnostics(const QString& operationId);
 // "Diagnostics package saved" with a Show in folder button.
 void notifyDiagnosticsSaved(const QJsonObject& bundleResult);
 // The update check's state (#293): a stable update shows a menu entry that
 // opens Settings; nightly updates show only in Settings.
 void setUpdateStatus(const QJsonObject& status);
 // "put.gd X is available", once per version; main wires UpdateService to it.
 void notifyUpdate(const QJsonObject& result);
 QString updateLabel() const {return updateLabel_;}
 // The panel tone the tray tint follows, and which input decided it (#60).
 QJsonObject trayTint()const;
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
#endif
 QString lastShortcutError_;
 QElapsedTimer shortcutFeedbackClock_;
 // The Show in folder button: a refusal is logged and said, never silent.
 void showSavedPackage(const QString& path);
 // `code` names the notification in the log, which records each one (#250).
 void notifyMessage(const QString& code, const QString& title, const QString& message, bool warning, const QUrl& link = {}, const QString& diagnosticsOperation = {}, const QString& revealPath = {});
 quint64 notifications_ = 0;
 std::function<void(const QString&,JsonReply)> saveDiagnostics_;
 void refreshActivity();void refreshMenu();void cancelUploads();
 void flashError();void acknowledgeFailures();
 void latchFailure(const Error&,const QJsonObject& operation = {});
 QString attentionLabel_="Action needs attention",attentionView_="history",attentionOperation_;
 QTimer uploadRevealTimer_,uploadCompleteTimer_;
 QElapsedTimer uploadVisibleClock_;
 bool uploadVisible_=false,uploadCompleted_=false;
 int pendingUploadFinishes_=0;
 bool errorPending_=false;
 // Unset until the first refresh, which tints for the panel.
 QColor iconColor_;
 PanelTone panel_;
 void retintMenu();
 int iconPercent_=-1;
 int iconRecording_=0;
 QString activityText_,activityTitle_="put.gd";
 QAction *uploadStatus_=nullptr,*cancelUpload_=nullptr,*uploadSeparator_=nullptr,*failedUploads_=nullptr;
 QSet<QString> activeUploads_;
 void showAbout();
 void showView(const QString&);
 OperationService& operations_;
 std::function<void(const QString&)> openView_;
 std::function<void()> quit_;
 QSystemTrayIcon* tray_ = nullptr;
#ifdef Q_OS_LINUX
 std::unique_ptr<NativeTray> native_;
#endif
 std::unique_ptr<QMenu> menu_;
 QAction* updateAction_ = nullptr;
 QString updateLabel_;
};
}
