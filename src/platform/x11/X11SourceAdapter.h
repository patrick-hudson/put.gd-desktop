#pragma once
#include "app/OperationService.h"
#include "X11Capture.h"
#include "X11Clipboard.h"
#include "X11Shortcuts.h"
#include "ui/RegionOverlay.h"
#include <QPointer>
namespace putgd {
class X11SourceAdapter final:public QObject,public SourceAdapter {
 Q_OBJECT
public:
 explicit X11SourceAdapter(QObject* parent=nullptr);
 ~X11SourceAdapter() override;
 quint64 clipboardRevision()const override;
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void bindShortcuts(const QHash<QString,QString>&);
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
private:
 void capture(const X11Target&,const ResourceLimits&,std::function<void(PayloadResult)>);
 void compose(QList<CaptureFrame>,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 X11Capture capture_;
 X11Clipboard clipboard_;
 X11Shortcuts shortcuts_;
 QPointer<RegionOverlay> overlay_;
 QList<QThread*> workers_;
 quint64 generation_=0;
};
}
