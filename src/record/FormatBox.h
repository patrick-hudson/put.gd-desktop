#pragma once

#include <QComboBox>
#include <QAbstractItemModel>
#include "ui/Graphite.h"
#include <QAbstractItemView>
#include <algorithm>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QWheelEvent>

namespace putgd::record {
// The closed choice leads with purpose; its popup and tooltip keep the codec
// and recommendation without making every review action row that wide.
class FormatBox final : public QComboBox {
public:
    explicit FormatBox(QWidget* parent = nullptr) : QComboBox(parent) {
        const auto describe = [this] { setToolTip(currentData(Qt::ToolTipRole).toString()); setAccessibleDescription(currentText() + "\n" + toolTip()); };
        connect(this, &QComboBox::currentIndexChanged, this, describe);
        connect(model(), &QAbstractItemModel::dataChanged, this, describe);
    }
    QSize sizeHint() const override {
        auto size = QComboBox::sizeHint();
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(purpose(itemText(i))));
        size.setWidth(width + Graphite::scaled(44));
        return size;
    }
    QSize minimumSizeHint() const override {
        auto size = sizeHint();
        if (!property("purposeMinimum").toBool()) size.setWidth(std::min(size.width(), fontMetrics().horizontalAdvance(QStringLiteral("MP4")) + Graphite::scaled(44)));
        return size;
    }
    void showPopup() override {
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(itemText(i)));
        view()->setMinimumWidth(width + Graphite::scaled(40));
        QComboBox::showPopup();
    }
    QString displayedText() const {
        QStyleOptionComboBox option; initStyleOption(&option);
        const int available = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this).width();
        const auto label = purpose(currentText());
        if (fontMetrics().horizontalAdvance(label) <= available) return label;
        const auto format = currentData().toStringList().value(0);
        return format == "webm" ? QStringLiteral("WebM") : format == "webp" ? QStringLiteral("WebP") : format.toUpper();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        option.currentText = displayedText();
        QPainter painter(this);
        style()->drawComplexControl(QStyle::CC_ComboBox, &option, &painter, this);
        style()->drawControl(QStyle::CE_ComboBoxLabel, &option, &painter, this);
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
private:
    static QString purpose(const QString& label) { return label.section(QStringLiteral(" — "), 0, 0); }
};
}
