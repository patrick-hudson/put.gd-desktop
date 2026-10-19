#pragma once
#include "KdeRegionProtocol.h"
#include "platform/x11/CaptureControl.h"
#include <QProcess>
#include <QTimer>
#include <functional>
#include "core/Log.h"

namespace putgd {
// Owns one geometry-only helper. No callback can authorize capture before its
// fenced unmap/result handshake and normal zero exit have both completed.
class KdeRegionProcess final: public QObject {
 Q_OBJECT
public:
 explicit KdeRegionProcess(QObject* parent=nullptr);
 explicit KdeRegionProcess(QString helperPath,QObject* parent=nullptr);
 ~KdeRegionProcess() override;
 void start(const QList<CaptureFrame>&,std::function<void(QRectF,Error)>);
 void cancel(Error={"cancelled","Region selection cancelled",{}});
 // Permanently stop this request owner without calling into its dying caller.
 void shutdown();
 bool busy() const { return process_!=nullptr; }
 bool cleanupRequired() const { return cleanupRequired_; }
 qint64 processId() const { return process_?process_->processId():0; }
 QJsonObject diagnostics() const;
private:
 void receive(const QString&,const QJsonObject&);
 void fail(Error);
 void finish(int,QProcess::ExitStatus);
 QString helperPath_,nonce_,launchStartTicks_; quint64 generation_=0; qint64 launchPid_=0;
 QList<CaptureFrame> frames_; QRectF result_;
 QProcess* process_=nullptr; CaptureControl* control_=nullptr;
 QTimer deadline_,stopDeadline_;
 std::function<void(QRectF,Error)> reply_;
 Error failure_; QJsonObject lastExit_;
 HelperOutput output_{"putgd-kde-region",logCapture()};
 bool ready_=false,begun_=false,resultReceived_=false,accepted_=false,cancelling_=false,terminal_=false,teardownCertified_=false,cleanupRequired_=false,closing_=false;
};
}
