#pragma once
#include "core/SourceAdapter.h"
#include "KWinCapture.h"
#include "platform/wayland/DataControlClipboard.h"
#include "KdeShortcuts.h"
#include "platform/wayland/LayerShellRegionOverlay.h"
#include "platform/wayland/WaylandOutputs.h"
#include "platform/wayland/RegionProcess.h"
#include "KWinWindows.h"
#include "platform/CountdownProcess.h"
#include "platform/wayland/CaptureExtras.h"
#include <optional>
#include "core/RecordingWindows.h"
#include <QPointer>
#include <QThread>
namespace putgd {
struct CaptureSequence;
// Also the window list for window recordings (KWin window UUIDs).
class KdeSourceAdapter final:public QObject,public SourceAdapter,public RecordingWindows {
 Q_OBJECT
 friend struct CaptureSequence;
public:
 explicit KdeSourceAdapter(QObject* parent=nullptr);
 ~KdeSourceAdapter() override;
 quint64 clipboardRevision()const override;
 std::optional<quint64> lastCopyRevision()const override{return clipboard_.lastCopyRevision();}
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 // A live region selection starts here, before the journal, as on X11.
 PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)>,std::function<void()>) override;
 using SourceAdapter::prepareAcquisition;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void invalidateCopyEligibility() override;
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setRecording(active,std::move(reply));}
 void bindShortcuts(const QHash<QString,QString>&);
 void setUiProcess(qint64 pid) override {uiProcess_=pid>0?pid:0;}
 // Delayed capture (#1363): putgd-region's countdown mode draws the pill on the
 // overlay layer; KWin's screensaver service and logind answer the lock check.
 void countdown(int seconds,std::function<void(Error)>) override;
 void sessionLocked(std::function<void(bool)>) override;
 void setResourceLimits(const ResourceLimits& limits){limits_=limits;}
 // put.gd's own windows are left out as for window capture: the resident, the
 // processes it started (views, editor, recording helper) and the putgd-ui app ID.
 void listWindows(RecordingWindows::Reply) override;
signals:
 void actionRequested(const QString&);void capabilitiesChanged();
private:
 void verifyExternalWindow(const QString&,std::function<void(Error)>);
 QList<CaptureFrame> layout()const;
 bool unchanged(const QList<CaptureFrame>&,quint64)const;
 // remember=false for a text capture (#1428): Repeat last region keeps its rectangle.
 void capture(QString,QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome,bool screenshotFirst=false,int delay=0,bool remember=true);
 // Reads just the outputs the selection lies on.
 void captureScreens(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 // Capture monitor and Repeat last region (#1363): a known logical rectangle, no selector.
 void captureArea(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 // Remembers a region once it was captured, for Repeat last region.
 std::function<void(PayloadResult)> remembering(QRectF,QList<CaptureFrame>,std::function<void(PayloadResult)>,bool remember=true);
 // After a delayed live selection: a locked screen captures nothing.
 void lockGate(int delay,std::function<void(Error)>);
 std::optional<RegionMemory> lastRegion_;
 CountdownProcess countdown_{QStringLiteral("putgd-region")};
 // Reads the given outputs through ScreenShot2, in layout order.
 void captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>);
 // Screenshot first (#629): select over frames already read, then cut from them.
 void selectFrozen(QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome,bool remember=true);
 void compose(QList<CaptureFrame>,QList<CaptureFrame>,quint64,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 ResourceLimits limits_;
 qint64 uiProcess_=0;
 bool core_=false;
 KWinCapture capture_;DataControlClipboard clipboard_;KdeShortcuts shortcuts_;
 WaylandOutputs outputs_;WaylandRegionProcess region_;KWinWindows windows_;
 QPointer<LayerShellRegionOverlay> overlay_;QList<QThread*> workers_;quint64 generation_=0;
};
}
