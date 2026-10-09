#pragma once
#include "app/OperationService.h"
#include "X11Capture.h"
#include "X11Clipboard.h"
#include "X11Shortcuts.h"
#include "ui/RegionOverlay.h"
#include <QPointer>
#include <QSet>
namespace putgd {
class CinnamonRegionCapture;
class X11SourceAdapter final:public QObject,public SourceAdapter {
 Q_OBJECT
public:
 explicit X11SourceAdapter(QObject* parent=nullptr);
 ~X11SourceAdapter() override;
 quint64 clipboardRevision()const override;
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 // Private capture helper uses the resident's already frozen external target.
 void acquireFrozen(const X11Target&,const ResourceLimits&,std::function<void(PayloadResult)>);
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void bindShortcuts(const QHash<QString,QString>&);
 void setUiProcess(qint64 pid) override {capture_.setUiProcess(pid);}
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
private:
 void capture(const X11Target&,const ResourceLimits&,std::function<void(PayloadResult)>);
 void captureSelected(const X11Target&,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>,quint64);
 void compose(QList<CaptureFrame>,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 X11Capture capture_;
 X11Clipboard clipboard_;
 X11Shortcuts shortcuts_;
 QPointer<RegionOverlay> overlay_;
 QList<QThread*> workers_;
 QSet<CinnamonRegionCapture*> regionCaptures_;
 quint64 generation_=0;
};
}
