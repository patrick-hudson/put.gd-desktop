#pragma once
#include "core/RecordingWindows.h"
#include "core/SourceAdapter.h"
#include "HyprlandIpc.h"
#include "HyprlandShortcuts.h"
#include "ToplevelMapping.h"
#include "platform/wayland/DataControlClipboard.h"
#include "platform/wayland/ImageCopyCapture.h"
#include "platform/wayland/RegionProcess.h"
#include "platform/wayland/WaylandOutputs.h"
#include <QJsonArray>
#include <QPointer>
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
 void capture(QString,QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome,bool screenshotFirst=false);
 void captureScreens(QList<CaptureFrame>,quint64,QRectF,ResourceLimits,std::function<void(PayloadResult)>);
 void captureFrames(QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>);
 void selectFrozen(QList<CaptureFrame>,quint64,ResourceLimits,std::function<void(PayloadResult)>,const QString& outcome);
 void compose(QList<CaptureFrame>,QList<CaptureFrame>,quint64,QRectF,const ResourceLimits&,std::function<void(PayloadResult)>);
 ResourceLimits limits_;
 qint64 uiProcess_=0;
 HyprlandIpc ipc_;
 ImageCopyCapture capture_;DataControlClipboard clipboard_;HyprlandShortcuts shortcuts_;ToplevelMapping mapping_;
 WaylandOutputs outputs_;WaylandRegionProcess region_;
 QList<QThread*> workers_;quint64 generation_=0;
};
}
