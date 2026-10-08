#pragma once
#include "app/OperationService.h"
#include "KWinCapture.h"
#include "DataControlClipboard.h"
#include "KdeShortcuts.h"
#include "KdeRegionOverlay.h"
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
 void bindShortcuts(const QHash<QString,QString>&);
 void setResourceLimits(const ResourceLimits& limits){limits_=limits;}
signals:
 void actionRequested(const QString&);void capabilitiesChanged();
private:
 void capture(QString,QList<CaptureFrame>,ResourceLimits,std::function<void(PayloadResult)>);
 void captureScreens(QList<CaptureFrame>,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 void compose(QList<CaptureFrame>,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 ResourceLimits limits_;
 KWinCapture capture_;DataControlClipboard clipboard_;KdeShortcuts shortcuts_;
 QPointer<KdeRegionOverlay> overlay_;QList<QThread*> workers_;quint64 generation_=0;
};
}
