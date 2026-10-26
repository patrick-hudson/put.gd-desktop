#pragma once
#include <QAccessible>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QWidget>
#include <algorithm>

namespace putgd {
// Native capture helpers keep the desktop family and its text enlargement.
inline QFont nativeRegionFont() {
    auto font = QGuiApplication::font();
    if (font.pointSizeF() > 0) font.setPointSizeF(std::max(qreal(10.5), font.pointSizeF()));
    else font.setPixelSize(std::max(14, font.pixelSize()));
    return font;
}
inline QString regionInstructions(const QRectF& selection = {}, bool recording = false, const QString& display = {}) {
    const auto enter = recording ? (selection.isEmpty() ? QStringLiteral("Enter: use entire display") : QStringLiteral("Enter: use selected region"))
                                 : (selection.isEmpty() ? QStringLiteral("Enter: capture entire display") : QStringLiteral("Enter: capture selected region"));
    return (display.isEmpty() ? QString() : display + QStringLiteral(" · ")) + enter
        + QStringLiteral(" · Esc: cancel\nDrag or arrows to select · Shift+arrows: resize · Ctrl+arrows: 10 px · Tab / Shift+Tab: display");
}
inline QString regionDescription(const QRectF& selection, bool recording = false, const QString& display = {}) {
    return (selection.isEmpty() ? QStringLiteral("No region selected.")
        : QStringLiteral("Region %1 × %2 at %3, %4.").arg(selection.width()).arg(selection.height()).arg(selection.x()).arg(selection.y()))
        + QStringLiteral(" ") + regionInstructions(selection, recording, display);
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
inline void describeRegion(QWidget* surface, const QRectF& selection, bool recording = false, const QString& display = {}) {
    const auto description = regionDescription(selection, recording, display);
    if (surface->accessibleDescription() == description) return;
    surface->setAccessibleDescription(description);
    QAccessibleEvent changed(surface, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&changed);
}
inline QRect regionInstructionBox(const QRect& bounds, const QFontMetrics& metrics, const QString& instructions = regionInstructions()) {
    const QRect available = bounds.adjusted(16, 16, -16, -16);
    if (available.width() <= 20 || available.height() <= 12) return {};
    const auto measured = metrics.boundingRect(available.adjusted(10, 6, -10, -6), Qt::TextWordWrap | Qt::AlignCenter, instructions);
    const int width = std::min(available.width(), measured.width() + 20);
    return {bounds.center().x() - width / 2, available.top(), width, std::min(available.height(), measured.height() + 12)};
}
// An opaque, outlined backdrop stays legible over a live desktop in either theme.
inline void paintRegionInstructions(QPainter& painter, const QRect& bounds, const QPalette& palette, const QString& instructions = regionInstructions()) {
    const QRect box = regionInstructionBox(bounds, painter.fontMetrics(), instructions);
    if (box.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(palette.color(QPalette::Window));
    painter.setPen(QPen(palette.color(QPalette::WindowText), 1));
    painter.drawRoundedRect(box.adjusted(0, 0, -1, -1), 6, 6);
    painter.setPen(palette.color(QPalette::WindowText));
    painter.drawText(box.adjusted(10, 6, -10, -6), Qt::TextWordWrap | Qt::AlignCenter, instructions);
    painter.restore();
}
}
