#pragma once
#include "record/ReviewWindow.h"
#include "ui/Ui.h"
#include <QLabel>
#include <QLayout>
#include <QToolButton>
#include <algorithm>

namespace putgd::test {
// Shared by the widget gates and every rendered review state, including
// failures (#1613 layout): the footer keeps its inset, no two visible footer
// controls overlap, and the destination with its sharing mode stays visible
// and reachable with the full destination in its tooltip.
inline QString reviewFooterProblem(record::ReviewWindow& window) {
    auto* footer = window.findChild<QWidget*>("reviewFooter");
    auto* actions = window.findChild<QWidget*>("reviewActions");
    if (!footer || !actions) return "Review footer is missing";
    const QRect row(actions->mapTo(&window, QPoint()), actions->size());
    const int inset = window.height() - row.bottom() - 1;
    if (inset < 8) return QString("Review footer bottom inset is %1 px, expected at least 8").arg(inset);
    QList<QWidget*> leaves;
    for (auto* widget : actions->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        if (!widget->isWindow() && widget->isVisibleTo(&window)) leaves.append(widget);
    for (int i = 0; i < leaves.size(); ++i) {
        const QRect a(leaves[i]->mapTo(&window, QPoint()), leaves[i]->size());
        if (!window.rect().contains(a)) return QString("Review footer %1 lies outside the window").arg(leaves[i]->objectName());
        for (int j = i + 1; j < leaves.size(); ++j) {
            const QRect b(leaves[j]->mapTo(&window, QPoint()), leaves[j]->size());
            if (a.intersects(b)) return QString("Review footer %1 overlaps %2 at %3x%4")
                .arg(leaves[i]->objectName(), leaves[j]->objectName()).arg(window.width()).arg(window.height());
        }
    }
    const auto state = window.inspect();
    if (state["uploadSummary"].toString().isEmpty()) return {};
    auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
    if (!output || !output->isVisibleTo(&window) || !output->menu()
        || !window.rect().contains(QRect(output->mapTo(&window, QPoint()), output->size()))) return "Destination menu is not visible and reachable";
    const auto destination = state["destination"].toString();
    if (!output->toolTip().contains(destination) || !output->accessibleDescription().contains(destination)) return "Full destination is missing from tooltip or accessibility description";
    const auto suffix = output->property("sharingSuffix").toString();
    const auto mode = state["sharingMode"].toString() == "private" ? QStringLiteral("Private upload") : QStringLiteral("Shared link");
    if (!suffix.contains(mode) || output->width() < output->fontMetrics().horizontalAdvance(QStringLiteral("…") + suffix))
        return QString("Sharing mode is not fully visible: '%1' in %2 px").arg(suffix).arg(output->width());
    return {};
}
}
