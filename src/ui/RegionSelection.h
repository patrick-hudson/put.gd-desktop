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
// Both live selectors share the editor/review's pixel steps and screen bounds.
inline bool adjustRegion(QRectF& selection, const QRectF& screen, const QKeyEvent& event) {
    QPoint step;
    switch (event.key()) {
    case Qt::Key_Left: step = {-1, 0}; break;
    case Qt::Key_Right: step = {1, 0}; break;
    case Qt::Key_Up: step = {0, -1}; break;
    case Qt::Key_Down: step = {0, 1}; break;
    default: return false;
    }
    if (selection.isEmpty()) {
        selection = QRectF(screen.topLeft() + QPointF(screen.width() / 4, screen.height() / 4), screen.size() / 2);
        return true;
    }
    step *= event.modifiers() & Qt::ControlModifier ? 10 : 1;
    selection = selection.intersected(screen);
    if (event.modifiers() & Qt::ShiftModifier) {
        selection.setWidth(std::clamp(selection.width() + step.x(), qreal(1), screen.right() - selection.left()));
        selection.setHeight(std::clamp(selection.height() + step.y(), qreal(1), screen.bottom() - selection.top()));
    } else {
        selection.moveLeft(std::clamp(selection.left() + step.x(), screen.left(), screen.right() - selection.width()));
        selection.moveTop(std::clamp(selection.top() + step.y(), screen.top(), screen.bottom() - selection.height()));
    }
    return true;
}
inline void describeRegion(QWidget* surface, const QRectF& selection) {
    const auto description = regionDescription(selection);
    if (surface->accessibleDescription() == description) return;
    surface->setAccessibleDescription(description);
    QAccessibleEvent changed(surface, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&changed);
}
// An opaque, outlined backdrop stays legible over a live desktop in either theme.
inline void paintRegionInstructions(QPainter& painter, const QRect& bounds, const QPalette& palette) {
    const QRect available = bounds.adjusted(16, 16, -16, -16);
    const auto text = regionInstructions();
    const auto measured = painter.fontMetrics().boundingRect(available.adjusted(10, 6, -10, -6), Qt::TextWordWrap | Qt::AlignCenter, text);
    const int width = std::min(available.width(), measured.width() + 20);
    const QRect box(bounds.center().x() - width / 2, available.top(), width, measured.height() + 12);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(palette.color(QPalette::Window));
    painter.setPen(QPen(palette.color(QPalette::WindowText), 1));
    painter.drawRoundedRect(box.adjusted(0, 0, -1, -1), 6, 6);
    painter.setPen(palette.color(QPalette::WindowText));
    painter.drawText(box.adjusted(10, 6, -10, -6), Qt::TextWordWrap | Qt::AlignCenter, text);
    painter.restore();
}
}
