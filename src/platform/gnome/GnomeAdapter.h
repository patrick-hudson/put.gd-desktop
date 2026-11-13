#pragma once
#include "app/RecordingBackend.h"
#include "core/SourceAdapter.h"
#include "core/RecordingClicks.h"
#include "platform/gnome/GnomeExtensionSetup.h"
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <optional>
namespace putgd {
// Also the recording shell: the extension's selector, frame actors and bar
// placement; and the window list for window recordings (Mutter's window ids).
class GnomeAdapter final:public QObject,public SourceAdapter,public RecordingShell,public RecordingWindows,public RecordingClicks {
 Q_OBJECT
public:
 explicit GnomeAdapter(QObject* parent=nullptr);
 ~GnomeAdapter() override;
 quint64 clipboardRevision()const override;
 std::optional<quint64> lastCopyRevision()const override{return lastCopyRevision_;}
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void setShortcutRecording(bool,std::function<void(Error)>);
 void bindShortcuts(const QHash<QString,QString>&);
 void setUiProcess(qint64) override;
 void invalidateCopyEligibility() override;
 // Delayed capture (#1363): the extension draws the countdown as a Shell actor;
 // a lock drops it with session_locked.
 void countdown(int seconds,std::function<void(Error)>) override;
 void sessionLocked(std::function<void(bool)>) override;
 bool selectsRegions()const override;
 Error regionError()const override;
 // The loaded extension is older than the packaged one: a log-out loads it.
 RecordingShell* scrollShell() {return &scrollShell_;}
 bool outdated()const;
 QJsonObject desktopSetup()const override;
 void recheckDesktopSetup(std::function<void(QJsonObject)>) override;
 // Discovery retry: the first wait and the slow safety probe it doubles up to.
 static constexpr int firstRetryMs=3000,safetyProbeMs=15*60*1000;
 void setDiscoveryRetryForTesting(int firstMs,int capMs);
 void selectRegion(RecordingShell::Reply) override;
 void cancelSelection() override;
 void showFrame(const QJsonObject&,RecordingShell::Reply) override;
 void hideFrame() override;
 // Recording protocol 2: the extension's RecordingWindows, without put.gd's own.
 void listWindows(RecordingWindows::Reply) override;
 // Recording protocol 3 (#1364): the extension reports pointer button presses
 // for click highlights, to this resident's bus name only.
 QJsonObject clickCapability()const override;
 void watchClicks(const QJsonObject& target,RecordingClicks::Sink,RecordingClicks::Reply) override;
 void unwatchClicks() override;
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
 // A timed discovery retry ran while the helper was unavailable.
 void discoveryRetried();
 // Setup needs the user: once per state, for the notification.
 void setupNotice(const QString& code,const QString& message);
private slots:
 void helperAction(const QString& action,const QString& ticket);
 void helperClick(const QString& json);
 void helperClipboardChanged(qulonglong revision);
 void helperChanged();
private:
 void selectRegionImpl(RecordingShell::Reply,bool scroll);
 void showFrameImpl(const QJsonObject&,RecordingShell::Reply,bool scroll);
 class ScrollShell final:public RecordingShell {
 public:
  explicit ScrollShell(GnomeAdapter& owner):owner_(owner){}
  bool selectsRegions()const override{return owner_.ready_&&owner_.helper_["recordingVersion"].toInt()>=1&&owner_.helper_["extensionVersion"].toInt()>=22;}
  Error regionError()const override{return selectsRegions()?Error{}:owner_.regionError();}
  void selectRegion(Reply reply)override{owner_.selectRegionImpl(std::move(reply),true);}
  void cancelSelection()override{owner_.cancelSelection();}
  void showFrame(const QJsonObject& frame,Reply reply)override{owner_.showFrameImpl(frame,std::move(reply),true);}
  void hideFrame()override{if(selectsRegions())owner_.call("HideRecordingFrame",{},[](Error,QJsonObject){});}
 private:GnomeAdapter& owner_;
 } scrollShell_{*this};
 // Why a region cannot be recorded through the Shell right now.
 Error recordingShellError()const;
 // The per-user GNOME setup (GnomeExtensionSetup): once per Shell, on a worker.
 void evaluateSetup(bool force=false);
 gnome::SetupStatus currentSetup()const;
 Error helperAvailabilityError(bool fallback=false)const;
 void emitSetupNotice(const gnome::SetupStatus&);
 RecordingClicks::Sink clickSink_;
 using Reply=std::function<void(Error,QJsonObject)>;
 // Capture version 2: monitor, repeat and the countdown need the packaged extension.
 Error captureVersionError()const;
 // A helper that drops out of a counting request on a locked session is the lock.
 void lockAware(Error,std::function<void(Error)>,int attempt=0);
 // Repeat last region (#1363): the rectangle and the extension's layout key of
 // the last region capture, in this process's memory only.
 struct RegionMemory {QJsonArray area;QString layoutKey;};
 std::optional<RegionMemory> lastRegion_;
 QString countdownId_;
 struct Transfer {QString id,outcome,action;int delaySeconds=0;QJsonArray area;QString layoutKey;bool screenshotFirst=false,remember=true;ResourceLimits limits;QJsonObject metadata;QByteArray bytes;std::function<void(PayloadResult)> reply;quint64 generation=0;};
 void discover();
 void followOwner(const QString&);
 void helperUnavailable(const Error&);
 void applyShortcutBindings(std::function<void(Error)> reply={});
 void invalidateShortcutBindings();
 void call(const QString& method,const QVariantList&,Reply,int timeout=5000);
 QJsonObject sync(const QString& method,const QVariantList&,Error&,int timeout=1000);
 void start(const QString& id,const QString& action,const ResourceLimits&,std::function<void(PayloadResult)>,const QString& outcome,bool screenshotFirst=false,int delaySeconds=0,const std::optional<RegionMemory>& repeat={},bool remember=true);
 void requestTransfer(const std::shared_ptr<Transfer>&,const QJsonArray& imageMimes);
 void read(const std::shared_ptr<Transfer>&);
 void decode(const std::shared_ptr<Transfer>&);
 void release(const QString&);
 void writeCopy(const QString&,const QByteArray&,qsizetype,quint64,std::function<void(Error)>);
 void cancelAll(const QString&,const QString&);
 QDBusConnection bus_;
 QDBusServiceWatcher watcher_;
 QTimer discoveryRetry_,uiProcessRetry_;
 int retryFirstMs_=firstRetryMs,retryCapMs_=safetyProbeMs,retryMs_=firstRetryMs;
 QString owner_;
 Error helperError_,uiProcessError_;
 qint64 uiProcess_=0;
 QJsonObject helper_,bindings_;
 QHash<QString,QString> requested_;
 QHash<QString,QStringList> hotkeyPrepared_;
 QHash<QString,std::shared_ptr<Transfer>> transfers_;
 QStringList prepared_;
 QString recordingSelection_;
 QList<QThread*> workers_;
 std::optional<quint64> lastCopyRevision_;
 // Image types the transient image job can decode, listed once for clipboard imports.
 std::optional<QJsonArray> clipboardImageMimes_;
 quint64 revision_=0,generation_=0,helperRevision_=0,copyGeneration_=0;
 QHash<quint64,Reply> pending_;quint64 callSerial_=0;
 bool recording_=false;quint64 bindingGeneration_=0;
 bool ownsName_=false,ready_=false,stopping_=false;
 gnome::SetupStatus setup_{"checking",{},{},{}};
 bool setupRunning_=false,setupAgain_=false,setupCheckedForOwner_=false;
 QStringList setupNotified_;
 QList<std::function<void(QJsonObject)>> setupWaiters_;
};
}
