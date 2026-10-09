#pragma once
#include "app/OperationService.h"
#include "CaptureControl.h"
#include "CaptureArtifactLease.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
namespace putgd {
// Owns exactly one ephemeral child and its private payload until validated exit.
class CaptureProcessController final:public QObject {
 Q_OBJECT
public:
 explicit CaptureProcessController(QString helperPath={},QObject* parent=nullptr);
 ~CaptureProcessController() override;
 bool busy()const;
 void start(const QJsonObject& frozen,const ResourceLimits&,std::function<void(PayloadResult)>);
 void cancel();
 QJsonObject diagnostics()const;
 qint64 processId()const;
private:
 void receive(const QString&,const QJsonObject&);
 void fail(Error);
 void exited(int,QProcess::ExitStatus);
 void decode(QJsonObject);
 void complete();
 QString helperPath_,nonce_,launchStartTicks_;quint64 generation_=0;qint64 launchPid_=0;
 QProcess* process_=nullptr;CaptureControl* control_=nullptr;
 CaptureArtifactLease artifact_;std::unique_ptr<QTemporaryDir> directory_;QString path_,device_,inode_;
 ResourceLimits limits_;std::function<void(PayloadResult)> callback_;
 QTimer deadline_,stopDeadline_;QThread* decoder_=nullptr;
 PayloadResult result_;Error failure_;QJsonObject lastExit_;
 bool ready_=false,resultReceived_=false,accepted_=false,exited_=false,cancelling_=false,closing_=false;
};
}
