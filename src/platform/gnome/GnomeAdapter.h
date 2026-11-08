#pragma once
#include "app/RecordingBackend.h"
#include "core/SourceAdapter.h"
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
namespace putgd {
// Also the recording shell: the extension's selector, frame actors and bar
// placement; and the window list for window recordings (Mutter's window ids).
class GnomeAdapter final:public QObject,public SourceAdapter,public RecordingShell,public RecordingWindows {
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
 bool selectsRegions()const override;
 Error regionError()const override;
 // The loaded extension is older than the packaged one: a log-out loads it.
 bool outdated()const;
 // Discovery retry: the first wait and the slow safety probe it doubles up to.
 static constexpr int firstRetryMs=3000,safetyProbeMs=15*60*1000;
 void setDiscoveryRetryForTesting(int firstMs,int capMs);
 void selectRegion(RecordingShell::Reply) override;
 void cancelSelection() override;
 void showFrame(const QJsonObject&,RecordingShell::Reply) override;
 void hideFrame() override;
 // Recording protocol 2: the extension's RecordingWindows, without put.gd's own.
 void listWindows(RecordingWindows::Reply) override;
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
 // A timed discovery retry ran while the helper was unavailable.
 void discoveryRetried();
private slots:
 void helperAction(const QString& action,const QString& ticket);
 void helperClipboardChanged(qulonglong revision);
 void helperChanged();
private:
 // Why a region cannot be recorded through the Shell right now.
 Error recordingShellError()const;
 using Reply=std::function<void(Error,QJsonObject)>;
 struct Transfer {QString id,outcome;bool screenshotFirst=false;ResourceLimits limits;QJsonObject metadata;QByteArray bytes;std::function<void(PayloadResult)> reply;quint64 generation=0;};
 void discover();
 void followOwner(const QString&);
 void helperUnavailable(const Error&);
 void applyShortcutBindings(std::function<void(Error)> reply={});
 void invalidateShortcutBindings();
 void call(const QString& method,const QVariantList&,Reply,int timeout=5000);
 QJsonObject sync(const QString& method,const QVariantList&,Error&,int timeout=1000);
 void start(const QString& id,const QString& action,const ResourceLimits&,std::function<void(PayloadResult)>,const QString& outcome,bool screenshotFirst=false);
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
};
}
