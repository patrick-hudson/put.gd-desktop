#pragma once
#include "app/OperationService.h"
#include "KWinCapture.h"
#include "DataControlClipboard.h"
#include "KdeShortcuts.h"
#include "KdeRegionOverlay.h"
#include "KdeOutputs.h"
#include "KdeRegionProcess.h"
#include <QPointer>
#include <QThread>
namespace putgd {
class KdeSourceAdapter final:public QObject,public SourceAdapter {
 Q_OBJECT
public:
 explicit KdeSourceAdapter(QObject* parent=nullptr);
 ~KdeSourceAdapter() override;
 quint64 clipboardRevision()const override;
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void invalidateCopyEligibility() override;
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setRecording(active,std::move(reply));}
 void bindShortcuts(const QHash<QString,QString>&);
 void setUiProcess(qint64 pid) override {uiProcess_=pid>0?pid:0;}
 void setResourceLimits(const ResourceLimits& limits){limits_=limits;}
signals:
 void actionRequested(const QString&);void capabilitiesChanged();
private:
 void verifyExternalWindow(const QString&,std::function<void(Error)>);
 QList<CaptureFrame> layout()const;
 bool unchanged(const QList<CaptureFrame>&,quint64)const;
 void capture(QString,QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>);
 void captureScreens(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 void compose(QList<CaptureFrame>,QList<CaptureFrame>,quint64,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 ResourceLimits limits_;
 qint64 uiProcess_=0;
 bool core_=false;
 KWinCapture capture_;DataControlClipboard clipboard_;KdeShortcuts shortcuts_;
 KdeOutputs outputs_;KdeRegionProcess region_;
 QPointer<KdeRegionOverlay> overlay_;QList<QThread*> workers_;quint64 generation_=0;
};
}
