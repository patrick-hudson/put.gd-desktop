#pragma once
#include "RegionProtocol.h"
#include "ipc/CaptureControl.h"
#include <QPair>
#include <QPointF>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include "core/Log.h"

namespace putgd {
// Owns one geometry-only helper. No callback can authorize capture before its
// fenced unmap/result handshake and normal zero exit have both completed.
class WaylandRegionProcess final: public QObject {
 Q_OBJECT
public:
 explicit WaylandRegionProcess(QObject* parent=nullptr);
 explicit WaylandRegionProcess(QString helperPath,QObject* parent=nullptr);
 ~WaylandRegionProcess() override;
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
  friend class WaylandRegionProcess;
  QPointer<WaylandRegionProcess> owner_;
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
 // A drag that began before the selector could take input (kind: press, move
 // or release; logical layout coordinates). Held until the helper has begun,
 // keeping only the latest of consecutive moves. armPointerRelay() holds it from
 // before the helper is even started; disarm drops what no helper will take.
 void relayPointer(const QString& kind,QPointF logical);
 void armPointerRelay() { pointerArmed_=true; pendingPointer_.clear(); }
 void disarmPointerRelay() { if(!busy()) { pointerArmed_=false; pendingPointer_.clear(); } }
 // Hyprland ends a held button whenever a surface takes the keyboard. With this
 // set the selector maps without it and then accepts it on demand, which ends
 // nothing. grantKeyboard hands it the keyboard outright, with the key that asked
 // for it; false until the helper has begun.
 void setDeferKeyboard(bool defer) { deferKeyboard_=defer; }
 bool grantKeyboard(int qtKey,int qtModifiers);
signals:
 // The helper is gone, however the selection ended.
 void ended();
private:
 void sendPointer(const QString& kind,QPointF logical);
 void receive(const QString&,const QJsonObject&);
 void fail(Error);
 void finish(int,QProcess::ExitStatus);
 void closeFrozen();
 void abandon(const std::shared_ptr<Prepared>&);
 int frozenFd_=-1;
 QString helperPath_,nonce_,launchStartTicks_; quint64 generation_=0; qint64 launchPid_=0;
 QList<CaptureFrame> frames_; QRectF result_; QList<QPair<QString,QPointF>> pendingPointer_;
 // The control channel allows 64 frames a second: moves go out at most every
 // 33 ms, the latest one winning; a press or release flushes it first.
 QTimer moveFlush_; std::optional<QPointF> pendingMove_; bool deferKeyboard_=false,pointerArmed_=false;
 void flushMove();
 QProcess* process_=nullptr; CaptureControl* control_=nullptr;
 QTimer deadline_,stopDeadline_;
 std::function<void(QRectF,Error)> reply_;
 Error failure_; QJsonObject lastExit_;
 HelperOutput output_{"putgd-region",logCapture()};
 bool ready_=false,begun_=false,resultReceived_=false,accepted_=false,cancelling_=false,terminal_=false,teardownCertified_=false,cleanupRequired_=false,closing_=false;
};
}
