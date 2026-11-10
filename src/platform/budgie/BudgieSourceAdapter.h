#pragma once
#include "core/RecordingWindows.h"
#include "core/SourceAdapter.h"
#include "BudgieShortcuts.h"
#include "platform/wayland/DataControlClipboard.h"
#include "platform/wayland/ImageCopyCapture.h"
#include "platform/wayland/RegionProcess.h"
#include "platform/wayland/WaylandOutputs.h"
#include <QJsonArray>
#include <QPointer>
#include <QThread>
namespace putgd {
// Budgie 10.10 (labwc) and Magpie sessions, in the Core route only. Pixels come
// from ext-image-copy-capture on the adapter's own Wayland connection, region
// selection from the putgd-region layer-shell helper, the clipboard from data
// control. Nothing is chosen by compositor name: capabilities() reports which
// protocols the compositor actually offers.
class BudgieSourceAdapter final:public QObject,public SourceAdapter,public RecordingWindows {
 Q_OBJECT
public:
 explicit BudgieSourceAdapter(QObject* parent=nullptr);
 ~BudgieSourceAdapter() override;
 quint64 clipboardRevision()const override;
 std::optional<quint64> lastCopyRevision()const override{return clipboard_.lastCopyRevision();}
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)>,std::function<void()>) override;
 using SourceAdapter::prepareAcquisition;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void invalidateCopyEligibility() override;
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setRecording(active,std::move(reply));}
 void bindShortcuts(const QHash<QString,QString>&);
 void setUiProcess(qint64 pid) override {uiProcess_=pid>0?pid:0;}
 void setResourceLimits(const ResourceLimits& limits){limits_=limits;}
 // recording.windows: ext toplevel identifiers, the target a window recording
 // takes (no picker exists). Neither protocol gives geometry, so rect is empty.
 void listWindows(RecordingWindows::Reply) override;
 void clearShortcuts(){shortcuts_.clear();}
 // put.gd's own windows, refused by app ID: Wayland gives no PID for a toplevel.
 static bool ownAppId(const QString&);
signals:
 void actionRequested(const QString&);void capabilitiesChanged();
private:
 QList<CaptureFrame> layout()const{return outputs_.layout();}
 bool unchanged(const QList<CaptureFrame>&,quint64)const;
 void capture(QString,QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome,bool screenshotFirst=false);
 void captureScreens(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 void captureVisibleFrames(QList<CaptureFrame>,QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>);
 void captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>);
 void selectFrozen(QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome);
 void compose(QList<CaptureFrame>,QList<CaptureFrame>,quint64,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 ResourceLimits limits_;
 qint64 uiProcess_=0;
 ImageCopyCapture capture_;DataControlClipboard clipboard_;BudgieShortcuts shortcuts_;
 WaylandOutputs outputs_;WaylandRegionProcess region_;
 QList<QThread*> workers_;quint64 generation_=0;
};
}
