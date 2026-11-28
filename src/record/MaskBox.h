#pragma once
#include <QComboBox>
#include "ui/Ui.h"
#include <QAbstractItemView>
#include <algorithm>

namespace putgd::record {
// The chooser and its popup use the same stable mask names.
class MaskBox final : public QComboBox {
public:
    explicit MaskBox(QWidget* parent = nullptr) : QComboBox(parent) { Ui::leaveWheelToPane(this); }
    QSize sizeHint() const override {
        auto size = QComboBox::sizeHint();
        int width = fontMetrics().horizontalAdvance(placeholderText());
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(itemText(i)));
        size.setWidth(width + Ui::scaled(44));
        return size;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
    void showPopup() override {
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(itemText(i)));
        view()->setMinimumWidth(width + Ui::scaled(40));
        QComboBox::showPopup();
    }
};
}
