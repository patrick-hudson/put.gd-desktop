#pragma once
#include "app/UiControl.h"
#include "UiSession.h"
#include "Appearance.h"
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QList>
#include <memory>
class QWidget;
namespace putgd {
class MainWindow;class AboutWindow;
class UiApplicationController final:public QObject {
 Q_OBJECT
public:
 UiApplicationController(UiSession&,UiControl&,QJsonObject state,QObject* parent=nullptr);
 ~UiApplicationController() override;
signals:
 void exitRequested();
protected:
 bool eventFilter(QObject*,QEvent*)override;
private:
 QWidget* view(const QString&);
 void receive(const QString&,const QString&,const QJsonObject&);
 QJsonObject snapshot()const;
 void finishShortcutRecordingRequests(Error);
 void clearPendingAction(bool restore);void requestAction(const QString&);void requestEdit(const QString& source,const QString& path);void prepareAction();void invokeAction();
 void requestQuit();void requestClose(QWidget*);void submitClose();void checkpoint();void notify(const Error&);void allowClose();
 UiSession& session_;UiControl& control_;QJsonObject saved_;
 // The colour scheme in use: the appearance setting, or the desktop's (#60).
 Appearance appearance_;
 // A theme picked in Settings and shown before the resident accepts it (#237).
 QString themePreview_;
 void followAppearance();
 std::unique_ptr<MainWindow> main_;
 std::unique_ptr<AboutWindow> about_;
 struct RecordingRequest {bool active;std::function<void(Error)> reply;};
 QHash<QString,RecordingRequest> recordingRequests_;
 QTimer recordingDeadline_;
 QTimer checkpointDelay_,closeDeadline_,quitDelay_,actionDelay_,captureDelay_;
 QString pendingAction_;QJsonObject pendingOptions_;QList<QPointer<QWidget>> actionWindows_;
 QString checkpointSequence_,closeSequence_,closeCancelSequence_;
 QPointer<QWidget> closing_;
 QSet<QWidget*> openViews_;
 bool dirtyCheckpoint_=false,exiting_=false,quitting_=false,actionDispatching_=false;
};
}
