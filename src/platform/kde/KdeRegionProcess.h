#pragma once
#include "KdeRegionProtocol.h"
#include "platform/x11/CaptureControl.h"
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
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
 // pixels (Screenshot first, #629): the frozen frames, in layout order, that the
 // helper shows as its backdrop. Empty keeps the transparent live selector.
 void start(const QList<CaptureFrame>&,std::function<void(QRectF,Error)>,QString outcome={},const QList<CaptureFrame>& pixels={},QJsonObject appearance={});
 // Live selection before durable acceptance: the selector shows now, and its
 // geometry waits for begin(). No pixels are involved until the caller acts on
 // that result. terminal reports a failure or cancellation before begin();
 // released reports that the helper has drained. A ticket dropped unconsumed
 // cancels its selection.
 struct Prepared;
 class Preparation final {
 public:
  ~Preparation();
 private:
  friend class KdeRegionProcess;
  QPointer<KdeRegionProcess> owner_;
  std::shared_ptr<Prepared> state_;
  std::atomic_bool consumed_=false;
 };
 using Ticket=std::shared_ptr<Preparation>;
 Ticket prepare(const QList<CaptureFrame>&,QString outcome,QJsonObject appearance,std::function<void(Error)> terminal,std::function<void()> released);
 void begin(const Ticket&,std::function<void(QRectF,Error)>);
 void discard(const Ticket&);
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
 void closeFrozen();
 void abandon(const std::shared_ptr<Prepared>&);
 int frozenFd_=-1;
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
