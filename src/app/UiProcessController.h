#pragma once
#include "UiControl.h"
#include "core/Log.h"
#include <QHash>
#include <QProcess>
#include <QTimer>
#include <functional>
namespace putgd {
class UiProcessController final:public QObject {
 Q_OBJECT
public:
 using Reply=std::function<void(Error)>;
 using Validator=std::function<Error(const QJsonObject&)>;
 UiProcessController(QString socketPath,QString residentEpoch,QString helperPath,Validator,QObject* parent=nullptr);
 ~UiProcessController() override;
 using ShortcutRecordingHandler=std::function<void(bool,Reply)>;
 void setShortcutRecordingHandler(ShortcutRecordingHandler handler){shortcutRecordingHandler_=std::move(handler);}
 void activate(const QString& view,Reply reply={});
 // Forwards one command to an open view; the helper validates and answers it.
 using CommandReply=std::function<void(Error,QJsonObject)>;
 void command(const QString& name,const QJsonObject& params,CommandReply reply);
 void invalidate();
 void shutdown(std::function<void()> finished={});
 QJsonObject status()const;
 QJsonObject viewState()const;
signals:
 void failure(putgd::Error error);
 void helperPidChanged(qint64 pid);
private:
 enum class Phase {Stopped,Starting,Ready,Closing,Stopping,Failed};
 struct Activation {QString view;QList<Reply> replies;};
 void launch();void flushActivations();void sendInvalidation();
 void receive(const QString& type,const QString& sequence,const QJsonObject& payload);
 void fail(Error);void failActivations(Error);void failCommands(const Error&);void finished(QProcess*,quint64,int,QProcess::ExitStatus);
 void updateShortcutRecording();void restoreShortcuts();
 Error storeState(const QJsonObject&);void completeShutdown();
 struct RecordingRequest {QString sequence;bool active;};
 QList<RecordingRequest> recordingRequests_;
 ShortcutRecordingHandler shortcutRecordingHandler_;
 bool shortcutRecording_=false,recordingRequestActive_=false;
 quint64 recordingGeneration_=0;
 QString socketPath_,epoch_,helperPath_;Validator validate_;
 QProcess* process_=nullptr;UiControl* control_=nullptr;
 QTimer startup_,stopDeadline_,activationDeadline_,closeDeadline_;
 Phase phase_=Phase::Stopped;
 QHash<QString,QList<Reply>> pending_;
 QHash<QString,Activation> activations_;
 QHash<QString,CommandReply> commands_;QTimer commandDeadline_;
 QJsonObject state_,lastExit_;
 HelperOutput output_{"putgd-ui",logUi()};
 qsizetype stateBytes_=0;
 quint64 processGeneration_=0,invalidationGeneration_=0,sentInvalidation_=0;
 QString invalidationSequence_,closeSequence_;
 bool closeConfirmed_=false;
 std::function<void()> stopped_;
 bool stopping_=false,finishing_=false;
};
}
