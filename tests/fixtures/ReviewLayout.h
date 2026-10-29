#pragma once
#include "record/ReviewWindow.h"
#include "ui/Graphite.h"
#include <QLabel>
#include <QLayout>
#include <QToolButton>
#include <algorithm>

namespace putgd::test {
// Shared by the widget gates and every rendered review state, including failures.
inline QString reviewFooterProblem(record::ReviewWindow& window) {
    const auto bounds = [&](QWidget* widget) {
        const auto content = widget->layout() ? widget->layout()->contentsRect() : widget->rect();
        return content.translated(widget->mapTo(&window, QPoint()));
    };
    auto* footer = window.findChild<QWidget*>("reviewFooter");
    auto* actions = window.findChild<QWidget*>("reviewActions");
    if (!footer || !actions) return "Review footer is missing";
    const int inset = window.height() - bounds(actions).bottom() - 1;
    if (inset < 8) return QString("Review footer bottom inset is %1 px, expected at least 8").arg(inset);
    QList<QPair<QString, QRect>> rows;
    for (const auto* name : {"trimControls", "maskControls", "statusRow", "reviewDestinationLine", "reviewMetadataLine", "reviewActionLine"}) {
        auto* row = window.findChild<QWidget*>(name);
        if (row && row->isVisibleTo(&window)) rows.append({QString::fromLatin1(name), bounds(row)});
    }
    if (actions->layout()->property("singleRow").toBool()) rows.append({QStringLiteral("reviewActions"), bounds(actions)});
    // Every visible control and label in the footer keeps its own space.
    QList<QWidget*> leaves;
    for (auto* widget : actions->findChildren<QWidget*>()) {
        if (widget->isWindow() || !widget->isVisibleTo(&window)) continue;
        const auto children = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        if (std::none_of(children.cbegin(), children.cend(), [&](QWidget* child) { return !child->isWindow() && child->isVisibleTo(&window); })) leaves.append(widget);
    }
    for (int i = 0; i < leaves.size(); ++i)
        for (int j = i + 1; j < leaves.size(); ++j) {
            const QRect a(leaves[i]->mapTo(&window, QPoint()), leaves[i]->size()), b(leaves[j]->mapTo(&window, QPoint()), leaves[j]->size());
            if (a.intersects(b)) return QString("Review footer %1 %2 overlaps %3 %4 at %5x%6")
                .arg(leaves[i]->objectName().isEmpty() ? QString::fromLatin1(leaves[i]->metaObject()->className()) : leaves[i]->objectName())
                .arg(QString("%1,%2 %3x%4").arg(a.x()).arg(a.y()).arg(a.width()).arg(a.height()))
                .arg(leaves[j]->objectName().isEmpty() ? QString::fromLatin1(leaves[j]->metaObject()->className()) : leaves[j]->objectName())
                .arg(QString("%1,%2 %3x%4").arg(b.x()).arg(b.y()).arg(b.width()).arg(b.height()))
                .arg(window.width()).arg(window.height());
        }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.top() < b.second.top(); });
    for (int i = 1; i < rows.size(); ++i) {
        const int gap = rows[i].second.top() - rows[i - 1].second.bottom() - 1;
        if (gap < 4) return QString("Review rows %1 and %2 are %3 px apart, expected at least 4")
            .arg(rows[i - 1].first, rows[i].first).arg(gap);
    }
    auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
    const auto state = window.inspect();
    if (state["uploadSummary"].toString().isEmpty()) return {};
    if (!output || !output->isVisibleTo(&window) || !output->menu()
        || !window.rect().contains(QRect(output->mapTo(&window, QPoint()), output->size()))) return "Destination menu is not visible and reachable";
    const auto destination = state["destination"].toString();
    const auto mode = state["sharingMode"].toString() == "private" ? QStringLiteral("Private upload") : QStringLiteral("Shared link");
    if (!output->toolTip().contains(destination) || !output->accessibleDescription().contains(destination)) return "Full destination is missing from tooltip or accessibility description";
    const bool elided = output->property("elideDestination").toBool();
    const auto display = elided ? output->property("sharingSuffix").toString()
        : output->property("displayText").isValid() ? output->property("displayText").toString() : output->text();
    const int textWidth = output->width() - int(Graphite::Space::l);
    if (!display.contains(mode) || textWidth < output->fontMetrics().horizontalAdvance(elided ? QStringLiteral("…") + display : mode)
        || (!elided && output->fontMetrics().horizontalAdvance(display) > textWidth
            && output->height() < output->fontMetrics().boundingRect(QRect(0, 0, std::max(1, textWidth), 10000), Qt::TextWordWrap, display).height()))
        return QString("Sharing mode is not fully visible: '%1' in %2x%3 px (font %4 px, mode %5 px, elided %6)")
            .arg(display).arg(output->width()).arg(output->height()).arg(output->fontMetrics().height()).arg(output->fontMetrics().horizontalAdvance(mode)).arg(elided);
    return {};
}
}
