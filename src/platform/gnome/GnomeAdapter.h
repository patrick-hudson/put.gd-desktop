#pragma once
#include "app/OperationService.h"
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
namespace putgd {
class GnomeAdapter final:public QObject,public SourceAdapter {
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
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
private slots:
 void helperAction(const QString& action,const QString& ticket);
 void helperClipboardChanged(qulonglong revision);
 void helperChanged();
private:
 using Reply=std::function<void(Error,QJsonObject)>;
 struct Transfer {QString id;ResourceLimits limits;QJsonObject metadata;QByteArray bytes;std::function<void(PayloadResult)> reply;quint64 generation=0;};
 void discover();
 void applyShortcutBindings(std::function<void(Error)> reply={});
 void invalidateShortcutBindings();
 void call(const QString& method,const QVariantList&,Reply,int timeout=5000);
 QJsonObject sync(const QString& method,const QVariantList&,Error&,int timeout=1000);
 void start(const QString& id,const QString& action,const ResourceLimits&,std::function<void(PayloadResult)>);
 void requestTransfer(const std::shared_ptr<Transfer>&,const QJsonArray& imageMimes);
 void read(const std::shared_ptr<Transfer>&);
 void decode(const std::shared_ptr<Transfer>&);
 void release(const QString&);
 void writeCopy(const QString&,const QByteArray&,qsizetype,quint64,std::function<void(Error)>);
 void cancelAll(const QString&,const QString&);
 QDBusConnection bus_;
 QDBusServiceWatcher watcher_;
 QTimer probe_;
 QString owner_;
 Error helperError_,uiProcessError_;
 qint64 uiProcess_=0;
 QJsonObject helper_,bindings_;
 QHash<QString,QString> requested_;
 QHash<QString,QStringList> hotkeyPrepared_;
 QHash<QString,std::shared_ptr<Transfer>> transfers_;
 QStringList prepared_;
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
