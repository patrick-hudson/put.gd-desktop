#pragma once
#include "platform/Geometry.h"
#include "Graphite.h"
#include <cmath>
#include <QAccessible>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QWidget>
#include <algorithm>
#include <optional>
#include <functional>
#include <QElapsedTimer>
#include <QTimer>

namespace putgd {
// Native capture helpers keep the desktop family and its text enlargement.
inline QFont nativeRegionFont() {
    auto font = QGuiApplication::font();
    if (font.pointSizeF() > 0) font.setPointSizeF(std::max(qreal(10.5), font.pointSizeF()));
    else font.setPixelSize(std::max(14, font.pixelSize()));
    return font;
}
// Match composeFrames: preserve a single output's pixel grid; compose spans at
// the highest participating density, rounding boundaries outwards once.
inline QSize regionOutputSize(const QRectF& selection, const QList<CaptureFrame>& frames) {
    const CaptureFrame* single = nullptr;
    qreal density = 0;
    int count = 0;
    for (const auto& frame : frames) if (frame.logicalRect.intersects(selection)) {
        if (frame.logicalRect.isEmpty() || frame.nativeRect.isEmpty() || !std::isfinite(frame.density) || frame.density <= 0) return {};
        single = &frame;
        ++count;
        density = std::max(density, frame.density);
    }
    if (!count) return {};
    QRectF rect = selection;
    if (count == 1 && single->logicalRect.contains(selection)) {
        rect.translate(-single->logicalRect.topLeft());
        const qreal sx = single->nativeRect.width() / single->logicalRect.width(), sy = single->nativeRect.height() / single->logicalRect.height();
        return {int(std::ceil(rect.right() * sx) - std::floor(rect.left() * sx)), int(std::ceil(rect.bottom() * sy) - std::floor(rect.top() * sy))};
    }
    return {int(std::ceil(rect.right() * density) - std::floor(rect.left() * density)), int(std::ceil(rect.bottom() * density) - std::floor(rect.top() * density))};
}
inline QList<CaptureFrame> regionScreenFrames() {
    QList<CaptureFrame> frames;
    for (auto* screen : QGuiApplication::screens()) {
        const auto bounds = screen->geometry();
        const auto density = screen->devicePixelRatio();
        frames.append({screen->name(), bounds, QRect(QPoint(), QSizeF(bounds.size() * density).toSize()), density, 0, {}});
    }
    return frames;
}
inline QString regionInstructions(const QRectF& selection = {}, bool recording = false, const QString& display = {}, const QRectF& displayBounds = {}, const QList<CaptureFrame>& frames = regionScreenFrames(), bool dragging = false, const QString& outcome = {}) {
    const auto bounds = displayBounds.isEmpty() && QGuiApplication::primaryScreen() ? QRectF(QGuiApplication::primaryScreen()->geometry()) : displayBounds;
    const auto logical = selection.isEmpty() ? bounds : selection;
    const auto output = regionOutputSize(logical, frames);
    const auto size = output.isEmpty() ? logical.size() : QSizeF(output);
    const auto unit = output.isEmpty() ? QStringLiteral("logical px") : QStringLiteral("px");
    const bool selected = !selection.isEmpty();
    const bool multipleDisplays = frames.size() > 1;
    const QString context = (multipleDisplays ? (display.isEmpty() ? QStringLiteral("Display 1") : display) + QStringLiteral(" · ") : QString())
        + (selected ? QStringLiteral("%1 × %2 %3 · ") : QStringLiteral("Entire display · %1 × %2 %3 · "))
            .arg(qRound(size.width())).arg(qRound(size.height())).arg(unit);
    const QString accept = dragging ? (recording ? QStringLiteral("Release to choose recording area") : QStringLiteral("Release to capture")) : recording ? QStringLiteral("Enter to choose")
                                     : QStringLiteral("Enter to capture");
    const QString help = (dragging ? QString() : QStringLiteral("Drag to select · ")) + QStringLiteral("Arrows move · Shift+arrows resize")
        + (multipleDisplays ? QStringLiteral(" · Tab next display") : QString());
    return context + accept + QStringLiteral(" · Esc to cancel\n") + help
        + (recording ? QStringLiteral("\nStart recording comes next") : outcome.isEmpty() ? QString() : QStringLiteral("\n") + outcome);

}
inline QString regionDescription(const QRectF& selection, bool recording = false, const QString& display = {}, const QRectF& displayBounds = {}, const QList<CaptureFrame>& frames = regionScreenFrames(), bool dragging = false, const QString& outcome = {}) {
    return (selection.isEmpty() ? QStringLiteral("No region selected.")
        : QStringLiteral("Region %1 × %2 at %3, %4.").arg(selection.width()).arg(selection.height()).arg(selection.x()).arg(selection.y()))
        + QStringLiteral(" ") + regionInstructions(selection, recording, display, displayBounds, frames, dragging, outcome);
}
// Transfer the region's size and relative position, then clamp to the new display.
inline bool switchRegionDisplay(QRectF& selection, const QList<QRectF>& screens, int& current, int key, Qt::KeyboardModifiers modifiers = {}) {
    if (screens.isEmpty() || (key != Qt::Key_Tab && key != Qt::Key_Backtab)) return false;
    current = std::clamp(current, 0, int(screens.size()) - 1);
    const auto previous = screens[current];
    const int direction = key == Qt::Key_Backtab || modifiers.testFlag(Qt::ShiftModifier) ? -1 : 1;
    current = (current + direction + screens.size()) % screens.size();
    const auto next = screens[current];
    if (!selection.isEmpty()) {
        selection.translate(next.topLeft() - previous.topLeft());
        selection.setSize(QSizeF(std::min(selection.width(), next.width()), std::min(selection.height(), next.height())));
        selection.moveLeft(std::clamp(selection.left(), next.left(), next.right() - selection.width()));
        selection.moveTop(std::clamp(selection.top(), next.top(), next.bottom() - selection.height()));
    }
    return true;
}
// All selectors share the editor/review's pixel steps and screen bounds.
inline bool adjustRegion(QRectF& selection, const QRectF& screen, int key, Qt::KeyboardModifiers modifiers = {}) {
    QPoint step;
    switch (key) {
    case Qt::Key_Left: step = {-1, 0}; break;
    case Qt::Key_Right: step = {1, 0}; break;
    case Qt::Key_Up: step = {0, -1}; break;
    case Qt::Key_Down: step = {0, 1}; break;
    default: return false;
    }
    if (screen.width() < 1 || screen.height() < 1) return false;
    selection = selection.intersected(screen);
    if (selection.width() < 1 || selection.height() < 1) {
        const QSizeF size(std::max(qreal(1), screen.width() / 2), std::max(qreal(1), screen.height() / 2));
        selection = QRectF(screen.center() - QPointF(size.width() / 2, size.height() / 2), size);
        return true;
    }
    step *= modifiers & Qt::ControlModifier ? 10 : 1;
    if (modifiers & Qt::ShiftModifier) {
        selection.setWidth(std::clamp(selection.width() + step.x(), qreal(1), screen.right() - selection.left()));
        selection.setHeight(std::clamp(selection.height() + step.y(), qreal(1), screen.bottom() - selection.top()));
    } else {
        selection.moveLeft(std::clamp(selection.left() + step.x(), screen.left(), screen.right() - selection.width()));
        selection.moveTop(std::clamp(selection.top() + step.y(), screen.top(), screen.bottom() - selection.height()));
    }
    return true;
}
inline bool adjustRegion(QRectF& selection, const QRectF& screen, const QKeyEvent& event) {
    return adjustRegion(selection, screen, event.key(), event.modifiers());
}
enum class RegionKeyAction { Ignore, Change, Accept, Cancel };
inline RegionKeyAction regionKey(QRectF& selection, const QRectF& screen, int key, Qt::KeyboardModifiers modifiers = {}) {
    if (key == Qt::Key_Escape) return RegionKeyAction::Cancel;
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (selection.isEmpty()) selection = screen;
        return RegionKeyAction::Accept;
    }
    return adjustRegion(selection, screen, key, modifiers) ? RegionKeyAction::Change : RegionKeyAction::Ignore;
}
inline void describeRegion(QWidget* surface, const QRectF& selection, bool recording = false, const QString& display = {}, const QRectF& displayBounds = {}, const QList<CaptureFrame>& frames = regionScreenFrames(), bool dragging = false, const QString& outcome = {}) {
    const auto description = regionDescription(selection, recording, display, displayBounds, frames, dragging, outcome);
    if (surface->accessibleDescription() == description) return;
    surface->setAccessibleDescription(description);
    QAccessibleEvent changed(surface, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&changed);
}
// Selectors draw no scrim on any monitor (#629). A one-line hint pill shows at
// start and fades; afterwards only a compact output-pixel size label follows the
// selection. The full keyboard help and outcome stay in the accessible description.
inline constexpr int regionHintHoldMs=1700,regionHintFadeMs=300;
inline QString regionHint(){return QStringLiteral("Drag to select · Esc to cancel");}
inline qreal regionHintOpacity(qint64 elapsed){
    if(elapsed<=regionHintHoldMs)return 1;
    if(elapsed>=regionHintHoldMs+regionHintFadeMs)return 0;
    return 1-qreal(elapsed-regionHintHoldMs)/regionHintFadeMs;
}
// The same output-pixel size the capture produces, else logical pixels.
inline QString regionSizeLabel(const QRectF& selection,const QList<CaptureFrame>& frames=regionScreenFrames()){
    if(selection.isEmpty())return {};
    const auto output=regionOutputSize(selection,frames);
    const auto size=output.isEmpty()?selection.size():QSizeF(output);
    return QStringLiteral("%1 × %2").arg(qRound(size.width())).arg(qRound(size.height()));
}
// The display that shows the size label: the one holding most of the selection.
inline int regionLabelDisplay(const QList<QRectF>& displays,const QRectF& selection){
    int chosen=-1;qreal best=0;
    for(int i=0;i<displays.size();++i){const auto part=displays[i].intersected(selection);const qreal area=part.width()*part.height();if(area>best){best=area;chosen=i;}}
    return chosen;
}
inline QSize regionPillSize(const QFontMetrics& metrics,const QString& text){
    return {metrics.horizontalAdvance(text)+2*Graphite::space(Graphite::Space::m),metrics.height()+2*Graphite::space(Graphite::Space::xs)};
}
// Top centre of the display, or its bottom when the selection covers the top.
inline QRect regionHintBox(const QRect& bounds,const QFontMetrics& metrics,const QString& text=regionHint(),const QRectF& selection={}){
    const int margin=Graphite::space(Graphite::Space::l);
    const QRect available=bounds.adjusted(margin,margin,-margin,-margin);
    const auto size=regionPillSize(metrics,text);
    if(available.width()<size.width()||available.height()<size.height())return {};
    const QRect top(available.left()+(available.width()-size.width())/2,available.top(),size.width(),size.height());
    const QRect bottom(top.x(),available.bottom()-size.height()+1,size.width(),size.height());
    return QRectF(top).intersects(selection)&&!QRectF(bottom).intersects(selection)?bottom:top;
}
// Just outside the selection's bottom-left corner; above its top-left when the
// display has no room below, and inside the top-left corner when neither fits.
inline QRect regionSizeBox(const QRect& bounds,const QFontMetrics& metrics,const QString& text,const QRectF& selection){
    if(text.isEmpty()||selection.isEmpty())return {};
    const auto size=regionPillSize(metrics,text);
    if(bounds.width()<size.width()||bounds.height()<size.height())return {};
    const int gap=Graphite::space(Graphite::Space::xs);
    const auto outer=selection.toAlignedRect();
    int x=outer.left(),y=outer.bottom()+1+gap;
    if(y+size.height()-1>bounds.bottom()){y=outer.top()-gap-size.height();if(y<bounds.top()){x+=gap;y=outer.top()+gap;}}
    x=std::clamp(x,bounds.left(),bounds.right()-size.width()+1);
    y=std::clamp(y,bounds.top(),bounds.bottom()-size.height()+1);
    return {QPoint(x,y),size};
}
// An opaque, outlined pill stays legible over any desktop in either theme.
inline void paintRegionPill(QPainter& painter,const QRect& box,const QString& text,qreal opacity=1){
    if(box.isEmpty()||opacity<=0)return;
    painter.save();
    painter.setOpacity(painter.opacity()*opacity);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& colors=Theme::current();
    const qreal radius=box.height()/2.0;
    painter.setBrush(colors.surfaceRaised);
    painter.setPen(QPen(colors.borderStrong,1));
    painter.drawRoundedRect(QRectF(box).adjusted(0.5,0.5,-0.5,-0.5),radius,radius);
    painter.setPen(colors.text);
    painter.drawText(box,Qt::AlignCenter,text);
    painter.restore();
}
// Hint, then size label, in one surface's local coordinates. The hint shows
// only on the active display; the label only on the display chosen above.
inline void paintRegionChrome(QPainter& painter,const QRect& bounds,const QRectF& selection,const QString& size,bool hint,qreal hintOpacity,bool label){
    const QFontMetrics metrics(painter.font());
    if(hint)paintRegionPill(painter,regionHintBox(bounds,metrics,regionHint(),selection),regionHint(),hintOpacity);
    if(label)paintRegionPill(painter,regionSizeBox(bounds,metrics,size,selection),size);
}
// Drives the start-up hint's fade; repaint runs whenever its opacity changes.
class RegionHintFade {
public:
    explicit RegionHintFade(std::function<void()> repaint):repaint_(std::move(repaint)){
        timer_.setInterval(33);
        QObject::connect(&timer_,&QTimer::timeout,&timer_,[this]{const auto now=opacity();if(now!=last_){last_=now;repaint_();}if(now<=0)timer_.stop();});
    }
    void start(){clock_.start();expired_=false;last_=1;timer_.start();}
    void stop(){timer_.stop();}
    // Hide at once (tests, and selectors that close).
    void expire(){expired_=true;timer_.stop();}
    qreal opacity()const{return expired_?0:clock_.isValid()?regionHintOpacity(clock_.elapsed()):1;}
    bool visible()const{return opacity()>0;}
private:
    std::function<void()> repaint_;
    QElapsedTimer clock_;
    QTimer timer_;
    qreal last_=1;
    bool expired_=false;
};
}
