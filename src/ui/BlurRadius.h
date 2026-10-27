#pragma once
#include "Graphite.h"
#include "VisualStyle.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace putgd {
// Image-pixel radius in both editors; review can keep only the precise field.
class BlurRadius final : public QWidget {
public:
    BlurRadius(int minimum, int maximum, int value, bool compact, QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        label_ = new QLabel("Blur radius", this);
        VisualStyle::role(label_, "meta");
        if (compact) row->addWidget(label_);
        else layout->addWidget(label_);
        value_ = new QSpinBox(this);
        value_->setRange(minimum, maximum);
        value_->setSuffix(" px");
        value_->setValue(value);
        value_->setKeyboardTracking(false);
        value_->setAccessibleName("Blur radius");
        const auto description = QString("Blur radius in image pixels, %1 to %2").arg(minimum).arg(maximum);
        value_->setAccessibleDescription(description);
        value_->setToolTip(description);
        label_->setBuddy(value_);
        if (!compact) {
            slider_ = new QSlider(Qt::Horizontal, this);
            slider_->setRange(minimum, maximum);
            slider_->setValue(value);
            slider_->setAccessibleName("Blur radius");
            slider_->setAccessibleDescription(description);
            row->addWidget(slider_, 1);
            connect(slider_, &QSlider::valueChanged, value_, &QSpinBox::setValue);
            connect(value_, &QSpinBox::valueChanged, slider_, &QSlider::setValue);
        }
        row->addWidget(value_);
        layout->addLayout(row);
    }
    QSlider* slider() const { return slider_; }
    QSpinBox* spinBox() const { return value_; }
private:
    QLabel* label_ = nullptr;
    QSlider* slider_ = nullptr;
    QSpinBox* value_ = nullptr;
};
}
