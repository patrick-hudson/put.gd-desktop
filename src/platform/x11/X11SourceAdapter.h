#pragma once
#include "app/OperationService.h"
#include "X11Capture.h"
#include "X11Clipboard.h"
#include "X11Shortcuts.h"
#include "ui/RegionSelector.h"
#include <QPointer>
#include <QSet>
namespace putgd {
class CinnamonRegionCapture;
class X11CompositorSettle;
// Selection carries geometry only; desktop pixels require a separate acquisition.
struct RegionSelectionResult {
 QRectF rectangle;
 quint64 generation=0;
 Error error;
};
class X11SourceAdapter final:public QObject,public SourceAdapter {
 Q_OBJECT
public:
 explicit X11SourceAdapter(QObject* parent=nullptr,bool nativeRegionSelector=false);
 ~X11SourceAdapter() override;
 quint64 clipboardRevision()const override;
 std::optional<quint64> lastCopyRevision()const override{return clipboard_.lastCopyRevision();}
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 // Private capture helper uses the resident's already frozen external target.
 void acquireFrozen(const X11Target&,const ResourceLimits&,std::function<void(PayloadResult)>);
 void selectRegionFrozen(const X11Target&,const ResourceLimits&,std::function<void(RegionSelectionResult)>);
 void acquireSelectedRegionFrozen(const X11Target&,const RegionSelectionResult&,const ResourceLimits&,std::function<void(PayloadResult)>);
 void setResidentRegionCursor(quint32 cursor){residentRegionCursor_=cursor;}
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setRecording(active,std::move(reply));}
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
 QPointer<RegionSelector> overlay_;
 bool nativeRegionSelector_=false;
 quint32 residentRegionCursor_=0;
 QList<QThread*> workers_;
 QSet<CinnamonRegionCapture*> regionCaptures_;
 QSet<X11CompositorSettle*> settles_;
 quint64 generation_=0;
 quint64 selectedGeneration_=0;
 X11Target selectedTarget_;
};
}
