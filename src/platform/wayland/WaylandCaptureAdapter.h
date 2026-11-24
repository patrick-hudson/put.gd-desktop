#pragma once
#include "core/RecordingWindows.h"
#include "core/SourceAdapter.h"
#include "platform/wayland/CaptureExtras.h"
#include "platform/wayland/DataControlClipboard.h"
#include "platform/wayland/ImageCopyCapture.h"
#include "platform/wayland/RegionProcess.h"
#include "platform/wayland/WaylandOutputs.h"
#include "platform/CountdownProcess.h"
#include <optional>
#include <QPointer>
#include <QThread>
namespace putgd {
struct CaptureSequence;
// The capture sequence of the ext-image-copy-capture adapters (Budgie,
// Hyprland), in the Core route only: pixels from ext-image-copy-capture on the
// adapter's own Wayland connection, region selection from the putgd-region
// layer-shell helper, the clipboard from data control (#1446). Each desktop
// supplies how the active window is frozen and read, how the pointer's output
// is found and how sessionLocked() is answered; the other hooks default to
// doing nothing.
class WaylandCaptureAdapter:public QObject,public SourceAdapter,public RecordingWindows {
 Q_OBJECT
public:
 explicit WaylandCaptureAdapter(QObject* parent=nullptr);
 quint64 clipboardRevision()const override;
 std::optional<quint64> lastCopyRevision()const override{return clipboard_.lastCopyRevision();}
 PreparedAcquisition prepareAcquisition(const QString&) override;
 PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)>,std::function<void()>) override;
 using SourceAdapter::prepareAcquisition;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void invalidateCopyEligibility() override;
 void setUiProcess(qint64 pid) override {uiProcess_=pid>0?pid:0;}
 // Delayed capture (#1363): putgd-region's countdown mode draws the pill on the overlay layer.
 void countdown(int seconds,std::function<void(Error)>) override;
 void setResourceLimits(const ResourceLimits& limits){limits_=limits;}
signals:
 void actionRequested(const QString&);void capabilitiesChanged();
protected:
 friend struct CaptureSequence;
 static QString cleanupMessage();
 // The derived destructor calls this first, while its own members still exist.
 void shutdownCapture();
 // Freezes the active window now and reads it; complete gets its image or why not.
 virtual void captureActiveWindow(const ResourceLimits&,quint64 generation,std::function<void(QImage,Error)> complete)=0;
 // The output under the pointer now, as its logical rectangle.
 virtual void probePointer(const QList<CaptureFrame>& frozen,std::function<void(QRectF,Error)> reply)=0;
 virtual void cancelPointer(){}
 // Hyprland's press catcher: a press or key before the selector has surfaces.
 virtual bool takeEarlyCancel(){return false;}
 virtual void catchEarlyPress(const QList<CaptureFrame>&){}
 // The selector's start was asked for; a catcher with no selector to feed stops.
 virtual void selectorStarted(){}
 // A read output's image before it is checked against its frozen output.
 virtual Error checkOutput(const QImage&)const{return {};}
 // Reads the outputs of a capture; Budgie witnesses visibility around it.
 virtual void captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>);
 // frames: the outputs to read; frozen: the whole layout, which must not change.
 void readOutputs(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>);
 QList<CaptureFrame> layout()const{return outputs_.layout();}
 bool unchanged(const QList<CaptureFrame>&,quint64)const;
 // remember=false for a text capture (#1428): Repeat last region keeps its rectangle.
 void capture(QString,QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome,bool screenshotFirst=false,int delay=0,bool remember=true);
 // Capture monitor and Repeat last region (#1363): a known logical rectangle, no selector.
 void captureArea(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 // Remembers a region once it was captured, for Repeat last region.
 std::function<void(PayloadResult)> remembering(QRectF,QList<CaptureFrame>,std::function<void(PayloadResult)>,bool remember=true);
 // After a delayed live selection: a locked screen captures nothing.
 void lockGate(int delay,std::function<void(Error)>);
 void captureScreens(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 void selectFrozen(QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome,bool remember=true);
 void compose(QList<CaptureFrame>,QList<CaptureFrame>,quint64,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 std::optional<RegionMemory> lastRegion_;
 CountdownProcess countdown_{QStringLiteral("putgd-region")};
 ResourceLimits limits_;
 qint64 uiProcess_=0;
 // Resolve capture's inherited socket before other clients can consume it:
 // these are constructed before the derived adapter's own connections.
 ImageCopyCapture capture_;DataControlClipboard clipboard_;
 WaylandOutputs outputs_;WaylandRegionProcess region_;
 QList<QThread*> workers_;quint64 generation_=0;
};
}
