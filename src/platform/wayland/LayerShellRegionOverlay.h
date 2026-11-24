#pragma once
#include "platform/Geometry.h"
#include "ui/RegionSelection.h"
#include <QObject>
#include <QWidget>
#include <QSet>
#include <thread>
#include <functional>
class QWindow;
class QScreen;
struct wl_callback;
namespace putgd {
// Delayed capture (#1363): maps the countdown pill as an overlay layer surface
// at the top centre of `screen`, or of the output the compositor picks (the
// active one) when none is given. It takes no keyboard and covers only itself.
// False when this build has no per-window layer shell.
bool showLayerShellCountdown(QWidget& pill,QScreen* screen=nullptr);
// Takes the pill off the screen so that a capture cannot contain it: a
// transparent frame is committed and rendered first (KWin animates a closing
// surface from its last frame, and a screenshot renders that animation), then
// the surface is unmapped and a display sync orders the caller after the
// compositor has processed it. acknowledged is false when that could not be
// confirmed. presented is false when the transparent frame was never rendered
// within a second: the compositor is not presenting this surface, as when the
// session is locked or the output is off, and the caller must not capture.
class CountdownPill;
void hideLayerShellCountdown(CountdownPill& pill,std::function<void(bool acknowledged,bool presented)> done);
class LayerShellRegionOverlay final:public QObject {
 Q_OBJECT
public:
 // frozen: frames carry the pixels captured before selection (#629), shown as
 // the backdrop; otherwise the layer surfaces are transparent over the live desktop.
 explicit LayerShellRegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr,bool recording=false,QString outcome={},bool frozen=false);
 ~LayerShellRegionOverlay() override;
 void start();void cancel(Error={"cancelled","Region selection cancelled",{}});
 // The first font metrics populate Qt's whole font database. This does it on a
 // worker; until it ends, surfaces map and draw the outline without the hint and
 // size pills, which follow. Call once after QApplication; join before it ends.
 [[nodiscard]] static std::thread preloadFonts();
 // Completes the selection with a logical rectangle, as a drag would (agent control).
 void select(const QRectF& logical);
 // A drag seen before these surfaces took input (kind: press, move or release; layout coordinates).
 void relayPointer(const QString& kind,QPointF logical);
 // Before start(): map without the keyboard, then accept it on demand once drawn
 // (Hyprland releases every held button when a surface takes it while mapping).
 void setDeferKeyboard(bool defer){deferKeyboard_=defer;}
 // Takes the keyboard outright and handles the key that asked for it.
 void takeKeyboard(int qtKey,int qtModifiers);
signals:
 void selected(const QRectF&);void cancelled(const putgd::Error&);
private:
 friend class WaylandRegionOverlayTest;
 bool addSurface(const CaptureFrame&,QScreen*);
 void accept();
 bool eventFilter(QObject*,QEvent*) override;
 class Surface;void press(QPointF);void move(QPointF);void release(QPointF);void finish();void dismiss();void surfacePainted(const void*);void acceptKeyboardOnDemand();QSet<const void*> drawn_;bool shown_=false,deferKeyboard_=false,keyboardTaken_=false;QSet<QWindow*> awaitingMapped_;
 int keyboardScreen_=-1;
 QList<CaptureFrame> frames_;QString outcome_;QList<Surface*> surfaces_;QRectF selection_;QPointF anchor_;bool dragging_=false,finished_=false,clearing_=false,dismissing_=false;QSet<QWindow*> awaitingFrames_;
 bool recording_=false,frozen_=false;
 int hintScreen_=0;
 RegionPaintStats stats_;bool logged_=false;
 void repaint();
 void fontsLoaded();
 RegionHintFade hint_{[this]{repaint();}};
 wl_callback* dismissal_=nullptr;
};
}
