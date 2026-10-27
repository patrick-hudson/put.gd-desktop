#pragma once

#include <QComboBox>
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
        connect(this, &QComboBox::currentIndexChanged, this, [this](int index) {
            setToolTip(itemData(index, Qt::ToolTipRole).toString());
        });
    }
    QSize sizeHint() const override {
        auto size = QComboBox::sizeHint();
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(purpose(itemText(i))));
        size.setWidth(width + 44);
        return size;
    }
    QSize minimumSizeHint() const override {
        auto size = sizeHint();
        size.setWidth(std::min(size.width(), fontMetrics().horizontalAdvance(QStringLiteral("MP4")) + 44));
        return size;
    }
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
        option.currentText = purpose(currentText());
        QPainter painter(this);
        style()->drawComplexControl(QStyle::CC_ComboBox, &option, &painter, this);
        style()->drawControl(QStyle::CE_ComboBoxLabel, &option, &painter, this);
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
private:
    static QString purpose(const QString& label) { return label.section(QStringLiteral(" — "), 0, 0); }
};
}
