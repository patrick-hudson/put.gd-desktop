#pragma once
#include "HyprlandIpc.h"
#include "HyprlandShortcuts.h"
#include "PressCatcher.h"
#include "ToplevelMapping.h"
#include "platform/wayland/WaylandCaptureAdapter.h"
#include <QJsonArray>
#include <QElapsedTimer>
#include <QTimer>
namespace putgd {
// Hyprland sessions (Omarchy and others), in the Core route only. Pixels come
// from ext-image-copy-capture on the adapter's own Wayland connection, region
// selection from the putgd-region layer-shell helper, the clipboard from data
// control, all through the shared Wayland code. Hyprland's own parts are its
// IPC (the active window, binds, reloads), the toplevel mapping check, the
// shortcut backend and the capture permission check.
class HyprlandSourceAdapter final:public WaylandCaptureAdapter {
 Q_OBJECT
public:
 explicit HyprlandSourceAdapter(QObject* parent=nullptr);
 ~HyprlandSourceAdapter() override;
 QJsonObject capabilities()const override;
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setPaused(active,std::move(reply));}
 void bindShortcuts(const QHash<QString,QString>& desired){shortcuts_.bind(desired);}
 // Delayed capture (#1363): putgd-region's countdown mode draws the pill on the
 // overlay layer; Hyprland's own "locked" request answers the lock check, with
 // logind's hint and the freedesktop screensaver service beside it.
 void sessionLocked(std::function<void(bool)>) override;
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
private:
 void captureActiveWindow(const ResourceLimits&,quint64 generation,std::function<void(QImage,Error)> complete) override;
 void probePointer(const QList<CaptureFrame>& frozen,std::function<void(QRectF,Error)> reply) override;
 // A denied client gets a transparent frame of the right size, never an error.
 Error checkOutput(const QImage& image)const override{return refusedFrame(image)?permissionError():Error{};}
 HyprlandIpc ipc_;
 HyprlandShortcuts shortcuts_;ToplevelMapping mapping_;
 // region_ (the base's) ending stops them. It outlives them, so the destructor
 // shuts it down first, after which it reports no end.
 PressCatcher catcher_;QTimer catcherUnclaimed_;QElapsedTimer earlyCancel_;
 void catchEarlyPress(const QList<CaptureFrame>&) override;
 void selectorStarted() override{if(!region_.busy())catcher_.stop();}
 // Esc or a right click on the catcher before the selector started: the region
 // acquisition already on its way must not open one afterwards.
 bool takeEarlyCancel() override{const bool pending=earlyCancel_.isValid()&&earlyCancel_.elapsed()<3000;earlyCancel_.invalidate();return pending;}
};
}
