#pragma once
#include "app/UiControl.h"
#include "UiSession.h"
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <memory>
class QWidget;
namespace putgd {
class SettingsWindow;class HistoryWindow;class RecentOperations;class AboutWindow;
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
 void requestClose(QWidget*);void submitClose();void checkpoint();void notify(const Error&);void allowClose();
 UiSession& session_;UiControl& control_;QJsonObject saved_;
 std::unique_ptr<SettingsWindow> settings_;
 std::unique_ptr<HistoryWindow> history_;
 std::unique_ptr<RecentOperations> activity_;
 std::unique_ptr<AboutWindow> about_;
 struct RecordingRequest {bool active;std::function<void(Error)> reply;};
 QHash<QString,RecordingRequest> recordingRequests_;
 QTimer recordingDeadline_;
 QTimer checkpointDelay_,closeDeadline_;
 QString checkpointSequence_,closeSequence_,closeCancelSequence_;
 QPointer<QWidget> closing_;
 QSet<QWidget*> openViews_;
 bool dirtyCheckpoint_=false,exiting_=false;
};
}
