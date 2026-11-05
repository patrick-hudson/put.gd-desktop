#pragma once
#include "Ui.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace putgd {
// The same joined image-pixel stepper in both editors; compact only changes
// label placement and omits the optional linked slider.
class BlurRadius final : public QWidget {
public:
    BlurRadius(int minimum, int maximum, int value, bool compact, QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(Ui::space(Ui::Space::xs));
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(Ui::space(Ui::Space::s));
        label_ = new QLabel("Blur radius", this);
        Ui::role(label_, "meta");
        if (compact) { row->addWidget(label_); row->addStretch(1); }
        else layout->addWidget(label_);
        value_ = new QSpinBox(this);
        value_->setRange(minimum, maximum);
        value_->setProperty("hasUnit", true);
        value_->setValue(value);
        value_->setKeyboardTracking(false);
        value_->setAccessibleName("Blur radius");
        const auto description = QString("Blur radius in image pixels, %1 to %2").arg(minimum).arg(maximum);
        value_->setAccessibleDescription(description);
        value_->setToolTip(description);
        label_->setBuddy(value_);
        auto* field = new QWidget(this);
        field->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* joined = new QHBoxLayout(field);
        joined->setContentsMargins(0, 0, 0, 0);
        joined->setSpacing(0);
        joined->addWidget(value_);
        unit_ = new QLabel("px", field);
        unit_->setObjectName("blurRadiusUnit");
        Ui::role(unit_, "unitSuffix");
        joined->addWidget(unit_);
        row->addWidget(field);
        if (!compact) row->addStretch(1);
        layout->addLayout(row);
        if (!compact) {
            slider_ = new QSlider(Qt::Horizontal, this);
            slider_->setRange(minimum, maximum);
            slider_->setValue(value);
            slider_->setAccessibleName("Blur radius");
            slider_->setAccessibleDescription(description);
            layout->addWidget(slider_);
            connect(slider_, &QSlider::valueChanged, value_, &QSpinBox::setValue);
            connect(value_, &QSpinBox::valueChanged, slider_, &QSlider::setValue);
        }
    }
    QLabel* label() const { return label_; }
    QLabel* unitLabel() const { return unit_; }
    QSlider* slider() const { return slider_; }
    QSpinBox* spinBox() const { return value_; }
private:
    QLabel* label_ = nullptr;
    QLabel* unit_ = nullptr;
    QSlider* slider_ = nullptr;
    QSpinBox* value_ = nullptr;
};
}
