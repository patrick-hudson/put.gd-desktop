#pragma once
#include "app/OperationService.h"
#include "core/Log.h"
#include "CaptureControl.h"
#include "CaptureArtifactLease.h"
#include "CaptureCursorCache.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <memory>
namespace putgd {
// Owns exactly one ephemeral child and its private payload until validated exit.
class CaptureProcessController final:public QObject {
 Q_OBJECT
public:
 class Preparation final {
 public:
  ~Preparation();
 private:
  friend class CaptureProcessController;
  QPointer<CaptureProcessController> controller_;
  quint64 generation_=0;
  std::atomic_bool consumed_=false;
  Error failure_;
 };
 using Ticket=std::shared_ptr<Preparation>;
 explicit CaptureProcessController(QString helperPath={},QObject* parent=nullptr);
 ~CaptureProcessController() override;
 bool busy()const;
 bool preparedReady()const;
 Ticket prepare(const QJsonObject& frozen,const ResourceLimits&,std::function<void(Error)> earlyTerminal={},std::function<void()> earlyReleased={});
 void begin(const Ticket&,std::function<void(PayloadResult)>);
 void discard(const Ticket&);
 void start(const QJsonObject& frozen,const ResourceLimits&,std::function<void(PayloadResult)>);
 void cancel();
 QJsonObject diagnostics()const;
 qint64 processId()const;
private:
 void receive(const QString&,const QJsonObject&);
 void sendPreview();
 void sendBegin();
 void notifyEarlyTerminal();
 void abandon(quint64);
 void fail(Error);
 void exited(int,QProcess::ExitStatus);
 void decode(QJsonObject);
 void complete();
 QString helperPath_,nonce_,launchStartTicks_;quint64 generation_=0;qint64 launchPid_=0;
 QProcess* process_=nullptr;CaptureControl* control_=nullptr;
 CaptureArtifactLease artifact_;std::unique_ptr<QTemporaryDir> directory_;QString path_,device_,inode_;
 CaptureCursorCache cursorCache_;QString cursorFingerprint_;quint32 requestedCursor_=0,previewCursor_=0;
 ResourceLimits limits_;std::function<void(PayloadResult)> callback_;
 std::function<void(Error)> earlyTerminal_;
 std::function<void()> earlyReleased_;
 std::weak_ptr<Preparation> preparation_;
 QTimer deadline_,stopDeadline_;QThread* decoder_=nullptr;
 PayloadResult result_;Error failure_;QJsonObject lastExit_;
 HelperOutput output_{"putgd-capture",logCapture()};
 bool prepared_=false,beginRequested_=false,begun_=false,region_=false,previewed_=false;
 bool ready_=false,resultReceived_=false,accepted_=false,exited_=false,cancelling_=false,closing_=false;
};
}
