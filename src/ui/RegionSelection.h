#pragma once
#include "platform/Geometry.h"
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

namespace putgd {
// Native capture helpers keep the desktop family and its text enlargement.
inline QFont nativeRegionFont() {
    auto font = QGuiApplication::font();
    if (font.pointSizeF() > 0) font.setPointSizeF(std::max(qreal(10.5), font.pointSizeF()));
    else font.setPixelSize(std::max(14, font.pixelSize()));
    return font;
}
inline constexpr int regionScrimAlpha = 70;
inline void paintRegionScrim(QPainter& painter, const QRect& bounds, const QRectF& selection) {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(bounds, QColor(0, 0, 0, regionScrimAlpha));
    if (!selection.isEmpty()) painter.fillRect(selection, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
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
        + (selected ? QStringLiteral("%1 × %2 %3 · ") : QStringLiteral("Entire display %1 × %2 %3 · "))
            .arg(qRound(size.width())).arg(qRound(size.height())).arg(unit);
    const QString accept = dragging ? (recording ? QStringLiteral("Release to choose recording area") : QStringLiteral("Release to capture")) : recording ? (selected ? QStringLiteral("Enter to choose") : QStringLiteral("Enter chooses this entire display"))
                                     : (selected ? QStringLiteral("Enter to capture") : QStringLiteral("Enter captures this entire display"));
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
inline QRect regionInstructionBox(const QRect& bounds, const QFontMetrics& metrics, const QString& instructions = regionInstructions(), const QRectF& selection = {}, const std::optional<QPointF>& pointer = {}, const QFont& font = nativeRegionFont()) {
    const QRect available = bounds.adjusted(16, 16, -16, -16);
    if (available.width() <= 20 || available.height() <= 12) return {};
    auto strong = font;
    strong.setWeight(QFont::DemiBold);
    const QFontMetrics titleMetrics(strong);
    const auto lines = instructions.split('\n');
    int naturalWidth = 0;
    for (int i = 0; i < lines.size(); ++i) naturalWidth = std::max(naturalWidth, (i == 0 ? titleMetrics : metrics).horizontalAdvance(lines[i]));
    const int width = std::min(available.width(), naturalWidth + 20);
    int textHeight = 0;
    for (int i = 0; i < lines.size(); ++i)
        textHeight += (i == 0 ? titleMetrics : metrics).boundingRect(QRect(0, 0, width - 20, available.height()), Qt::TextWordWrap | Qt::AlignCenter, lines[i]).height();
    const int height = std::min(available.height(), textHeight + 12);
    const QRect top(bounds.center().x() - width / 2, available.top(), width, height);
    const QRect bottom(top.x(), available.bottom() - height + 1, width, height);
    // Prefer clear content; near the drag pointer, avoid covering its next move.
    auto score = [&](const QRect& box) {
        const auto overlap = QRectF(box).intersected(selection);
        return overlap.width() * overlap.height() + (pointer && QRectF(box).adjusted(-24, -24, 24, 24).contains(*pointer) ? qreal(bounds.width()) * bounds.height() : 0);
    };
    return score(bottom) < score(top) ? bottom : top;
}
// An opaque, outlined backdrop stays legible over a live desktop in either theme.
inline void paintRegionInstructions(QPainter& painter, const QRect& bounds, const QPalette& palette, const QString& instructions = regionInstructions(), const QRectF& selection = {}, const std::optional<QPointF>& pointer = {}) {
    const QRect box = regionInstructionBox(bounds, painter.fontMetrics(), instructions, selection, pointer, painter.font());
    if (box.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(palette.color(QPalette::Window));
    painter.setPen(QPen(palette.color(QPalette::WindowText), 1));
    painter.drawRoundedRect(box.adjusted(0, 0, -1, -1), 6, 6);
    const auto base = painter.font();
    const auto lines = instructions.split('\n');
    int top = box.top() + 6;
    for (int i = 0; i < lines.size(); ++i) {
        auto font = base;
        font.setWeight(i == 0 ? QFont::DemiBold : QFont::Normal);
        painter.setFont(font);
        const auto ink = palette.color(QPalette::WindowText), background = palette.color(QPalette::Window);
        painter.setPen(i == 0 ? ink : QColor((ink.red() * 4 + background.red()) / 5, (ink.green() * 4 + background.green()) / 5, (ink.blue() * 4 + background.blue()) / 5));
        const QRect area(box.left() + 10, top, box.width() - 20, box.bottom() - top - 5);
        const int height = QFontMetrics(font).boundingRect(area, Qt::TextWordWrap | Qt::AlignCenter, lines[i]).height();
        painter.drawText(QRect(area.left(), top, area.width(), height), Qt::TextWordWrap | Qt::AlignCenter, lines[i]);
        top += height;
    }
    painter.restore();
}
}
