#pragma once
#include "BudgieShortcuts.h"
#include "platform/wayland/CursorOutputProbe.h"
#include "platform/wayland/WaylandCaptureAdapter.h"
#include <QJsonArray>
namespace putgd {
// Budgie 10.10 (labwc) and Magpie sessions, in the Core route only. Pixels come
// from ext-image-copy-capture on the adapter's own Wayland connection, region
// selection from the putgd-region layer-shell helper, the clipboard from data
// control. Nothing is chosen by compositor name: capabilities() reports which
// protocols the compositor actually offers.
class BudgieSourceAdapter final:public WaylandCaptureAdapter {
 Q_OBJECT
public:
 explicit BudgieSourceAdapter(QObject* parent=nullptr);
 ~BudgieSourceAdapter() override;
 QJsonObject capabilities()const override;
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setRecording(active,std::move(reply));}
 void bindShortcuts(const QHash<QString,QString>&);
 // Delayed capture (#1363): putgd-region's countdown mode draws the pill on the
 // overlay layer. Expiry uses the same visibility witness as pixel reads;
 // gtklock does not set logind's hint or publish a screensaver query.
 void sessionLocked(std::function<void(bool)>) override;
 // recording.windows: ext toplevel identifiers, the target a window recording
 // takes (no picker exists). Neither protocol gives geometry, so rect is empty.
 void listWindows(RecordingWindows::Reply) override;
 void clearShortcuts(){shortcuts_.clear();}
 // put.gd's own windows, refused by app ID: Wayland gives no PID for a toplevel.
 static bool ownAppId(const QString&);
private:
 friend struct BudgieCaptureTestAccess;
 // Private visibility seam lets the real-compositor test pause after freeze/copy.
 std::function<void(std::function<void(bool)>)> visibilityQuery_;
 void queryVisibility(std::function<void(bool)>);
 void captureActiveWindow(const ResourceLimits&,quint64 generation,std::function<void(QImage,Error)> complete) override;
 void probePointer(const QList<CaptureFrame>& frozen,std::function<void(QRectF,Error)> reply) override;
 void cancelPointer() override{pointer_.cancel();}
 void captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64,ResourceLimits,std::function<void(QList<CaptureFrame>,Error)>) override;
 CursorOutputProbe pointer_;BudgieShortcuts shortcuts_;
};
}
