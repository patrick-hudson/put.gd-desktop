#pragma once
#include "core/SourceAdapter.h"
#include "core/Log.h"
#include "ipc/CaptureControl.h"
#include "CaptureArtifactLease.h"
#include "CaptureCursorCache.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <memory>
#include <optional>
#include <QRectF>
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
 // screenshotFirst (#629): the helper reads every monitor after begin, before
 // its selector maps, and cuts the region from those frames.
 // delaySeconds (#1363): a live region counts down after selection, in the helper.
 Ticket prepare(const QJsonObject& frozen,const ResourceLimits&,std::function<void(Error)> earlyTerminal={},std::function<void()> earlyReleased={},QString outcome={},bool screenshotFirst=false,QJsonObject appearance={},int delaySeconds=0);
 // The logical rectangle of the last accepted region result, until the next request.
 std::optional<QRectF> selection()const{return selection_;}
 // The monitors that selection was made on, as the helper described them.
 QJsonArray selectionLayout()const{return selectionLayout_;}
 void begin(const Ticket&,std::function<void(PayloadResult)>);
 void discard(const Ticket&);
 void start(const QJsonObject& frozen,const ResourceLimits&,std::function<void(PayloadResult)>,QString outcome={},bool screenshotFirst=false,QJsonObject appearance={},int delaySeconds=0);
 void cancel();
 // The crosshair at the key press, while the helper loads: the resident's server
 // cursor and the pointer, handed to the helper's mapped surfaces with the clicks
 // made meanwhile. Needs a cursor from an earlier selection.
 bool holdCrosshair();
 bool crosshairHeld()const{return cursorCache_.holding();}
 // The press did not start a selection, so nothing will claim the hold.
 void releaseUnclaimedCrosshair();
 // A hold never outlasts this: the desktop gets the pointer and its cursor back,
 // and a helper that arrives later selects as usual with the clicks already made.
 static constexpr int crosshairHoldMs=5000;
 QJsonObject diagnostics()const;
 qint64 processId()const;
private:
 void receive(const QString&,const QJsonObject&);
 void releaseCrosshair();
 void forgetHeldButtons();
 void observedCursor(const QJsonObject&);
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
 CaptureCursorCache cursorCache_;QString cursorFingerprint_;quint32 requestedCursor_=0,previewCursor_=0;QByteArray observedCursorHash_;QList<CaptureCursorCache::HeldButton> heldButtons_;
 ResourceLimits limits_;std::function<void(PayloadResult)> callback_;
 std::function<void(Error)> earlyTerminal_;
 std::function<void()> earlyReleased_;
 std::weak_ptr<Preparation> preparation_;
 QTimer deadline_,stopDeadline_,holdDeadline_;QThread* decoder_=nullptr;
 PayloadResult result_;Error failure_;QJsonObject lastExit_;std::optional<QRectF> selection_;QJsonArray selectionLayout_;int delayMs_=0;
 HelperOutput output_{"putgd-capture",logCapture()};
 bool cursorCompatible_=false,prepared_=false,beginRequested_=false,begun_=false,region_=false,previewed_=false;
 bool ready_=false,resultReceived_=false,accepted_=false,exited_=false,cancelling_=false,closing_=false;
};
}
