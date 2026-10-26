#pragma once
#include <QAccessible>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QWidget>
#include <algorithm>

namespace putgd {
inline QString regionInstructions() {
    return QStringLiteral("Drag to select, or use the arrow keys · Enter to capture · Esc to cancel");
}
inline QString regionDescription(const QRectF& selection) {
    return (selection.isEmpty() ? QStringLiteral("No region selected. Enter captures the current screen.")
        : QStringLiteral("Region %1 × %2 at %3, %4.").arg(selection.width()).arg(selection.height()).arg(selection.x()).arg(selection.y()))
        + QStringLiteral(" Arrows move 1 px; Ctrl+arrows 10 px; Shift+arrows resize. Enter accepts; Esc cancels.");
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
inline void describeRegion(QWidget* surface, const QRectF& selection) {
    const auto description = regionDescription(selection);
    if (surface->accessibleDescription() == description) return;
    surface->setAccessibleDescription(description);
    QAccessibleEvent changed(surface, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&changed);
}
inline QRect regionInstructionBox(const QRect& bounds, const QFontMetrics& metrics) {
    const QRect available = bounds.adjusted(16, 16, -16, -16);
    if (available.width() <= 20 || available.height() <= 12) return {};
    const auto measured = metrics.boundingRect(available.adjusted(10, 6, -10, -6), Qt::TextWordWrap | Qt::AlignCenter, regionInstructions());
    const int width = std::min(available.width(), measured.width() + 20);
    return {bounds.center().x() - width / 2, available.top(), width, std::min(available.height(), measured.height() + 12)};
}
// An opaque, outlined backdrop stays legible over a live desktop in either theme.
inline void paintRegionInstructions(QPainter& painter, const QRect& bounds, const QPalette& palette) {
    const QRect box = regionInstructionBox(bounds, painter.fontMetrics());
    if (box.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(palette.color(QPalette::Window));
    painter.setPen(QPen(palette.color(QPalette::WindowText), 1));
    painter.drawRoundedRect(box.adjusted(0, 0, -1, -1), 6, 6);
    painter.setPen(palette.color(QPalette::WindowText));
    painter.drawText(box.adjusted(10, 6, -10, -6), Qt::TextWordWrap | Qt::AlignCenter, regionInstructions());
    painter.restore();
}
}
