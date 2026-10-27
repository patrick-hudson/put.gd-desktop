#pragma once
#include <QComboBox>
#include <QAbstractItemView>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QWheelEvent>
#include <algorithm>

namespace putgd::record {
// The adjacent Mask label supplies the scope; the popup keeps each full name.
class MaskBox final : public QComboBox {
public:
    using QComboBox::QComboBox;
    QSize sizeHint() const override {
        auto size = QComboBox::sizeHint();
        int width = fontMetrics().horizontalAdvance(QStringLiteral("88"));
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(itemData(i).toString()));
        size.setWidth(width + 44);
        return size;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
    void showPopup() override {
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(itemText(i)));
        view()->setMinimumWidth(width + 40);
        QComboBox::showPopup();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        option.currentText = currentIndex() < 0 ? QStringLiteral("—") : currentData().toString();
        QPainter painter(this);
        style()->drawComplexControl(QStyle::CC_ComboBox, &option, &painter, this);
        style()->drawControl(QStyle::CE_ComboBoxLabel, &option, &painter, this);
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};
}
