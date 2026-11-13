#pragma once
#include "core/RecordingWindows.h"
#include "core/SourceAdapter.h"
#include "HyprlandIpc.h"
#include "HyprlandShortcuts.h"
#include "PressCatcher.h"
#include "ToplevelMapping.h"
#include "platform/wayland/DataControlClipboard.h"
#include "platform/wayland/ImageCopyCapture.h"
#include "platform/wayland/RegionProcess.h"
#include "platform/wayland/WaylandOutputs.h"
#include "platform/wayland/CaptureExtras.h"
#include "platform/CountdownProcess.h"
#include <optional>
#include <QJsonArray>
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
namespace putgd {
// Hyprland sessions (Omarchy and others), in the Core route only. Pixels come
// from ext-image-copy-capture on the adapter's own Wayland connection, region
// selection from the putgd-region layer-shell helper, the clipboard from data
// control, all through the shared Wayland code. Hyprland's own parts are its
// IPC (the active window, binds, reloads), the toplevel mapping check, the
// shortcut backend and the capture permission check.
class HyprlandSourceAdapter final:public QObject,public SourceAdapter,public RecordingWindows {
 Q_OBJECT
public:
 explicit HyprlandSourceAdapter(QObject* parent=nullptr);
 ~HyprlandSourceAdapter() override;
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
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setPaused(active,std::move(reply));}
 void bindShortcuts(const QHash<QString,QString>& desired){shortcuts_.bind(desired);}
 void setUiProcess(qint64 pid) override {uiProcess_=pid>0?pid:0;}
 // Delayed capture (#1363): putgd-region's countdown mode draws the pill on the
 // overlay layer; Hyprland's own "locked" request answers the lock check, with
 // logind's hint and the freedesktop screensaver service beside it.
 void countdown(int seconds,std::function<void(Error)>) override;
 void sessionLocked(std::function<void(bool)>) override;
 void setResourceLimits(const ResourceLimits& limits){limits_=limits;}
 // Windows from Hyprland's IPC (j/clients, j/activewindow): identity is the
 // stableId, which is the ext-foreign-toplevel identifier the recorder streams.
 void listWindows(RecordingWindows::Reply) override;
 void clearShortcuts(){shortcuts_.clear();}
 // The active window as Hyprland reports it, frozen now: {identity (stableId),
 // address, pid, class, title, at, size}. Refuses put.gd's own windows and
 // windows the toplevel mapping does not confirm. Used by window capture.
 void freezeActiveWindow(std::function<void(QJsonObject,Error)>);
 // A window of put.gd's: the resident or any process below it (putgd-ui,
 // putgd-record, putgd-region), or a put.gd app ID.
 bool ownWindow(qint64 pid,const QString& appId)const;
 // A 32-bit frame at least 99% 0x00000000: Hyprland's answer to a client whose
 // screencopy permission is denied is a transparent image (plus the cursor),
 // never an error.
 static bool refusedFrame(const QImage&);
 // The hl.permission rule that lets this executable capture.
 static QString permissionRule(const QString& executable,const QString& type=QStringLiteral("screencopy"));
 static Error permissionError();
signals:
 void actionRequested(const QString&);void capabilitiesChanged();
private:
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
 std::optional<RegionMemory> lastRegion_;
 CountdownProcess countdown_{QStringLiteral("putgd-region")};
 void captureScreens(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 // frames: the outputs to read; frozen: the whole layout, which must not change.
 void captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>);
 void selectFrozen(QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome,bool remember=true);
 void compose(QList<CaptureFrame>,QList<CaptureFrame>,quint64,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 ResourceLimits limits_;
 qint64 uiProcess_=0;
 HyprlandIpc ipc_;
 ImageCopyCapture capture_;DataControlClipboard clipboard_;HyprlandShortcuts shortcuts_;ToplevelMapping mapping_;
 // Declared before region_: its end stops them.
 PressCatcher catcher_;QTimer catcherUnclaimed_;QElapsedTimer earlyCancel_;
 void catchEarlyPress(const QList<CaptureFrame>&);
 // Esc or a right click on the catcher before the selector started: the region
 // acquisition already on its way must not open one afterwards.
 bool takeEarlyCancel(){const bool pending=earlyCancel_.isValid()&&earlyCancel_.elapsed()<3000;earlyCancel_.invalidate();return pending;}
 WaylandOutputs outputs_;WaylandRegionProcess region_;
 QList<QThread*> workers_;quint64 generation_=0;
};
}
