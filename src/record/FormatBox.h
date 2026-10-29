#pragma once

#include <QComboBox>
#include <QApplication>
#include <QAbstractItemModel>
#include "ui/Ui.h"
#include <QAbstractItemView>
#include <algorithm>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>
#include <QWheelEvent>

namespace putgd::record {
class FormatDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static QString tradeoff(const QModelIndex& index) {
        const auto format = index.data(Qt::UserRole).toStringList();
        if (format.value(0) == "mp4") return format.value(1) == "h265" ? QStringLiteral("Smaller files · Limited browser compatibility") : QStringLiteral("Recommended for browser sharing");
        if (format.value(0) == "webm") return QStringLiteral("Browser playback · Older Safari unsupported");
        return format.value(0) == "gif" ? QStringLiteral("Silent animation · Larger files") : QStringLiteral("Silent animation · Smaller than GIF");
    }
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const QFontMetrics title(Ui::font()), description(Ui::font(Ui::Type::metadata));
        return {std::max(title.horizontalAdvance(index.data().toString().section(QStringLiteral(" · "), 0, 0)), description.horizontalAdvance(tradeoff(index))) + 2 * Ui::space(Ui::Space::m), title.height() + description.height() + Ui::space(Ui::Space::xs) + 2 * Ui::space(Ui::Space::s)};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& base, const QModelIndex& index) const override {
        auto option = base; initStyleOption(&option, index); option.text.clear();
        const auto* style = option.widget ? option.widget->style() : qApp->style();
        style->drawControl(QStyle::CE_ItemViewItem, &option, painter, option.widget);
        const auto& colors = Theme::current();
        const auto area = option.rect.adjusted(Ui::space(Ui::Space::m), Ui::space(Ui::Space::s), -Ui::space(Ui::Space::m), -Ui::space(Ui::Space::s));
        const int titleHeight = QFontMetrics(Ui::font()).height();
        Ui::paintText(*painter, QRect(area.left(), area.top(), area.width(), titleHeight), index.data().toString().section(QStringLiteral(" · "), 0, 0), colors.text, Ui::font(), Qt::AlignLeft | Qt::AlignVCenter);
        Ui::paintText(*painter, QRect(area.left(), area.top() + titleHeight + Ui::space(Ui::Space::xs), area.width(), QFontMetrics(Ui::font(Ui::Type::metadata)).height()), tradeoff(index), colors.textSecondary, Ui::font(Ui::Type::metadata), Qt::AlignLeft | Qt::AlignVCenter);
    }
};
// The closed choice leads with purpose; its popup and tooltip keep the codec
// and recommendation without making every review action row that wide.
class FormatBox final : public QComboBox {
public:
    explicit FormatBox(QWidget* parent = nullptr) : QComboBox(parent) {
        setItemDelegate(new FormatDelegate(this));
        const auto describe = [this] { setToolTip(currentData(Qt::ToolTipRole).toString()); setAccessibleDescription(currentText() + "\n" + toolTip()); };
        connect(this, &QComboBox::currentIndexChanged, this, describe);
        connect(model(), &QAbstractItemModel::dataChanged, this, describe);
    }
    QSize sizeHint() const override {
        auto size = QComboBox::sizeHint();
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(purpose(itemText(i))));
        size.setWidth(width + Ui::scaled(44));
        return size;
    }
    QSize minimumSizeHint() const override {
        auto size = sizeHint();
        if (!property("purposeMinimum").toBool()) {
            int compact = 0;
            for (int i = 0; i < count(); ++i) compact = std::max(compact, fontMetrics().horizontalAdvance(compactLabel(itemData(i).toStringList())));
            size.setWidth(std::min(size.width(), compact + Ui::scaled(44)));
        }
        return size;
    }
    void showPopup() override {
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(itemText(i)));
        view()->setMinimumWidth(width + Ui::scaled(40));
        QComboBox::showPopup();
    }
    QString displayedText() const {
        QStyleOptionComboBox option; initStyleOption(&option);
        const int available = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this).width();
        const auto label = purpose(currentText());
        if (fontMetrics().horizontalAdvance(label) <= available) return label;
        return compactLabel(currentData().toStringList());
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
    static QString compactLabel(const QStringList& value) {
        const auto format = value.value(0), codec = value.value(1);
        if (format == "mp4" && codec == "h264") return QStringLiteral("H.264");
        if (format == "mp4" && codec == "h265") return QStringLiteral("H.265");
        return format == "webm" ? QStringLiteral("WebM") : format == "webp" ? QStringLiteral("WebP") : format.toUpper();
    }
    static QString purpose(const QString& label) { return label.section(QStringLiteral(" — "), 0, 0); }
};
}
