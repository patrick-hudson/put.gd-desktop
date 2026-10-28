#include "ReviewWindow.h"
#include "Duration.h"
#include "FormatBox.h"
#include "MaskBox.h"
#include "ui/BlurRadius.h"
#include "EffectiveAudio.h"
#include <QFormLayout>
#include <QGridLayout>
#include <QFrame>
#include <QScrollArea>
#include <QAccessibleWidget>
#include <QScrollBar>
#include <QWheelEvent>
#include <QResizeEvent>
#include <limits>
#include "ClipExporter.h"
#include "platform/HostOpen.h"
#include <QClipboard>
#include <QFileInfo>
#include "DebugLog.h"
#include "Overlays.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QWidgetAction>
#include <QToolTip>
#include <QHelpEvent>
#include "ui/FileDialog.h"
#include "ui/Theme.h"
#include "ui/VisualStyle.h"
#include "ui/Graphite.h"
#include "ui/Fonts.h"
#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QFontMetricsF>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QSpinBox>
#include <QThread>
#include <QToolButton>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <utility>

namespace putgd::record {
namespace {
constexpr int handleSize = 8;
constexpr int compactReviewHeight = 648;
// Compact media chrome keeps its padding on the logical grid as text grows.
int reviewSpace(Graphite::Space value) { return static_cast<int>(value); }
int textControlHeight(const QWidget* widget) { return std::max(28, widget->fontMetrics().height()); }
int iconTarget() { return Graphite::Metric::iconTarget; }
}

namespace {
// Media overlays use fixed colours, not the app theme: they sit on captured
// content, which is as likely pale as dark (Graphite, Recording).
const QColor mediaAccent(0xE8, 0xB6, 0x6D);
QString seconds(qint64 us) {
    auto text = QString::number(double(us / 10000) / 100, 'f', 2);
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}
// A compact lane-header action keeps its words inside the shared gutter.
class AddMaskButton final : public QToolButton {
public:
    using QToolButton::QToolButton;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        style()->drawPrimitive(QStyle::PE_PanelButtonTool, &option, &painter, this);
        const auto colour = isEnabled() ? Theme::current().textSecondary : Theme::current().textDisabled;
        const auto labelRect = rect().adjusted(4, 0, -10, 0);
        auto label = text();
        if (fontMetrics().horizontalAdvance(label) > labelRect.width()) label.remove(0, 2);
        Graphite::paintText(painter, labelRect, label, colour, font(), Qt::AlignLeft | Qt::AlignVCenter);
        Graphite::paintChevron(painter, QPointF(width() - 9, height() / 2.0), 6, Qt::DownArrow, colour);
    }
};
class BlurChip final : public QToolButton {
public:
    explicit BlurChip(QWidget* parent) : QToolButton(parent) {
        setObjectName("reviewBlurChip");
        setText(QStringLiteral("Blur may be reversible · Use Redact"));
        setAccessibleName(text());
        setToolTip(text());
        setAutoRaise(true);
        setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setIcon(VisualStyle::icon("alert"));
        setFocusPolicy(Qt::StrongFocus);
        setProperty("driveAction", QJsonObject{{"action", "useSolidCover"}});
        VisualStyle::role(this, "meta");
        setProperty("graphiteDensity", "compact");
        setFixedHeight(textControlHeight(this));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto& palette = Theme::current();
        painter.setBrush(underMouse() && isEnabled() ? palette.selected : palette.warningSurface);
        painter.setPen(Graphite::keyboardFocus(this) ? QPen(palette.accent, 1) : QPen(Qt::NoPen));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Graphite::Metric::smallRadius, Graphite::Metric::smallRadius);
        const int gap = reviewSpace(Graphite::Space::xs), side = 16;
        VisualStyle::icon("alert", palette.textSecondary).paint(&painter, QRect(gap, (height() - side) / 2, side, side));
        Graphite::paintText(painter, rect().adjusted(side + gap * 2, 0, -gap, 0), text(), palette.textSecondary, font(), Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideNone);
    }
};
// The interval and warning are independent wrapping groups. Reserve their
// widest content, so selecting a mask never reflows the frame under a press.
class MaskInterval final : public QLabel {
public:
    explicit MaskInterval(QWidget* parent) : QLabel(parent), warning_(new BlurChip(this)) {
        auto policy = warning_->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        warning_->setSizePolicy(policy);
        warning_->hide();
    }
    QToolButton* warningButton() const { return warning_; }
    QSize sizeHint() const override { return {reservedWidth_, fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return sizeHint(); }
    void reserve(const QStringList& intervals, bool warning) {
        int width = fontMetrics().horizontalAdvance(QStringLiteral("—"));
        for (const auto& interval : intervals) width = std::max(width, fontMetrics().horizontalAdvance(interval));
        reservedWidth_ = width + reviewSpace(Graphite::Space::s);
        setMinimumWidth(reservedWidth_);
        warning_->setFixedWidth(warning_->fontMetrics().horizontalAdvance(warning_->text()) + 16 + 3 * reviewSpace(Graphite::Space::xs));
        auto policy = warning_->sizePolicy();
        policy.setRetainSizeWhenHidden(warning);
        warning_->setSizePolicy(policy);
    }
    void setInterval(const QString& interval, const QString& state) {
        setText(interval + (state.isEmpty() ? QString() : QStringLiteral(" · ") + state));
        setToolTip(text());
        setAccessibleName(text());
    }
    void setWarning(const QString& warning) {
        warning_->setVisible(!warning.isEmpty());
        setAccessibleDescription(warning);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        Graphite::paintText(painter, rect(), text().section(QStringLiteral(" · "), 0, 0), palette().color(foregroundRole()), font(), Qt::AlignRight | Qt::AlignVCenter, Qt::ElideNone);
    }
private:
    BlurChip* warning_;
    int reservedWidth_ = 0;
};
// Keep destination names intact. The footer moves metadata above its actions
// before this menu's label needs to wrap; the popup retains selectable details.
class DestinationMenu final : public QToolButton {
public:
    using QToolButton::QToolButton;
    int heightForWidth(int width) const override {
        const int padding = Graphite::Metric::splitZone + 24;
        if (fontMetrics().horizontalAdvance(text()) <= width - padding) return fontMetrics().height();
        return std::max(fontMetrics().height(), fontMetrics().boundingRect(QRect(0, 0, std::max(1, width - padding), 10000), Qt::TextWordWrap, text()).height());
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_PanelButtonTool, &option, &painter, this);
        const auto ink = isEnabled() ? Theme::current().textSecondary : Theme::current().textDisabled;
        const auto textRect = rect().adjusted(12, 0, -Graphite::Metric::splitZone - 12, 0);
        const auto display = property("displayText").isValid() ? property("displayText").toString() : text();
        painter.setFont(font());
        painter.setPen(ink);
        const int wrap = fontMetrics().horizontalAdvance(display) <= textRect.width() ? Qt::TextSingleLine : Qt::TextWordWrap;
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | wrap, display);
        Graphite::paintChevron(painter, QPointF(width() - Graphite::Metric::splitZone / 2.0 - 4, height() / 2.0), 8, Qt::DownArrow, ink);
    }
};
// The mask row: its gutter label, then groups of controls that wrap whole,
// under the first group, when the window is too narrow or the text too large
// for one line. Every control keeps its full size; the row grows instead.
class FlowRow final : public QWidget {
public:
    static int groupGap() { return reviewSpace(Graphite::Space::xs); }
    static int lineGap() { return reviewSpace(Graphite::Space::xs); }
    explicit FlowRow(QWidget* parent) : QWidget(parent) {}
    void setGutter(QWidget* label, int indent) { label->setParent(this); label_ = label; indent_ = indent; relayout(); }
    void addGroup(QWidget* group) { group->setParent(this); group->installEventFilter(this); groups_.append(group); }
    // The height at a width; reserved also counts hidden groups that keep their size.
    int heightFor(int width, bool reserved = false) const { return arrange(width, reserved, false); }
    int singleLineWidth(bool reserved) const {
        int x = indent_;
        for (auto* group : groups_) if (shown(group, reserved)) x += widthOf(group) + groupGap();
        return x - groupGap();
    }
    // The enclosing box layout asks for the height at the width it gives.
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return heightFor(width); }
    QSize sizeHint() const override { return {singleLineWidth(false), heightFor(width())}; }
    QSize minimumSizeHint() const override {
        int widest = 0;
        for (auto* group : groups_) if (shown(group, true)) widest = std::max(widest, widthOf(group));
        return {indent_ + widest, heightFor(width())};
    }
    void relayout() {
        // A newly shown group may still carry its previous layout minimum.
        for (auto* group : groups_) if (auto* row = group->layout()) {
            if (group->minimumHeight() > group->minimumSizeHint().height()) row->invalidate();
            row->activate();
        }
        const int required = arrange(width(), false, true);
        if (minimumHeight() != required) { setMinimumHeight(required); updateGeometry(); }
    }
protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::LayoutRequest) relayout();
        return QWidget::event(e);
    }
    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() == QEvent::ShowToParent || e->type() == QEvent::HideToParent) QCoreApplication::postEvent(this, new QEvent(QEvent::LayoutRequest));
        return false;
    }
    void resizeEvent(QResizeEvent* e) override { QWidget::resizeEvent(e); relayout(); }
private:
    static bool shown(QWidget* group, bool reserved) { return !group->isHidden() || (reserved && group->sizePolicy().retainSizeWhenHidden()); }
    static int widthOf(QWidget* group) { return std::max({group->sizeHint().width(), group->minimumSizeHint().width(), group->minimumWidth()}); }
    static int heightOf(QWidget* group) { return std::min(group->maximumHeight(), std::max({group->sizeHint().height(), group->minimumHeight(), qobject_cast<QLabel*>(group) ? 28 : 0})); }
    int arrange(int width, bool reserved, bool apply) const {
        // Wrap complete groups without compressing text or pointer targets.
        QList<QList<QPair<QWidget*, QRect>>> lines(1);
        int x = indent_;
        for (auto* group : groups_) {
            if (!shown(group, reserved) && !group->sizePolicy().retainSizeWhenHidden()) continue;
            const int w = group == groups_.last() && qobject_cast<QLabel*>(group) ? group->minimumSizeHint().width() : widthOf(group);
            if (x > indent_ && x + w > width) { lines.append(QList<QPair<QWidget*, QRect>>{}); x = indent_; }
            lines.last().append({group, QRect(x, 0, w, heightOf(group))});
            x += w + groupGap();
        }
        int top = 0;
        for (int i = 0; i < lines.size(); ++i) {
            int line = i == 0 ? 28 : 0;
            for (const auto& placed : lines[i]) line = std::max(line, placed.second.height());
            if (apply) {
                if (!lines[i].isEmpty()) {
                    auto& last = lines[i].last();
                    if (last.first == groups_.last() && qobject_cast<QLabel*>(last.first))
                        last.second.setWidth(std::max(last.second.width(), width - last.second.left()));
                }
                for (const auto& [group, rect] : lines[i]) group->setGeometry(rect.translated(0, top + (line - rect.height()) / 2));
                if (i == 0 && label_) label_->setGeometry(0, 0, indent_ - reviewSpace(Graphite::Space::s), line);
            }
            top += line + (i + 1 < lines.size() ? lineGap() : 0);
        }
        return top;
    }
    QWidget* label_ = nullptr;
    QList<QWidget*> groups_;
    int indent_ = 0;
};
// The card over the frame when a preview could not be decoded.
class ErrorCard final : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(255, 255, 255, 60), 1));
        painter.setBrush(QColor(0, 0, 0, 215));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    }
};
}
FrameView::FrameView(QWidget* parent, std::shared_ptr<MaskNames> names) : QWidget(parent), maskNames_(names ? std::move(names) : std::make_shared<MaskNames>()), nudge_(this) {
    setMinimumSize(320, 180);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Keyboard mask editing (#283 accessibility): the frame takes focus by Tab or click.
    setFocusPolicy(Qt::StrongFocus);
    nudge_.setSingleShot(true);
    nudge_.setInterval(350);
    connect(&nudge_, &QTimer::timeout, this, &FrameView::commitNudge);
    errorCard_ = new ErrorCard(this);
    errorCard_->setObjectName("previewError");
    auto* row = new QHBoxLayout(errorCard_);
    row->setContentsMargins(reviewSpace(Graphite::Space::l), reviewSpace(Graphite::Space::s), reviewSpace(Graphite::Space::s), reviewSpace(Graphite::Space::s));
    row->setSpacing(reviewSpace(Graphite::Space::m));
    errorText_ = new QLabel(errorCard_);
    errorText_->setObjectName("previewErrorText");
    errorText_->setWordWrap(true);
    errorText_->setTextFormat(Qt::PlainText);
    QPalette onMedia = errorText_->palette();
    onMedia.setColor(QPalette::WindowText, Theme::onMedia());
    errorText_->setPalette(onMedia);
    retry_ = new QPushButton(QStringLiteral("Retry preview"), errorCard_);
    retry_->setObjectName("retryPreview");
    retry_->setToolTip(QStringLiteral("Decode the frame at the playhead again"));
    row->addWidget(errorText_, 1);
    row->addWidget(retry_, 0, Qt::AlignVCenter);
    connect(retry_, &QPushButton::clicked, this, &FrameView::retryRequested);
    errorCard_->hide();
}
void FrameView::setFrame(const QImage& image, qint64 timeUs) { frame_ = image; timeUs_ = timeUs; setPreviewError({}); update(); }
void FrameView::setPreviewError(const QString& message) {
    error_ = message;
    announce();
    if (message.isEmpty()) { errorCard_->hide(); update(); return; }
    // A frame still on screen is an earlier one: say which, as well as dimming it.
    const QString text = frame_.isNull() ? message : message + QStringLiteral("\nShowing an earlier frame (%1).").arg(durationText(timeUs_));
    errorText_->setText(text);
    errorText_->setAccessibleName(text);
    errorCard_->show();
    placeErrorCard();
    update();
}
void FrameView::placeErrorCard() {
    if (errorCard_->isHidden()) return;
    const QRect area = (frame_.isNull() ? rect() : imageRect()).intersected(rect()).adjusted(8, 8, -8, -8);
    if (area.isEmpty()) return;
    auto* row = static_cast<QBoxLayout*>(errorCard_->layout());
    const auto margins = row->contentsMargins();
    const int width = std::min(area.width(), Graphite::scaled(640));
    const int inner = std::max(1, width - margins.left() - margins.right());
    const int buttonWidth = retry_->sizeHint().width();
    const int textWidth = inner - buttonWidth - row->spacing();
    // Measure the label's actual remaining width, including enlarged type.
    const bool stacked = textWidth < errorText_->fontMetrics().horizontalAdvance(QStringLiteral("Showing an earlier frame"));
    row->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    row->setAlignment(retry_, stacked ? Qt::AlignLeft : Qt::AlignVCenter);
    const int labelHeight = errorText_->heightForWidth(std::max(1, stacked ? inner : textWidth));
    const int contentHeight = stacked ? labelHeight + row->spacing() + retry_->sizeHint().height() : std::max(labelHeight, retry_->sizeHint().height());
    const int height = std::min(area.height(), contentHeight + margins.top() + margins.bottom());
    errorCard_->setGeometry(QRect(QPoint(area.center().x() - width / 2, area.center().y() - height / 2), QSize(width, height)));

}
void FrameView::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); placeErrorCard(); }
void FrameView::setMasks(const QList<RecordingMask>& masks, const QString& selected) {
    // A keyboard nudge not yet proposed keeps its place over the last answer.
    QRect pending;
    if (nudging_) for (const auto& m : masks_) if (m.id == selected_ && m.id == selected) pending = m.rect;
    if (selected != selected_) commitNudge();
    maskNames_->observe(masks);
    masks_ = masks;
    selected_ = selected;
    if (!pending.isNull()) for (auto& m : masks_) if (m.id == selected_) m.rect = pending;
    announce();
    update();
}
void FrameView::announce() {
    QString text = QStringLiteral("No mask selected. Choose one in the Masks list.");
    for (int i = 0; i < masks_.size(); ++i) {
        const auto& m = masks_[i];
        if (m.id != selected_) continue;
        text = QStringLiteral("%1 selected: %2 × %3 at %4, %5. Applies from %6 to %7. Arrows move, Shift+arrows resize, Ctrl+arrows 10 px; Delete removes, Escape deselects.")
            .arg(maskNames_->name(m)).arg(m.rect.width()).arg(m.rect.height()).arg(m.rect.x()).arg(m.rect.y()).arg(durationText(m.start), durationText(m.end));
        break;
    }
    if (!armed_.isEmpty()) text = armed_ == "solid" ? QStringLiteral("Drag on the frame to place the redaction, or press Enter for a centered one") : QStringLiteral("Drag on the frame to place the blur, or press Enter for a centered one");
    if (!error_.isEmpty()) text.prepend(error_ + QStringLiteral(". "));
    if (accessibleDescription() == text) return;
    setAccessibleDescription(text);
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}
QRect FrameView::imageRect() const {
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (source.isEmpty()) return rect();
    const QSize scaled = source.scaled(contentsRect().size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
}
QRect FrameView::bounds() const { return QRect(QPoint(0, 0), canvas_.isEmpty() ? frame_.size() : canvas_); }
QRect FrameView::defaultMaskRect() const {
    const QRect all = bounds();
    if (all.width() < 4 || all.height() < 4) return {};
    const QSize size(std::max(2, all.width() / 2), std::max(2, all.height() / 2));
    return QRect(QPoint((all.width() - size.width()) / 2, (all.height() - size.height()) / 2), size);
}
QPointF FrameView::toCanvas(const QPointF& p) const {
    const auto r = imageRect();
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (r.width() < 1 || r.height() < 1) return p;
    return QPointF((p.x() - r.x()) * source.width() / r.width(), (p.y() - r.y()) * source.height() / r.height());
}
QRectF FrameView::toView(const QRect& c) const {
    const auto r = imageRect();
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (source.isEmpty()) return c;
    const qreal sx = qreal(r.width()) / source.width(), sy = qreal(r.height()) / source.height();
    return QRectF(r.x() + c.x() * sx, r.y() + c.y() * sy, c.width() * sx, c.height() * sy);
}
namespace {
// Over arbitrary captured content: a dark scrim under light text, so labels and
// instructions read on pale and dark frames alike.
QRectF mediaLabelRect(const QPointF& at, const QString& text, const QRectF& bounds, bool badge = false) {
    const QFontMetricsF metrics(Graphite::font(badge ? Graphite::Type::maskBadge : Graphite::Type::metadata, true));
    QRectF box(at, QSizeF(std::min(metrics.horizontalAdvance(text) + (badge ? 8 : 12), bounds.width()), (badge ? metrics.tightBoundingRect(text).height() : metrics.height()) + 6));
    if (box.right() > bounds.right()) box.moveRight(bounds.right());
    if (box.bottom() > bounds.bottom()) box.moveBottom(bounds.bottom());
    return box;
}
void mediaLabel(QPainter& painter, const QPointF& at, const QString& text, const QRectF& bounds, bool badge = false) {
    const auto font = Graphite::font(badge ? Graphite::Type::maskBadge : Graphite::Type::metadata, true);
    const QRectF box = mediaLabelRect(at, text, bounds, badge);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 175));
    painter.drawRoundedRect(box, 4, 4);
    Graphite::paintText(painter, box.adjusted(badge ? 4 : 6, 0, badge ? -4 : -6, 0), text, Theme::onMedia(), font);
    painter.restore();
}
// A mask outline readable on any content: a dark line under a light (or the
// accent, when selected) one.
void dualOutline(QPainter& painter, const QRectF& r, const QColor& colour, qreal width, Qt::PenStyle style) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 170), width + 2, Qt::SolidLine));
    painter.drawRect(r);
    painter.setPen(QPen(colour, width, style));
    painter.drawRect(r);
}
}
void FrameView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Theme::current().well);
    const auto target = imageRect();
    Graphite::paintImageShadow(painter, target);
    painter.fillRect(target, Qt::black);
    if (!frame_.isNull()) painter.drawImage(target, frame_);
    Graphite::paintImageEdge(painter, target);
    // An earlier frame under a preview error is dimmed so it never reads as current.
    if (stale()) painter.fillRect(target, QColor(0, 0, 0, 140));
    const QRectF selectedView = [&] { for (const auto& m : masks_) if (m.id == selected_) return toView(m.rect); return QRectF(); }();
    if (!playing_) for (const auto& mask : masks_) {
        const bool active = mask.start <= timeUs_ && timeUs_ < mask.end;
        const bool selected = mask.id == selected_;
        const auto r = toView(mask.rect);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mask.kind == "solid" ? QBrush(QColor(0, 0, 0, active ? 0 : 60)) : QBrush(QColor(255, 255, 255, active ? 0 : 30)));
        painter.drawRect(r);
        dualOutline(painter, r, selected ? mediaAccent : active ? QColor(255, 255, 255, 230) : QColor(255, 255, 255, 150), selected ? 2 : 1, active ? Qt::SolidLine : Qt::DashLine);
        if (selected) { painter.setBrush(mediaAccent); painter.setPen(QPen(QColor(0, 0, 0, 170), 1)); painter.drawRect(QRectF(r.right() - handleSize, r.bottom() - handleSize, handleSize, handleSize)); }
    }
    // Keep identities at the edge; only the mask being edited gets a full badge.
    if (!playing_) for (const auto& mask : masks_) {
        const bool selected = mask.id == selected_;
        const bool full = selected || mask.id == hovered_;
        const auto r = toView(mask.rect);
        QString text = full ? maskNames_->name(mask) + (selected && !(mask.start <= timeUs_ && timeUs_ < mask.end) ? QStringLiteral(" · inactive") : QString())
                            : QString::number(maskNames_->number(mask));
        const QPointF at = r.topLeft() + QPointF(3, 3);
        auto label = mediaLabelRect(at, text, target, !full);
        bool badge = !full || label.width() > r.width() - 6 || label.height() > r.height() - 6;
        if (badge) {
            text = QString::number(maskNames_->number(mask));
            label = mediaLabelRect(at, text, target, true);
        }
        if (label.width() > r.width() - 6 || label.height() > r.height() - 6 || (!selected && label.intersects(selectedView))) continue;
        mediaLabel(painter, at, text, target, badge);
    }
    if (drag_ == Drag::Create) {
        // The area being dragged, clipped to the frame like the mask it becomes.
        const auto pending = QRectF(anchor_, current_).normalized().intersected(target);
        painter.setPen(Qt::NoPen);
        painter.setBrush(armed_ == "solid" ? QBrush(QColor(0, 0, 0, 120)) : QBrush(QColor(255, 255, 255, 60)));
        painter.drawRect(pending);
        dualOutline(painter, pending, mediaAccent, 1.5, Qt::DashLine);
    }
    if (!armed_.isEmpty())
        mediaLabel(painter, QPointF(target.left() + 8, target.top() + 8), armed_ == "solid" ? QStringLiteral("Drag on the frame to place the redaction, or press Enter for a centered one") : QStringLiteral("Drag on the frame to place the blur, or press Enter for a centered one"), target);
    if (Graphite::keyboardFocus(this)) { painter.setPen(QPen(Theme::current().accent, 2)); painter.setBrush(Qt::NoBrush); painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1)); }
}
void FrameView::focusInEvent(QFocusEvent* e) { QWidget::focusInEvent(e); update(); }
void FrameView::focusOutEvent(QFocusEvent* e) { commitNudge(); QWidget::focusOutEvent(e); update(); }
void FrameView::keyPressEvent(QKeyEvent* e) {
    auto mask = std::find_if(masks_.begin(), masks_.end(), [&](const RecordingMask& m) { return m.id == selected_; });
    if (e->key() == Qt::Key_Escape && mask != masks_.end() && armed_.isEmpty()) { commitNudge(); selected_.clear(); announce(); emit maskSelected({}); update(); return; }
    QPoint step;
    switch (e->key()) {
    case Qt::Key_Left: step = {-1, 0}; break;
    case Qt::Key_Right: step = {1, 0}; break;
    case Qt::Key_Up: step = {0, -1}; break;
    case Qt::Key_Down: step = {0, 1}; break;
    default: QWidget::keyPressEvent(e); return;
    }
    if (mask == masks_.end() || drag_ != Drag::None) { QWidget::keyPressEvent(e); return; }
    // The editor's steps: 1 px, 10 px with Ctrl; Shift resizes.
    step *= e->modifiers() & Qt::ControlModifier ? 10 : 1;
    if (!nudging_) nudgeFrom_ = mask->rect;
    nudging_ = true;
    const QRect all = bounds();
    QRect r = mask->rect;
    if (e->modifiers() & Qt::ShiftModifier) {
        // Resize from the bottom-right corner, as the grip does.
        r.setWidth(std::clamp(r.width() + step.x(), 2, std::max(2, all.right() + 1 - r.left())));
        r.setHeight(std::clamp(r.height() + step.y(), 2, std::max(2, all.bottom() + 1 - r.top())));
    } else {
        r.translate(step);
        r.moveLeft(std::clamp(r.left(), 0, std::max(0, all.width() - r.width())));
        r.moveTop(std::clamp(r.top(), 0, std::max(0, all.height() - r.height())));
    }
    mask->rect = r;
    announce();
    nudge_.start();
    update();
}
void FrameView::commitNudge() {
    if (!nudging_) return;
    nudging_ = false;
    nudge_.stop();
    for (const auto& mask : masks_) if (mask.id == selected_ && mask.rect != nudgeFrom_) emit maskMoved(mask.id, mask.rect);
}
void FrameView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    commitNudge();
    const auto p = e->position();
    if (!armed_.isEmpty()) { drag_ = Drag::Create; anchor_ = p; current_ = p; update(); return; }
    const RecordingMask* nearest = nullptr;
    qreal distance = std::numeric_limits<qreal>::max();
    for (const auto& mask : masks_) {
        const auto corner = toView(mask.rect).bottomRight() - QPointF(handleSize / 2, handleSize / 2);
        if (!QRectF(corner - QPointF(16, 16), QSizeF(32, 32)).contains(p)) continue;
        const auto delta = corner - p;
        const auto squared = delta.x() * delta.x() + delta.y() * delta.y();
        if (squared < distance || (squared == distance && mask.id == selected_)) { nearest = &mask; distance = squared; }
    }
    if (nearest) {
        selected_ = nearest->id; original_ = nearest->rect;
        drag_ = Drag::Resize; anchor_ = p;
        announce(); emit maskSelected(selected_); update(); return;
    }
    for (auto it = masks_.rbegin(); it != masks_.rend(); ++it) {
        const auto r = toView(it->rect);
        if (r.contains(p)) { selected_ = it->id; announce(); emit maskSelected(it->id); drag_ = Drag::Move; anchor_ = p; original_ = it->rect; update(); return; }
    }
    selected_.clear();
    announce();
    emit maskSelected({});
    update();
}
void FrameView::leaveEvent(QEvent* e) {
    hovered_.clear();
    update();
    QWidget::leaveEvent(e);
}
void FrameView::mouseMoveEvent(QMouseEvent* e) {
    QString hovered;
    for (auto it = masks_.crbegin(); it != masks_.crend(); ++it) if (toView(it->rect).contains(e->position())) { hovered = it->id; break; }
    if (hovered_ != hovered) { hovered_ = hovered; update(); }
    if (drag_ == Drag::None) return;
    const auto p = e->position();
    if (drag_ == Drag::Create) { current_ = p; update(); return; }
    const QPointF delta = toCanvas(p) - toCanvas(anchor_);
    auto mask = std::find_if(masks_.begin(), masks_.end(), [&](const RecordingMask& m) { return m.id == selected_; });
    if (mask == masks_.end()) return;
    const QRect bounds(QPoint(0, 0), canvas_.isEmpty() ? frame_.size() : canvas_);
    if (drag_ == Drag::Move) { QRect moved = original_.translated(delta.toPoint()); moved.moveLeft(std::clamp(moved.left(), 0, std::max(0, bounds.width() - moved.width()))); moved.moveTop(std::clamp(moved.top(), 0, std::max(0, bounds.height() - moved.height()))); mask->rect = moved; }
    else { QRect resized = original_; resized.setWidth(std::max(2, original_.width() + int(delta.x()))); resized.setHeight(std::max(2, original_.height() + int(delta.y()))); mask->rect = resized.intersected(bounds); }
    announce();
    update();
}
void FrameView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || drag_ == Drag::None) return;
    const auto p = e->position();
    const QRect bounds(QPoint(0, 0), canvas_.isEmpty() ? frame_.size() : canvas_);
    if (drag_ == Drag::Create) {
        const QRect r = QRectF(toCanvas(anchor_), toCanvas(p)).normalized().toAlignedRect().intersected(bounds);
        const auto kind = armed_;
        armed_.clear();
        setCursor(Qt::ArrowCursor);
        drag_ = Drag::None;
        if (r.width() >= 2 && r.height() >= 2) emit maskCreated(kind, r);
        update();
        return;
    }
    drag_ = Drag::None;
    for (const auto& mask : masks_) if (mask.id == selected_ && mask.rect != original_) emit maskMoved(mask.id, mask.rect);
}

namespace {
// The ruler and two readable lanes bound the timeline; extra masks remain
// reachable through the visible scrollbar and its named navigation control.
constexpr int trackPad = 16, trackHeight = 8;
int gripTarget() { return std::max(40, iconTarget()); }
int visibleLanes() { return 2; }
QFont gutterFont() { auto font = Graphite::font(Graphite::Type::caption); font.setWeight(QFont::Medium); return font; }
QFont rulerFont() { return Graphite::font(Graphite::Type::caption); }
int laneHeight() { return 16; }
int rowHeight() { return laneHeight() + 2; }
int rulerHeight() { return 16; }
int clipHeight() { return rulerHeight() + 10; }
constexpr qint64 minimumSpanUs = 10000;
}

namespace {
class TimelineAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit TimelineAccessible(ReviewTimeline* timeline) : QAccessibleWidget(timeline, QAccessible::Slider) {}
    void* interface_cast(QAccessible::InterfaceType type) override { return type == QAccessible::ValueInterface ? static_cast<QAccessibleValueInterface*>(this) : QAccessibleWidget::interface_cast(type); }
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Value) return QStringLiteral("%1 seconds of %2 seconds").arg(seconds(timeline()->playhead()), seconds(timeline()->duration()));
        return QAccessibleWidget::text(type);
    }
    QVariant currentValue() const override { return double(timeline()->playhead()) / 1000000; }
    QVariant minimumValue() const override { return 0.0; }
    QVariant maximumValue() const override { return double(timeline()->duration() - 1) / 1000000; }
    QVariant minimumStepSize() const override { return 0.1; }
    void setCurrentValue(const QVariant& value) override {
        if (!timeline()->isEnabled()) return;
        const double seconds = value.toDouble();
        if (!std::isfinite(seconds)) return;
        emit timeline()->scrubStarted();
        timeline()->setPlayhead(qRound64(std::clamp(seconds, 0.0, maximumValue().toDouble()) * 1000000));
        emit timeline()->scrubbed(timeline()->playhead());
    }
private:
    ReviewTimeline* timeline() const { return static_cast<ReviewTimeline*>(object()); }
};
QAccessibleInterface* reviewAccessible(const QString& name, QObject* object) {
    return name == QLatin1String("putgd::record::ReviewTimeline") ? new TimelineAccessible(static_cast<ReviewTimeline*>(object)) : nullptr;
}
}
ReviewTimeline::ReviewTimeline(QWidget* parent, std::shared_ptr<MaskNames> names) : QWidget(parent), maskNames_(names ? std::move(names) : std::make_shared<MaskNames>()) {
    static const bool installed = [] { QAccessible::installFactory(reviewAccessible); return true; }();
    Q_UNUSED(installed);
    clipLabel_ = new Graphite::ElidedLabel(this);
    clipLabel_->setObjectName("clipGroup");
    clipLabel_->hide();
    VisualStyle::role(clipLabel_, "meta");
    laneScroll_ = new QScrollBar(Qt::Vertical, this);
    laneScroll_->setObjectName("maskLaneScroll");
    laneScroll_->setAccessibleName("Mask lanes");
    laneScroll_->setPageStep(visibleLanes());
    laneScroll_->hide();
    moreMasks_ = new QToolButton(this);
    moreMasks_->setObjectName("showMoreMasks");
    moreMasks_->setAutoRaise(true);
    moreMasks_->setIcon(Graphite::chromeIcon("chevron-down"));
    moreMasks_->setIconSize(QSize(Graphite::scaled(18), Graphite::scaled(18)));
    moreMasks_->setFixedSize(iconTarget(), iconTarget());
    moreMasks_->setAccessibleName("Next mask lanes");
    moreMasks_->setToolTip("Scroll to the next mask lanes without changing the selected mask");
    moreMasks_->hide();
    previousMasks_ = new QToolButton(this);
    previousMasks_->setObjectName("previousMaskLanes");
    previousMasks_->setAutoRaise(true);
    previousMasks_->setIcon(Graphite::chromeIcon("chevron-up"));
    previousMasks_->setIconSize(QSize(Graphite::scaled(18), Graphite::scaled(18)));
    previousMasks_->setFixedSize(iconTarget(), iconTarget());
    previousMasks_->setAccessibleName("Previous mask lanes");
    previousMasks_->setToolTip("Scroll to the previous mask lanes without changing the selected mask");
    previousMasks_->hide();
    laneSummary_ = new QLabel(this);
    laneSummary_->setObjectName("maskLaneSummary");
    VisualStyle::role(laneSummary_, "meta");
    laneSummary_->hide();
    connect(previousMasks_, &QToolButton::clicked, this, &ReviewTimeline::showPreviousMasks);
    connect(moreMasks_, &QToolButton::clicked, this, &ReviewTimeline::showMoreMasks);
    connect(laneScroll_, &QScrollBar::valueChanged, this, [this] { placeNavigation(); update(); });
    setObjectName("timeline");
    setAccessibleName("Clip timeline playhead (seconds)");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}
void ReviewTimeline::setDuration(qint64 us) { duration_ = std::max<qint64>(1, us); trimEnd_ = std::min(trimEnd_, duration_); update(); }
void ReviewTimeline::setTrim(qint64 start, qint64 end) { trimStart_ = start; trimEnd_ = end; update(); }
void ReviewTimeline::setMasks(const QList<RecordingMask>& masks, const QString& selected, bool reveal) {
    maskNames_->observe(masks);
    const bool selectionChanged = selected_ != selected;
    masks_ = masks;
    selected_ = selected;
    const int before = lanes_;
    layoutLanes();
    laneScroll_->setRange(0, std::max(0, lanes_ - visibleLanes()));
    laneScroll_->hide();
    moreMasks_->setVisible(lanes_ > visibleLanes());
    previousMasks_->setVisible(lanes_ > visibleLanes());
    laneSummary_->setVisible(lanes_ > visibleLanes());
    if (selectionChanged || reveal) for (int i = 0; i < masks_.size(); ++i) if (masks_[i].id == selected_) {
        const int lane = lane_[i];
        if (lane < laneScroll_->value()) laneScroll_->setValue(lane);
        else if (lane >= laneScroll_->value() + visibleLanes()) laneScroll_->setValue(lane - visibleLanes() + 1);
    }
    if (lanes_ != before) updateGeometry();
    placeNavigation();
    update();
}
void ReviewTimeline::setPlayhead(qint64 us) {
    const auto previous = playhead_;
    playhead_ = std::clamp<qint64>(us, 0, duration_ - 1);
    if (previous != playhead_) { QAccessibleValueChangeEvent changed(this, double(playhead_) / 1000000); QAccessible::updateAccessibility(&changed); }
    update();
}
// A concurrent mask always gets its own lane; the viewport bounds their height.
void ReviewTimeline::layoutLanes() {
    lane_ = QList<int>(masks_.size(), 0);
    QList<int> order(masks_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return masks_[a].start < masks_[b].start; });
    QList<qint64> ends;
    for (const int i : order) {
        int lane = -1;
        for (int l = 0; l < ends.size(); ++l) if (ends[l] <= masks_[i].start) { lane = l; break; }
        if (lane < 0) { ends.append(0); lane = int(ends.size()) - 1; }
        ends[lane] = std::max(ends[lane], masks_[i].end);
        lane_[i] = lane;
    }
    lanes_ = int(ends.size());
}
int ReviewTimeline::gutterWidth() {
    return std::min(112, Graphite::scaled(96));
}
bool ReviewTimeline::transportAbove(int) const { return transport_ != nullptr; }
int ReviewTimeline::timelineTop() const { return transport_ ? transport_->height() : 0; }
QRect ReviewTimeline::trackRect() const {
    return QRect(gutterWidth(), timelineTop() + rulerHeight() + 1, std::max(1, width() - trackPad - gutterWidth()), trackHeight);
}
QRect ReviewTimeline::laneRect(int lane) const { const auto track = trackRect(); return QRect(track.left(), timelineTop() + clipHeight() + (lane - laneScroll_->value()) * rowHeight() + (rowHeight() - laneHeight()) / 2, track.width(), laneHeight()); }
int ReviewTimeline::lanesBottom() const { return timelineTop() + clipHeight() + std::min(lanes_, visibleLanes()) * rowHeight(); }
void ReviewTimeline::setLaneHeader(QWidget* header) { laneHeader_ = header; header->setParent(this); placeNavigation(); }
int ReviewTimeline::xFor(qint64 us) const { const auto track = trackRect(); return track.left() + int(double(track.width()) * std::clamp<qint64>(us, 0, duration_) / duration_); }
qint64 ReviewTimeline::timeAt(qreal x) const { const auto track = trackRect(); return std::clamp<qint64>(qint64((x - track.left()) * double(duration_) / track.width()), 0, duration_); }
QRect ReviewTimeline::maskRect(const QString& id) const {
    for (int i = 0; i < masks_.size(); ++i) {
        if (masks_[i].id != id) continue;
        if (lane_.value(i) < laneScroll_->value() || lane_.value(i) >= laneScroll_->value() + visibleLanes()) return {};
        const auto lane = laneRect(lane_.value(i));
        const int left = xFor(masks_[i].start);
        return QRect(left, lane.top(), std::max(3, xFor(masks_[i].end) - left), lane.height());
    }
    return {};
}
QRect ReviewTimeline::maskHitRect(const QString& id) const {
    const auto bar = maskRect(id);
    if (bar.isNull()) return {};
    // A generous invisible target, kept inside the timeline; the nearest lane wins where targets overlap.
    const int side = gripTarget();
    QRect target(bar.left() - side / 2, bar.center().y() - side / 2, std::max(side, bar.width() + side), side);
    if (target.bottom() >= height()) target.moveBottom(height() - 1);
    if (target.top() < 0) target.moveTop(0);
    return target;
}
QSize ReviewTimeline::sizeHint() const { return {480, minimumSizeHint().height()}; }
int ReviewTimeline::heightForLanes(int width, int lanes) const {
    Q_UNUSED(width);
    // Leave the clamped last target below the trim track and nearest its own lane.
    const int rows = clipHeight() + std::min(lanes, visibleLanes()) * rowHeight() + (lanes > 0 ? 3 : 0)
        + (lanes > visibleLanes() ? iconTarget() : 0);
    return timelineTop() + std::max(laneHeader_ ? clipHeight() + rowHeight() / 2 + (laneHeader_->height() + 1) / 2 : rulerHeight() + 10, rows);
}
QSize ReviewTimeline::minimumSizeHint() const { return {220, heightForLanes(width(), lanes_)}; }
void ReviewTimeline::paintEvent(QPaintEvent*) {
    // Graphite: a quiet retained interval between amber
    // two grip handles, lightly dimmed trimmed-away ends, mask bars in lanes
    // and the playhead with its marker.
    const auto& p = Theme::current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto track = trackRect();
    // Adapt the ruler to the available width; endpoints always remain visible.
    const int count = std::clamp(track.width() / std::max(1, QFontMetrics(rulerFont()).horizontalAdvance(durationText(duration_)) + 36), 1, 4);
    for (int i = 0; i <= count; ++i) {
        const auto at = duration_ * i / count;
        const int x = xFor(at);
        const int labelWidth = QFontMetrics(rulerFont()).horizontalAdvance(durationText(at)) + 2;
        const int left = std::clamp(x - labelWidth / 2, track.left(), std::max(track.left(), track.right() - labelWidth + 1));
        const auto label = QRect(left, timelineTop(), labelWidth, rulerHeight());
        Graphite::paintText(painter, label, durationText(at), p.textMuted, rulerFont(), Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideNone);
        painter.setPen(QPen(p.borderStrong, 1));
        painter.drawLine(x, timelineTop() + rulerHeight(), x, track.top() - 2);
    }
    const bool trimming = drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd;
    const qint64 start = trimming ? dragStart_ : trimStart_, end = trimming ? dragEnd_ : trimEnd_;
    painter.setPen(QPen(p.border, 1));
    painter.setBrush(p.surfaceRaised);
    painter.drawRoundedRect(QRectF(track).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    const QColor edge = isEnabled() ? p.accent : p.borderStrong;
    QColor kept = p.text;
    kept.setAlphaF(0.22);
    const QRectF retained(xFor(start), track.center().y() - 2, xFor(end) - xFor(start), 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kept);
    painter.drawRoundedRect(retained, 3, 3);
    painter.setPen(Qt::NoPen);
    for (const qint64 at : {start, end}) {
        const QRectF grip(xFor(at) - 5.5, track.top() - 3, 11, track.height() + 6);
        painter.setBrush(edge);
        painter.drawRoundedRect(grip, 2.5, 2.5);
        painter.setPen(QPen(p.onAccent, 1));
        painter.drawLine(QPointF(grip.center().x(), grip.top() + 5), QPointF(grip.center().x(), grip.bottom() - 5));
        painter.setPen(Qt::NoPen);
    }
    for (int i = 0; i < masks_.size(); ++i) {
        if (lane_.value(i) < laneScroll_->value() || lane_.value(i) >= laneScroll_->value() + visibleLanes()) continue;
        const auto& mask = masks_[i];
        const bool selected = mask.id == selected_;
        const bool retiming = selected && (drag_ == Drag::MaskStart || drag_ == Drag::MaskEnd || drag_ == Drag::MaskMove);
        const auto lane = laneRect(lane_.value(i));
        const int left = xFor(retiming ? dragStart_ : mask.start), right = xFor(retiming ? dragEnd_ : mask.end);
        const QRectF bar = QRectF(left, lane.top(), std::max(3, right - left), lane.height()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(selected ? QPen(p.accent, 1.5) : QPen(p.border, 1));
        painter.setBrush(selected ? p.warningSurface : p.selected);
        painter.drawRoundedRect(bar, Graphite::Metric::smallRadius, Graphite::Metric::smallRadius);
        // The kind at lane size: a blur is hatched, a solid cover filled.
        QColor ink = selected ? p.accent : p.textMuted;
        ink.setAlpha(mask.kind == "solid" ? 120 : 200);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mask.kind == "solid" ? QBrush(ink) : QBrush(ink, Qt::BDiagPattern));
        const auto fill = bar.adjusted(1.5, 1.5, -1.5, -1.5);
        painter.drawRoundedRect(mask.kind == "solid" ? fill : QRectF(fill.left(), fill.bottom() - 3, fill.width(), 3), 2, 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(selected ? p.accent : p.textMuted);
        painter.drawRoundedRect(QRectF(bar.left(), bar.top(), 2.5, bar.height()), 1, 1);
        if (selected) { painter.setBrush(p.accent); for (const int x : {left, right}) painter.drawRoundedRect(QRectF(x - 1.5, lane.top() - 2, 3, lane.height() + 4), 1, 1); }
        const QString name = maskNames_->name(mask);
        auto textArea = bar.adjusted(6, 0, -6, 0);
        if (mask.kind == "blur" && bar.width() > lane.height() + 12) {
            const int side = std::min(lane.height() - 2, Graphite::scaled(14));
            VisualStyle::icon("alert", selected ? p.accent : p.textMuted).paint(&painter, QRect(left + 4, lane.top() + (lane.height() - side) / 2, side, side));
            textArea.setLeft(textArea.left() + side + 4);
        }
        // Labels stay inside their own coverage without erasing interval pixels.
        if (QFontMetricsF(gutterFont()).horizontalAdvance(name) <= textArea.width() && textArea.height() >= QFontMetrics(gutterFont()).tightBoundingRect(name).height())
            Graphite::paintText(painter, textArea, name, p.text, gutterFont(), Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideNone);

    }
    // Keep the playhead inside the lane area, never beside the control rows.
    const int x = xFor(playhead_);
    painter.setPen(QPen(p.text, 1.5));
    painter.drawLine(QPointF(x, track.top()), QPointF(x, lanesBottom() - 1));
    painter.setPen(Qt::NoPen);
    painter.setBrush(p.text);
    painter.drawPolygon(QPolygonF{QPointF(x - 4, timelineTop() + rulerHeight()), QPointF(x + 4, timelineTop() + rulerHeight()), QPointF(x, timelineTop() + rulerHeight() + 5)});
    if (Graphite::keyboardFocus(this)) { painter.setPen(QPen(p.accent, 1)); painter.setBrush(Qt::NoBrush); painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5); }
}
void ReviewTimeline::setTransport(QWidget* transport) {
    transport_ = transport;
    transport_->setParent(this);
    clipLabel_->hide();
    placeNavigation();
    updateGeometry();
}
int ReviewTimeline::firstLane() const { return laneScroll_->value(); }
QString ReviewTimeline::laneSummary() const { return laneSummary_->text(); }
void ReviewTimeline::showMoreMasks() {
    if (lanes_ <= visibleLanes()) return;
    laneScroll_->setValue(std::min(laneScroll_->maximum(), laneScroll_->value() + visibleLanes()));
}
void ReviewTimeline::showPreviousMasks() {
    if (lanes_ <= visibleLanes()) return;
    laneScroll_->setValue(std::max(0, laneScroll_->value() - visibleLanes()));
}
void ReviewTimeline::placeNavigation() {
    laneSummary_->setText(QStringLiteral("Lanes %1–%2 of %3").arg(laneScroll_->value() + 1).arg(std::min(lanes_, laneScroll_->value() + visibleLanes())).arg(lanes_));
    laneSummary_->setToolTip(QStringLiteral("Lanes %1–%2 of %3").arg(laneScroll_->value() + 1).arg(std::min(lanes_, laneScroll_->value() + visibleLanes())).arg(lanes_));
    laneSummary_->setAccessibleDescription(laneSummary_->toolTip());
    previousMasks_->setEnabled(laneScroll_->value() > 0);
    moreMasks_->setEnabled(laneScroll_->value() < laneScroll_->maximum());
    if (transport_) {
        auto* row = static_cast<QBoxLayout*>(transport_->layout());
        row->setDirection(QBoxLayout::LeftToRight);
        rowTransportHeight_ = Graphite::scaled(28);
        transport_->setProperty("inlineWidth", 0);
        transport_->setProperty("aboveTimeline", true);
        transport_->setGeometry(0, 0, width(), rowTransportHeight_);
    }
    if (laneHeader_) laneHeader_->setGeometry(0, laneRect(laneScroll_->value()).center().y() - (laneHeader_->height() - 1) / 2, gutterWidth() - reviewSpace(Graphite::Space::xs), laneHeader_->height());
    const int side = iconTarget();
    previousMasks_->setGeometry(0, lanesBottom(), side, side);
    moreMasks_->setGeometry(side + 4, lanesBottom(), side, side);
    laneSummary_->setGeometry(side * 2 + 12, lanesBottom(), std::max(0, width() - side * 2 - 12), side);

}
void ReviewTimeline::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    placeNavigation();
    updateGeometry();
}
bool ReviewTimeline::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(e);
        const auto id = maskAt(help->pos());
        for (const auto& mask : masks_) if (mask.id == id) {
            QToolTip::showText(help->globalPos(), QStringLiteral("%1 · %2–%3").arg(maskNames_->name(mask), durationText(mask.start), durationText(mask.end)), this);
            return true;
        }
    }
    return QWidget::event(e);
}
void ReviewTimeline::wheelEvent(QWheelEvent* e) {
    if (laneScroll_->maximum() == 0) { e->ignore(); return; }
    laneScroll_->setValue(laneScroll_->value() + (e->angleDelta().y() < 0 ? 1 : -1));
    e->accept();
}
void ReviewTimeline::seek(qint64 us) { setPlayhead(us); emit scrubbed(playhead_); }
ReviewTimeline::Drag ReviewTimeline::handleAt(QPointF pos) const {
    Drag result = Drag::None;
    qreal nearest = std::numeric_limits<qreal>::max();
    const auto offer = [&](int x, int y, Drag kind) {
        const auto delta = pos - QPointF(x, y);
        const int side = gripTarget();
        const QRectF target(x - side / 2, std::clamp(y - side / 2, 0, std::max(0, height() - side)), side, side);
        if (!target.contains(pos)) return;
        const auto distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (distance < nearest) { nearest = distance; result = kind; }
    };
    // Trim and masks have separate interaction rows; nearest wins within a row.
    const auto row = maskAt(pos);
    offer(xFor(trimStart_), trackRect().center().y(), Drag::TrimStart);
    offer(xFor(trimEnd_), trackRect().center().y(), Drag::TrimEnd);
    if (const auto bar = maskRect(selected_); !bar.isNull() && row == selected_) {
        offer(bar.left(), bar.center().y(), Drag::MaskStart);
        offer(bar.left() + bar.width(), bar.center().y(), Drag::MaskEnd);
    }
    return result;
}
QString ReviewTimeline::maskAt(QPointF pos) const {
    if (pos.y() <= trackRect().bottom()) return {};
    QString result;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (const auto& mask : masks_) {
        if (!maskHitRect(mask.id).contains(pos.toPoint())) continue;
        const auto distance = std::abs(pos.y() - maskRect(mask.id).center().y());
        if (distance <= nearest) { nearest = distance; result = mask.id; }
    }
    return result;
}
void ReviewTimeline::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const auto pos = e->position();
    const auto handle = handleAt(pos);
    if (handle != Drag::None) {
        drag_ = handle;
        if (handle == Drag::MaskStart || handle == Drag::MaskEnd) {
            const auto& mask = *std::find_if(masks_.cbegin(), masks_.cend(), [this](const RecordingMask& m) { return m.id == selected_; });
            dragStart_ = mask.start; dragEnd_ = mask.end;
        } else { dragStart_ = trimStart_; dragEnd_ = trimEnd_; }
        emit scrubStarted();
        return;
    }
    const auto hit = maskAt(pos);
    if (!hit.isEmpty()) {
        const auto& mask = *std::find_if(masks_.cbegin(), masks_.cend(), [&](const RecordingMask& m) { return m.id == hit; });
        if (selected_ != hit) { selected_ = hit; emit maskSelected(selected_); }
        drag_ = Drag::MaskMovePending;
        dragStart_ = mask.start; dragEnd_ = mask.end;
        moveAnchor_ = timeAt(pos.x()) - mask.start;
        pressX_ = pos.x();
        update();
        return;
    }
    drag_ = Drag::Scrub;
    emit scrubStarted();
    seek(timeAt(pos.x()));
}
void ReviewTimeline::mouseMoveEvent(QMouseEvent* e) {
    const auto pos = e->position();
    if (drag_ == Drag::None) {
        setCursor(handleAt(pos) != Drag::None ? Qt::SizeHorCursor : !maskAt(pos).isEmpty() ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    const qint64 at = timeAt(pos.x());
    if (drag_ == Drag::Scrub) { seek(at); return; }
    if (drag_ == Drag::MaskMovePending) {
        if (std::abs(pos.x() - pressX_) < QApplication::startDragDistance()) return;
        drag_ = Drag::MaskMove;
        setCursor(Qt::ClosedHandCursor);
        emit scrubStarted();
    }
    if (drag_ == Drag::MaskMove) {
        // The whole interval follows the pointer, kept inside the clip; the frame
        // at its new start shows.
        const qint64 span = dragEnd_ - dragStart_;
        dragStart_ = std::clamp<qint64>(at - moveAnchor_, 0, duration_ - span);
        dragEnd_ = dragStart_ + span;
        seek(dragStart_);
        return;
    }
    // Moving an edge shows the frame at it, as the in/out buttons do.
    if (drag_ == Drag::TrimStart || drag_ == Drag::MaskStart) dragStart_ = std::clamp<qint64>(at, 0, dragEnd_ - minimumSpanUs);
    else dragEnd_ = std::clamp<qint64>(at, dragStart_ + minimumSpanUs, duration_);
    seek(drag_ == Drag::TrimStart || drag_ == Drag::MaskStart ? dragStart_ : dragEnd_);
}
void ReviewTimeline::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || drag_ == Drag::None) return;
    const auto drag = std::exchange(drag_, Drag::None);
    update();
    if ((drag == Drag::TrimStart || drag == Drag::TrimEnd) && (dragStart_ != trimStart_ || dragEnd_ != trimEnd_)) emit trimChanged(dragStart_, dragEnd_);
    if (drag == Drag::MaskMove || drag == Drag::MaskMovePending) setCursor(Qt::OpenHandCursor);
    if (drag == Drag::MaskStart || drag == Drag::MaskEnd || drag == Drag::MaskMove) {
        for (const auto& mask : masks_) if (mask.id == selected_ && (mask.start != dragStart_ || mask.end != dragEnd_)) emit maskRetimed(mask.id, dragStart_, dragEnd_);
    }
}
void ReviewTimeline::keyPressEvent(QKeyEvent* e) {
    // Arrows step a tenth of a second (a second with Shift); Home and End go to
    // the retained interval's first and last frame.
    const qint64 step = e->modifiers() & Qt::ShiftModifier ? 1000000 : 100000;
    switch (e->key()) {
    case Qt::Key_Left: emit scrubStarted(); seek(playhead_ - step); return;
    case Qt::Key_Right: emit scrubStarted(); seek(playhead_ + step); return;
    case Qt::Key_Home: emit scrubStarted(); seek(trimStart_); return;
    case Qt::Key_End: emit scrubStarted(); seek(trimEnd_ - 1); return;
    default: QWidget::keyPressEvent(e);
    }
}

ReviewWindow::ReviewWindow(const Options& options, const RecordingConfig& config, QWidget* parent) : QWidget(parent, Qt::Window), options_(options), config_(config), frameTimer_(this) {
    audioIntent_ = config_.audio.system;
    config_.audio.system = effectiveAudio(config_, options_.audioRecorded, audioIntent_);
    setObjectName("putgd-review");
    setWindowTitle("Review recording");
    // The Graphite controls (#198: in the interface's scheme).
    Fonts::load();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) VisualStyle::apply(*app);
    setAttribute(Qt::WA_DeleteOnClose, false);
    // #283: the frame takes the window; under it the timeline, the Trim and
    // contextual mask controls, any status and the footer.
    auto* frame = new QVBoxLayout(this);
    frame->setSizeConstraint(QLayout::SetNoConstraint);
    frame->setContentsMargins(0, 0, 0, 0);
    frame->setSpacing(0);
    // A one-line title and upload state preserve the frame area.
    header_ = Graphite::WindowHeader::install(this, frame, nullptr, Graphite::WindowHeader::Density::Compact);
    header_->findChild<Graphite::ElidedLabel*>("brandName")->setText(QStringLiteral("Review recording"));
    // Why the capture ended when it stopped itself (#225); hidden otherwise.
    auto* banner = new QVBoxLayout;
    banner->setContentsMargins(reviewSpace(Graphite::Space::l), 0, reviewSpace(Graphite::Space::l), 0);
    frame->addLayout(banner);
    notice_ = new QLabel(this);
    notice_->setObjectName("notice");
    VisualStyle::role(notice_, "notice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setAccessibleName("Why the recording stopped");
    notice_->hide();
    auto* noticeBand = new Graphite::Banner(Graphite::Banner::Tone::Info, {}, this);
    noticeBand->setContentWidget(notice_);
    noticeBand->hide();
    banner->addWidget(noticeBand);
    // Keep the well and image edge; fixed compact insets preserve the frame at large text.
    view_ = new FrameView(this, maskNames_);
    view_->setObjectName("frame");
    view_->setContentsMargins(reviewSpace(Graphite::Space::xs), 0, reviewSpace(Graphite::Space::xs), 0);
    view_->setMinimumHeight(180);
    view_->setCanvas(options.canvas);
    view_->setAccessibleName("Recording frame");
    frame->addWidget(view_, 1);
    // Compact bands leave room for the video even with enlarged text and status.
    auto* root = new QVBoxLayout;
    root->setContentsMargins(reviewSpace(Graphite::Space::m), 0, reviewSpace(Graphite::Space::m), 0);
    root->setSpacing(reviewSpace(Graphite::Space::xs));
    frame->addLayout(root);
    auto tool = [this](const char* icon, const QString& name, const QString& tip) {
        auto* button = new QToolButton(this);
        button->setIcon(VisualStyle::icon(icon));
        button->setIconSize(QSize(Graphite::scaled(Graphite::Metric::icon), Graphite::scaled(Graphite::Metric::icon)));
        button->setToolTip(tip);
        button->setAccessibleName(name);
        button->setAutoRaise(true);
        button->setProperty("graphiteDensity", "compact");
        button->setFixedSize(textControlHeight(button), textControlHeight(button));
        return button;
    };
    // Timing actions retain their words and grow with the text.
    auto action = [this](const QString& text, const QString& name, const QString& tip) {
        auto* button = name == "Add mask" ? new AddMaskButton(this) : new QToolButton(this);
        button->setText(text);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setAccessibleName(name);
        button->setToolTip(tip);
        button->setProperty("graphiteDensity", "compact");
        VisualStyle::role(button, "label");
        button->setFixedSize(int(std::ceil(QFontMetricsF(button->font()).horizontalAdvance(text))) + 24, textControlHeight(button));
        return button;
    };
    // A row's name in the shared gutter, beside its controls and level with the lanes' names.
    const int gutter = ReviewTimeline::gutterWidth();
    auto groupLabel = [this, gutter](const QString& text, const QString& name) { auto* label = new QLabel(text, this); label->setObjectName(name); VisualStyle::role(label, "meta"); label->setFixedWidth(gutter - 8); return label; };
    // Compact groups keep mask timing and its interval on the same line.
    auto group = [this](const QString& name, std::initializer_list<QWidget*> widgets) {
        auto* box = new QWidget(this);
        box->setObjectName(name);
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(reviewSpace(Graphite::Space::xs));
        for (auto* w : widgets) row->addWidget(w);
        return box;
    };
    // The timeline: the clip, one lane per mask; the full width under the frame.
    timeline_ = new ReviewTimeline(this, maskNames_);
    timeline_->setMinimumWidth(220);
    timeline_->setDuration(options.durationUs);
    timeline_->setToolTip("Click or drag to scrub; drag the trim handles to trim. Drag a mask to move it in time, or a selected mask's ends to retime them. Arrows step, Home/End jump to the kept part.");
    root->addWidget(timeline_);
    // Trim controls and the actual retained boundaries wrap as whole groups.
    trimStart_ = action("Start here", "Set trim start at playhead", "Keep from the playhead: set the trim start here (I)");
    trimEnd_ = action("End here", "Set trim end at playhead", "Keep up to the playhead: set the trim end here (O)");
    resetTrim_ = action("Reset", "Reset trim", "Keep the whole recording");
    retained_ = new QLabel(this);
    retained_->setObjectName("muted");
    retained_->setAccessibleName("Kept interval");
    auto* trimFlow = new FlowRow(this);
    trimGroup_ = trimFlow;
    trimGroup_->setObjectName("trimControls");
    trimFlow->setGutter(groupLabel("Trim", "trimGroup"), gutter);
    trimFlow->addGroup(group("trimTimingControls", {trimStart_, trimEnd_, resetTrim_}));
    VisualStyle::role(retained_, "meta");
    retained_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    trimFlow->addGroup(retained_);
    root->addWidget(trimGroup_);
    // Mask: the kinds to add, the mask to edit, its blur radius, its timing and
    // its interval, wrapping as whole groups when the row is too narrow.
    addMask_ = action("+ Add mask", "Add mask", "Add a redaction or blur to the frame");
    VisualStyle::role(addMask_, "meta");
    addMask_->setAutoRaise(true);
    addMask_->setObjectName("addMask");
    addMask_->setPopupMode(QToolButton::InstantPopup);
    addMask_->setFixedSize(gutter - reviewSpace(Graphite::Space::xs), textControlHeight(addMask_));
    auto* maskMenu = new QMenu(addMask_);
    addMask_->setMenu(maskMenu);
    addSolid_ = maskMenu->addAction(VisualStyle::icon("redact"), "Add redaction");
    addBlur_ = maskMenu->addAction(VisualStyle::icon("blur"), "Add blur");
    addSolid_->setToolTip("Drag on the frame to place a redaction, or press Enter for a centered one");
    addBlur_->setToolTip("Blur may be reversible. Use Redact for private information.");
    masks_ = new MaskBox(this);
    masks_->setObjectName("masks");
    masks_->setAccessibleName("Masks");
    masks_->setToolTip("The mask to edit; click one on the frame or the timeline to select it");
    masks_->setPlaceholderText("Select a mask");
    masks_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    masks_->setMinimumWidth(masks_->fontMetrics().horizontalAdvance(QStringLiteral("88")) + Graphite::Metric::splitZone + 18);
    auto* radius = new BlurRadius(2, 64, 16, true, this);
    strengthControl_ = radius;
    VisualStyle::role(radius->unitLabel(), "meta");
    strength_ = radius->spinBox();
    // Two digits plus the field padding and two full 32 px stepper targets.
    strength_->setFixedWidth(strength_->fontMetrics().horizontalAdvance(QString::number(strength_->maximum())) + 80);
    maskStart_ = action("Start here", "Set mask start at playhead", "Start the selected mask at the playhead (Shift+I)");
    maskEnd_ = action("End here", "Set mask end at playhead", "End the selected mask at the playhead (Shift+O)");
    removeMask_ = tool("remove", "Delete mask", "Delete the selected mask (Delete)");
    removeMask_->setText("Delete mask");
    VisualStyle::role(removeMask_, "label");
    removeMask_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    removeMask_->setFixedSize(int(std::ceil(QFontMetricsF(removeMask_->font()).horizontalAdvance(removeMask_->text()))) + removeMask_->iconSize().width() + 40, textControlHeight(removeMask_));
    maskOptions_ = tool("settings", "Mask options", "Mask options: blur radius and Delete mask");
    maskOptions_->setObjectName("maskOptions");
    maskOptions_->setFixedHeight(maskStart_->height());
    maskOptions_->setPopupMode(QToolButton::InstantPopup);
    auto* optionsMenu = new QMenu(maskOptions_);
    optionsMenu->setObjectName("maskOptionsMenu");
    auto* optionsAction = new QWidgetAction(optionsMenu);
    auto* optionsPanel = new QWidget(optionsMenu);
    auto* optionsLayout = new QVBoxLayout(optionsPanel);
    optionsLayout->setContentsMargins(reviewSpace(Graphite::Space::s), reviewSpace(Graphite::Space::s), reviewSpace(Graphite::Space::s), reviewSpace(Graphite::Space::s));
    optionsLayout->setSpacing(reviewSpace(Graphite::Space::s));
    auto* blurAdvice = new Graphite::Banner(Graphite::Banner::Tone::Warning, Graphite::blurWarningText(), optionsPanel);
    blurAdvice->setObjectName("reviewBlurAdvice");
    blurAdvice->setMaximumWidth(480);
    blurAdvice->setAction("Use Redact");
    blurAdvice->actionButton()->setObjectName("useRedact");
    blurAdvice->actionButton()->setProperty("driveAction", QJsonObject{{"action", "useSolidCover"}});
    blurAdvice->setDismissible(true, "Got it");
    blurAdvice->dismissButton()->setProperty("driveAction", QJsonObject{{"action", "dismissBlurTip"}});
    connect(blurAdvice->actionButton(), &QPushButton::clicked, this, [this, optionsMenu] { optionsMenu->close(); drive({{"action", "useSolidCover"}}); });
    connect(blurAdvice->dismissButton(), &QToolButton::clicked, this, [this, optionsMenu] { optionsMenu->close(); drive({{"action", "dismissBlurTip"}}); });
    optionsLayout->addWidget(blurAdvice);
    optionsLayout->addWidget(radius);
    optionsLayout->addWidget(removeMask_, 0, Qt::AlignLeft);
    optionsAction->setDefaultWidget(optionsPanel);
    optionsMenu->addAction(optionsAction);
    maskOptions_->setMenu(optionsMenu);
    connect(removeMask_, &QToolButton::clicked, optionsMenu, &QMenu::close);
    maskActive_ = new MaskInterval(this);
    maskActive_->setObjectName("maskActiveInterval");
    VisualStyle::role(maskActive_, "meta");
    connect(static_cast<MaskInterval*>(maskActive_)->warningButton(), &QToolButton::clicked, this, [this] { drive({{"action", "useSolidCover"}}); });
    auto* flow = new FlowRow(this);
    flow->setObjectName("maskControls");
    maskGroup_ = flow;
    maskTools_ = group("maskKinds", {masks_, maskOptions_});
    timeline_->setLaneHeader(addMask_);
    maskTiming_ = group("maskTimingControls", {maskStart_, maskEnd_});
    for (auto* w : std::initializer_list<QWidget*>{maskTools_, maskTiming_, static_cast<MaskInterval*>(maskActive_)->warningButton(), maskActive_}) flow->addGroup(w);
    flow->setGutter(groupLabel("Mask", "maskGroup"), gutter);
    root->addWidget(flow);
    // Playback has its own row, leaving every ruler tick readable.
    play_ = tool("play", "Play", "Play the retained part silently (Space)");
    play_->setObjectName("play");
    speed_ = VisualStyle::choiceBox(this);
    speed_->setObjectName("speed");
    speed_->setToolTip("Playback speed of the preview");
    speed_->setAccessibleName("Playback speed");
    for (const auto& [label, factor] : std::initializer_list<std::pair<const char*, double>>{{"0.5×", 0.5}, {"1×", 1.0}, {"2×", 2.0}, {"4×", 4.0}}) speed_->addItem(QString::fromUtf8(label), factor);
    speed_->setCurrentIndex(1);
    speed_->setFixedWidth(Graphite::scaled(64));
    speed_->setFrame(false);
    // Mono keeps the timecode columns steady while scrubbing.
    time_ = new Graphite::ElidedLabel(this);
    time_->setObjectName("time");
    VisualStyle::role(time_, "secondary");
    Graphite::setMono(time_, Graphite::Type::technical);
    time_->setAccessibleName("Playhead time");
    time_->setMinimumWidth(time_->fontMetrics().horizontalAdvance(QStringLiteral("%1 / %1").arg(durationText(options.durationUs))) + 4);
    playbackStatus_ = new Graphite::ElidedLabel(this);
    playbackStatus_->setText("Outside kept part");
    playbackStatus_->setObjectName("playbackStatus");
    VisualStyle::role(playbackStatus_, "meta");
    playbackStatus_->setMinimumWidth(int(std::ceil(QFontMetricsF(playbackStatus_->font()).horizontalAdvance(playbackStatus_->text()))) + 2);
    // Reserved, so crossing a trim edge never moves the row.
    auto notePolicy = playbackStatus_->sizePolicy();
    notePolicy.setRetainSizeWhenHidden(true);
    playbackStatus_->setSizePolicy(notePolicy);
    playbackStatus_->show();
    time_->setFixedHeight(time_->fontMetrics().tightBoundingRect(QStringLiteral("0:00.0/0:00.0")).height() + 1);
    playbackStatus_->setFixedHeight(playbackStatus_->fontMetrics().tightBoundingRect(playbackStatus_->text()).height());
    transport_ = new QWidget(this);
    transport_->setObjectName("reviewTransport");
    auto* transport = new QHBoxLayout(transport_);
    transport->setContentsMargins(0, 0, 0, 0);
    transport->setSpacing(reviewSpace(Graphite::Space::xs));
    transport->addWidget(play_);
    transport->addWidget(time_);
    transport->addWidget(playbackStatus_);
    transport->addWidget(speed_);
    transport->addStretch(1);
    if (Graphite::WindowHeader::customFrame()) {
        header_->setAccessibleName("Review recording");
        header_->setCentre(transport_);
    } else frame->insertWidget(frame->indexOf(view_), transport_);
    // Status belongs below the frame, so no guidance or recovery action hides video.
    statusRow_ = new QWidget(this);
    statusRow_->setObjectName("statusRow");
    statusLines_ = new QVBoxLayout(statusRow_);
    statusLines_->setContentsMargins(reviewSpace(Graphite::Space::m), 0, reviewSpace(Graphite::Space::m), 0);
    statusLines_->setSpacing(reviewSpace(Graphite::Space::xs));
    statusRow_->hide();
    frame->addWidget(statusRow_);
    // One footer row when its measured content fits; metadata moves above otherwise.
    format_ = new FormatBox(this);
    format_->setAccessibleName("Output format");
    for (const auto& format : options.formats) {
        const auto codecs = options.codecs[format].toArray();
        if (codecs.isEmpty()) { format_->addItem(formatLabel(format, {}), QStringList{format, QString()}); format_->setItemData(format_->count() - 1, formatLabel(format, {}) + "\n" + formatHint(format, {}), Qt::ToolTipRole); }
        for (const auto& codec : codecs) { format_->addItem(formatLabel(format, codec.toString()), QStringList{format, codec.toString()}); format_->setItemData(format_->count() - 1, formatLabel(format, codec.toString()) + "\n" + formatHint(format, codec.toString()), Qt::ToolTipRole); }
    }
    formatAdvice_ = tool("info", "Format tips", "Format tips: compatibility and audio for each format");
    formatAdvice_->setObjectName("formatAdvice");
    formatAdvice_->setProperty("driveAction", QJsonObject{{"action", "showFormatTips"}});
    connect(formatAdvice_, &QToolButton::clicked, this, [this] { drive({{"action", "showFormatTips"}}); });
    // What the export carries for sound, in a few words that never elide.
    audio_ = new QLabel(this);
    audio_->setObjectName("audioSummary");
    VisualStyle::role(audio_, "meta");
    audio_->hide();
    exportAudio_ = new QLabel(this);
    exportAudio_->setObjectName("exportAudioSummary");
    VisualStyle::role(exportAudio_, "meta");
    exportAudio_->setWordWrap(true);
    upload_ = new QPushButton("Upload", this);
    upload_->setObjectName("upload");
    upload_->setIcon(VisualStyle::icon("upload"));
    VisualStyle::role(upload_, "primary");
    upload_->setToolTip("Upload the reviewed clip with its trim and masks (the preview is silent; video exports keep the recorded system audio)");
    save_ = new QPushButton("Save a copy…", this);
    save_->setIcon(VisualStyle::icon("save"));
    save_->setToolTip("Export the reviewed clip to a file on this computer. Nothing is uploaded.");
    cancel_ = new QPushButton("Discard…", this);
    cancel_->setObjectName("discard");
    cancel_->setToolTip("Discard this recording and its edits (asks first)");
    VisualStyle::role(cancel_, "quiet");
    auto* draft = new QLabel("Not uploaded", this);
    draft->setObjectName("draftState");
    draft->setToolTip("Nothing uploads until you choose Upload; Save a copy… exports locally.");
    VisualStyle::role(draft, "meta");
    draft->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    destination_ = new Graphite::ElidedLabel(this);
    destination_->setText(options.uploadSummary);
    destination_->setWordWrap(true);
    destination_->setElideMode(Qt::ElideNone);
    destination_->setRevealable(true);
    destination_->setRevealText("Destination: " + options.destination + "\nSharing: " + (options.sharingMode == "private" ? "Private upload" : "Shared link"));
    destination_->setObjectName("uploadDestination");
    destination_->setTextFormat(Qt::PlainText);
    destination_->setAccessibleName(options.uploadSummary);
    VisualStyle::role(destination_, "meta");
    // Keep the destination readable while its complete value wraps.
    destination_->setMinimumWidth(12 * destination_->font().pixelSize());
    destination_->setVisible(!options.uploadSummary.isEmpty());
    auto* footer = new Graphite::Surface("bg", Qt::TopEdge, this);
    footer->setObjectName("reviewFooter");
    auto* footerRows = new QVBoxLayout(footer);
    footerRows->setContentsMargins(reviewSpace(Graphite::Space::l), reviewSpace(Graphite::Space::xs), reviewSpace(Graphite::Space::l), reviewSpace(Graphite::Space::xs));
    footerRows->setSpacing(reviewSpace(Graphite::Space::xs));
    auto* actions = new QWidget(footer);
    actions->setObjectName("reviewActions");
    auto* actionsRow = new QGridLayout(actions);
    actionsRow->setContentsMargins(0, 0, 0, 0);
    actionsRow->setHorizontalSpacing(reviewSpace(Graphite::Space::xs));
    actionsRow->setVerticalSpacing(reviewSpace(Graphite::Space::xs));
    destinationMenu_ = new DestinationMenu(actions);
    destinationMenu_->setObjectName("uploadDestinationMenu");
    destinationMenu_->setAutoRaise(true);
    destinationMenu_->setPopupMode(QToolButton::InstantPopup);
    destinationMenu_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    VisualStyle::role(destinationMenu_, "meta");
    destinationMenu_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    destinationMenu_->setMinimumWidth(60);
    destinationMenu_->setFixedHeight(32);
    auto* details = new QMenu(destinationMenu_);
    auto* detailsAction = new QWidgetAction(details);
    destinationFull_ = new QLabel(options.uploadSummary, details);
    destinationFull_->setObjectName("uploadDestinationWrapped");
    destinationFull_->setTextFormat(Qt::PlainText);
    destinationFull_->setWordWrap(true);
    destinationFull_->setMargin(reviewSpace(Graphite::Space::m));
    destinationFull_->setMaximumWidth(480);
    destinationFull_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    destinationFull_->setFocusPolicy(Qt::StrongFocus);
    VisualStyle::role(destinationFull_, "meta");
    detailsAction->setDefaultWidget(destinationFull_);
    details->addAction(detailsAction);
    destinationMenu_->setMenu(details);
    destination_->hide();
    actionsRow->addWidget(destinationMenu_, 0, 0);
    actionsRow->addWidget(format_, 0, 1);
    actionsRow->addWidget(formatAdvice_, 0, 2);
    exportAudio_->setWordWrap(false);
    auto* metadata = new QWidget(actions);
    metadata->setObjectName("reviewMetadata");
    auto* metadataRow = new QHBoxLayout(metadata);
    metadataRow->setContentsMargins(0, 0, 0, 0);
    metadataRow->setSpacing(reviewSpace(Graphite::Space::s));
    metadataRow->addWidget(exportAudio_);
    metadataRow->addStretch(1);
    metadataRow->addWidget(draft);
    actionsRow->addWidget(metadata, 0, 3);
    actionsRow->addWidget(save_, 0, 4);
    actionsRow->addWidget(cancel_, 0, 5);
    actionsRow->addWidget(upload_, 0, 6);
    auto* nameLine = new QWidget(actions);
    nameLine->setObjectName("reviewDestinationLine");
    auto* nameRow = new QVBoxLayout(nameLine);
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(0);
    nameRow->addWidget(destination_);
    nameLine->hide();
    for (const auto& name : {"reviewMetadataLine", "reviewActionLine"}) {
        auto* line = new QWidget(actions);
        line->setObjectName(name);
        auto* row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(reviewSpace(Graphite::Space::xs));
        line->hide();
    }
    footerRows->addWidget(actions);
    frame->addWidget(footer);
    errorDetails_ = new QPushButton("Details…", this);
    errorDetails_->setObjectName("reviewErrorDetails");
    errorDetails_->setProperty("driveAction", QJsonObject{{"action", "showAdvice"}});
    VisualStyle::role(errorDetails_, "quiet");
    errorDetails_->setFixedWidth(int(std::ceil(QFontMetricsF(errorDetails_->font()).horizontalAdvance(errorDetails_->text()))) + 28);
    errorDetails_->setFixedHeight(std::max(32, textControlHeight(errorDetails_)));
    errorDetails_->hide();
    connect(errorDetails_, &QPushButton::clicked, this, [this] { drive({{"action", "showAdvice"}}); });
    placeDestination();
    // Text stays enlarged; the review's fields need less vertical padding than forms.
    for (auto* field : std::initializer_list<QWidget*>{speed_, masks_, strength_, format_}) {
        field->setProperty("graphiteDensity", "compact");
        VisualStyle::role(field, "meta");
        field->setFixedHeight(textControlHeight(field));
    }
    strength_->setFixedHeight(std::max(32, textControlHeight(strength_)));
    play_->setProperty("graphiteIconBase", QSize(16, 16));
    play_->setIconSize(QSize(Graphite::scaled(16), Graphite::scaled(16)));
    play_->setFixedSize(textControlHeight(play_), textControlHeight(play_));
    speed_->setFixedHeight(textControlHeight(speed_));
    speed_->setFixedWidth(int(std::ceil(QFontMetricsF(speed_->font()).horizontalAdvance(QStringLiteral("0.5×")))) + Graphite::Metric::splitZone + reviewSpace(Graphite::Space::s));
    format_->setFixedHeight(textControlHeight(format_));
    formatAdvice_->setFixedHeight(format_->height());
    for (auto* action : {upload_, save_, cancel_}) {
        action->setProperty("graphiteDensity", "compact");
        action->ensurePolished();
        auto font = action->font();
        const int icon = action->icon().isNull() ? 0 : action->iconSize().width() + 8;
        action->setFixedWidth(int(std::ceil(QFontMetricsF(font).horizontalAdvance(action->text()))) + icon + 32);
        action->setFixedHeight(textControlHeight(action));
    }
    // Keyboard (#283): Space plays and pauses (keyPressEvent, so a focused
    // button or list still takes its own Space), I/O set the trim, Shift+I/O the
    // selected mask's times, Delete removes it, Escape drops an unplaced mask
    // and Enter places it centred (with the frame focused, arrows move it).
    auto shortcut = [this](const QKeySequence& key, auto slot) { auto* s = new QShortcut(key, this); s->setContext(Qt::WindowShortcut); connect(s, &QShortcut::activated, this, slot); return s; };
    shortcut(Qt::Key_I, [this] { stopPlayback(); setTrimAt(true); });
    shortcut(Qt::Key_O, [this] { stopPlayback(); setTrimAt(false); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_I), [this] { stopPlayback(); setMaskAt(true); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_O), [this] { stopPlayback(); setMaskAt(false); });
    shortcut(Qt::Key_Delete, [this] { stopPlayback(); removeSelected(); });
    escape_ = shortcut(Qt::Key_Escape, [this] { disarm(); refreshLabels(); });
    // Enter only while a mask is armed: it never uploads.
    const auto placeCentred = [this] {
        const auto kind = view_->armed();
        const auto rect = view_->defaultMaskRect();
        if (kind.isEmpty() || rect.isEmpty()) return;
        disarm();
        addMaskAt(kind, rect);
        view_->setFocus(Qt::TabFocusReason);
        refreshLabels();
    };
    place_ = shortcut(Qt::Key_Return, placeCentred);
    placeEnter_ = shortcut(Qt::Key_Enter, placeCentred);
    for (auto* s : {escape_, place_, placeEnter_}) s->setEnabled(false);
    frameTimer_.setSingleShot(true);
    frameTimer_.setInterval(qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") > 0 ? qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") : 60);
    connect(&frameTimer_, &QTimer::timeout, this, &ReviewWindow::refreshFrame);
    connect(view_, &FrameView::retryRequested, this, [this] { frameTimer_.stop(); refreshFrame(); });
    connect(timeline_, &ReviewTimeline::scrubbed, this, [this] { refreshLabels(); frameTimer_.start(); });
    connect(timeline_, &ReviewTimeline::scrubStarted, this, &ReviewWindow::stopPlayback);
    connect(timeline_, &ReviewTimeline::trimChanged, this, [this](qint64 start, qint64 end) { propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(start)}, {"end", end >= options_.durationUs ? QJsonValue(QJsonValue::Null) : QJsonValue(double(end))}}}); });
    connect(timeline_, &ReviewTimeline::maskRetimed, this, [this](const QString& id, qint64 start, qint64 end) {
        auto mask = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == id; });
        if (mask == edit_.masks.end()) return;
        auto updated = mask->toJson();
        updated.remove("id");
        updated["start"] = double(start);
        updated["end"] = double(end);
        propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", id}, {"mask", updated}}});
    });
    connect(timeline_, &ReviewTimeline::maskSelected, this, &ReviewWindow::selectMask);
    connect(play_, &QToolButton::clicked, this, [this] { if (playing_) stopPlayback(); else startPlayback(); });
    connect(speed_, &QComboBox::currentIndexChanged, this, [this] { if (playing_) { stopPlayback(); startPlayback(); } });
    for (auto* button : {trimStart_, trimEnd_, resetTrim_, addMask_, removeMask_, maskOptions_, maskStart_, maskEnd_}) connect(button, &QToolButton::clicked, this, &ReviewWindow::stopPlayback);
    for (auto* button : {upload_, save_, cancel_}) connect(button, &QPushButton::clicked, this, &ReviewWindow::stopPlayback);
    connect(trimStart_, &QToolButton::clicked, this, [this] { setTrimAt(true); });
    connect(trimEnd_, &QToolButton::clicked, this, [this] { setTrimAt(false); });
    connect(resetTrim_, &QToolButton::clicked, this, [this] { propose(QJsonArray{QJsonObject{{"op", "resetTrim"}}}); });
    connect(addBlur_, &QAction::triggered, this, [this] { armMask("blur"); });
    connect(addSolid_, &QAction::triggered, this, [this] { armMask("solid"); });
    connect(view_, &FrameView::maskCreated, this, [this](const QString& kind, const QRect& r) { disarm(); addMaskAt(kind, r); refreshLabels(); });
    connect(view_, &FrameView::maskMoved, this, [this](const QString& id, const QRect& r) {
        auto mask = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == id; });
        if (mask == edit_.masks.end()) return;
        auto updated = mask->toJson();
        updated.remove("id");
        updated["rect"] = QJsonArray{r.x(), r.y(), r.width(), r.height()};
        propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", id}, {"mask", updated}}});
    });
    connect(view_, &FrameView::maskSelected, this, &ReviewWindow::selectMask);
    connect(masks_, &QComboBox::currentIndexChanged, this, [this](int row) { if (row >= 0 && row < edit_.masks.size()) selectMask(edit_.masks[row].id); });
    connect(removeMask_, &QToolButton::clicked, this, &ReviewWindow::removeSelected);
    connect(maskStart_, &QToolButton::clicked, this, [this] { setMaskAt(true); });
    connect(maskEnd_, &QToolButton::clicked, this, [this] { setMaskAt(false); });
    connect(strength_, &QSpinBox::valueChanged, this, &ReviewWindow::setStrength);
    connect(format_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto value = format_->itemData(index).toStringList();
        if (value.size() != 2 || (value[0] == config_.format && value[1] == config_.codec)) return;
        config_.format = value[0];
        config_.codec = value[1];
        config_.audio.system = effectiveAudio(config_, options_.audioRecorded, audioIntent_);
        emit configChanged(config_);
        refreshLabels();
    });
    // Upload hands the clip to the resident: the review closes at once and the
    // export and upload continue in the background, reported like any upload.
    connect(upload_, &QPushButton::clicked, this, [this] {
        if (exporting_) return;
        closing_ = true;
        emit uploadRequested(edit_.revision);
        close();
    });
    chooser_ = new FileDialog(this);
    connect(save_, &QPushButton::clicked, this, [this] { chooseCopy(); });
    connect(cancel_, &QPushButton::clicked, this, &ReviewWindow::askDiscard);
    setConfig(config);
    setEdit({});
    // Tab order follows the visible rows and includes the inline privacy action.
    const QList<QWidget*> order{view_, play_, speed_, timeline_, addMask_, trimStart_, trimEnd_, resetTrim_, masks_, maskOptions_, maskStart_, maskEnd_, static_cast<MaskInterval*>(maskActive_)->warningButton(), destinationMenu_, format_, formatAdvice_, save_, cancel_, upload_};
    for (int i = 1; i < order.size(); ++i) setTabOrder(order[i - 1], order[i]);
    timeline_->setFocus();
    // Fit inside the workspace; first presentation measures the frame share before choosing width.
    QSize size = defaultSize().boundedTo(availableReviewSize());
    resize(size);
    placeTools();
    updateMinimum();
}
QSize ReviewWindow::defaultSize() {
    // Native decorations are already reserved by availableReviewSize().
    return {820, 720};
}

RecordingMask* ReviewWindow::selectedMask() {
    auto it = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == selectedMask_; });
    return it == edit_.masks.end() ? nullptr : &*it;
}
const RecordingMask* ReviewWindow::selectedMask() const { return const_cast<ReviewWindow*>(this)->selectedMask(); }
QString ReviewWindow::timeText(qint64 us) const { return durationText(us); }
void ReviewWindow::propose(const QJsonArray& commands) { if (!exporting_) emit editProposed(edit_.revision, commands); }
qint64 ReviewWindow::playhead() const { return timeline_->playhead(); }
void ReviewWindow::seek(qint64 us) { timeline_->setPlayhead(us); refreshLabels(); frameTimer_.start(); }
void ReviewWindow::selectMask(const QString& id) {
    stopPlayback();
    selectedMask_ = std::any_of(edit_.masks.cbegin(), edit_.masks.cend(), [&](const RecordingMask& m) { return m.id == id; }) ? id : QString();
    refreshMaskList();
    view_->setMasks(edit_.masks, selectedMask_);
    timeline_->setMasks(edit_.masks, selectedMask_, true);
    refreshLabels();
}
void ReviewWindow::armMask(const QString& kind) {
    stopPlayback();
    view_->armCreate(kind);
    for (auto* s : {escape_, place_, placeEnter_}) s->setEnabled(true);
    refreshLabels();
}
void ReviewWindow::disarm() {
    view_->disarm();
    for (auto* s : {escape_, place_, placeEnter_}) s->setEnabled(false);
}
void ReviewWindow::addMaskAt(const QString& kind, const QRect& r) {
    // A new mask starts at the current frame and runs to the retained end.
    const qint64 at = std::min<qint64>(playhead(), options_.durationUs - 1);
    const qint64 end = std::max<qint64>(at + 1, edit_.exportEnd(options_.durationUs));
    QJsonObject mask{{"kind", kind}, {"rect", QJsonArray{r.x(), r.y(), r.width(), r.height()}}, {"start", double(at)}, {"end", double(end)}};
    if (kind == "blur") mask["strength"] = strength_->value();
    propose(QJsonArray{QJsonObject{{"op", "addMask"}, {"mask", mask}}});
}
QString ReviewWindow::setTrimAt(bool start) {
    const qint64 at = playhead();
    const auto refuse = [this](const QString& message) { showError(message, false); return message; };
    if (start) {
        if (at >= edit_.exportEnd(options_.durationUs)) return refuse("Trim start must be before the trim end");
        propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(at)}, {"end", edit_.trimEnd ? QJsonValue(double(*edit_.trimEnd)) : QJsonValue(QJsonValue::Null)}}});
        return {};
    }
    if (at <= edit_.trimStart) return refuse("Trim end must be after the trim start");
    propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", double(at)}}});
    return {};
}
QString ReviewWindow::setMaskAt(bool start) {
    const auto refuse = [this](const QString& message) { showError(message, false); return message; };
    auto* mask = selectedMask();
    if (!mask) return refuse("Select a mask to set its times");
    const qint64 at = playhead();
    auto updated = mask->toJson();
    updated.remove("id");
    if (start) { if (at >= mask->end) return refuse("The mask must start before it ends"); updated["start"] = double(at); }
    else { if (at <= mask->start) return refuse("The mask must end after it starts"); updated["end"] = double(at); }
    propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", mask->id}, {"mask", updated}}});
    return {};
}
void ReviewWindow::setStrength(int value) {
    auto* mask = selectedMask();
    if (!mask || mask->kind != "blur" || mask->strength == value) return;
    auto updated = mask->toJson();
    updated.remove("id");
    updated["strength"] = value;
    propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", mask->id}, {"mask", updated}}});
}
void ReviewWindow::removeSelected() { if (selectedMask()) propose(QJsonArray{QJsonObject{{"op", "removeMask"}, {"maskId", selectedMask_}}}); }
// The privacy guidance's action: an armed blur becomes an armed solid cover; a
// selected blur becomes a solid cover in place (one updateMask, one undo step).
void ReviewWindow::useSolidCover() {
    if (view_->armed() == "blur") { armMask("solid"); return; }
    auto* mask = selectedMask();
    if (!mask || mask->kind != "blur") return;
    auto updated = mask->toJson();
    updated.remove("id");
    updated.remove("strength");
    updated["kind"] = "solid";
    propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", mask->id}, {"mask", updated}}});
}
void ReviewWindow::setEdit(const RecordingEdit& edit) {
    // A mask the user just added becomes the selection; a removed one leaves none.
    QStringList before;
    for (const auto& mask : edit_.masks) before.append(mask.id);
    // An accepted edit answers a refusal or an earlier result in the status row.
    if (edit.revision != edit_.revision && !exporting_) message_ = {};
    maskNames_->observe(edit.masks);
    edit_ = edit;
    for (const auto& mask : edit_.masks) if (!before.contains(mask.id)) selectedMask_ = mask.id;
    if (!selectedMask()) selectedMask_.clear();
    refreshMaskList();
    view_->setMasks(edit_.masks, selectedMask_);
    refreshTimeline();
    // Open a pre-trimmed clip on the first kept frame. Later accepted edits
    // preserve the user's playhead, including a deliberate seek outside it.
    if (!isVisible() && !initialFitDone_) timeline_->setPlayhead(edit_.trimStart);
    refreshLabels();
    frameTimer_.start();
}
void ReviewWindow::setConfig(const RecordingConfig& config) {
    if (!config.silent()) audioIntent_ = config.audio.system;
    config_ = config;
    config_.audio.system = effectiveAudio(config_, options_.audioRecorded, audioIntent_);
    for (int i = 0; i < format_->count(); ++i) { const auto value = format_->itemData(i).toStringList(); if (value.size() == 2 && value[0] == config.format && value[1] == config.codec) { format_->blockSignals(true); format_->setCurrentIndex(i); format_->blockSignals(false); } }
    format_->setToolTip(format_->currentData(Qt::ToolTipRole).toString());
    refreshLabels();
}
void ReviewWindow::setExporting(bool exporting, const QString& label) {
    stopPlayback();
    if (exporting) { disarm(); chooser_->cancel(); if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); } }
    if (exporting && discard_) answerDiscard(false);
    exporting_ = exporting;
    for (auto* entry : {addSolid_, addBlur_}) entry->setEnabled(!exporting);
    for (auto* w : std::initializer_list<QWidget*>{upload_, save_, cancel_, trimStart_, trimEnd_, resetTrim_, addMask_, removeMask_, maskOptions_, maskStart_, maskEnd_, strength_, masks_, format_, play_, speed_, timeline_, view_}) w->setEnabled(!exporting);
    // Progress while it runs, then the result (a failure arrives through showError).
    message_ = label.isEmpty() ? StatusEntry{} : StatusEntry{label, exporting ? QStringLiteral("info") : QStringLiteral("success"), {}};
    if (!exporting && !label.isEmpty()) message_.actions = {{QStringLiteral("Dismiss"), QJsonObject{{"action", "dismissStatus"}}}};
    refreshLabels();
}
bool ReviewWindow::savedCurrent() const {
    auto saved = savedConfig_.toJson(), current = config_.toJson();
    saved.remove("budgetBytes"); current.remove("budgetBytes");
    return savedRevision_ == edit_.revision && saved == current;
}
void ReviewWindow::setSavedCopy(const QString& path, quint64 revision, const RecordingConfig& config) {
    savedPath_ = path;
    savedRevision_ = revision;
    savedConfig_ = config;
    savedDismissed_ = false;
    lastShowInFolder_ = {};
    refreshLabels();
}
void ReviewWindow::showError(const QString& message, bool retryExport) {
    message_ = message.isEmpty() ? StatusEntry{} : StatusEntry{message, QStringLiteral("error"), {{QStringLiteral("Dismiss"), QJsonObject{{"action", "dismissStatus"}}}}};
    if (!message.isEmpty() && retryExport) message_.actions.prepend({QStringLiteral("Try again"), QJsonObject{{"action", "retryExport"}}});
    refreshLabels();
}
void ReviewWindow::setNotice(const QString& text) {
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
    notice_->parentWidget()->setVisible(!text.isEmpty());
}
void ReviewWindow::reopen(const QString& error) {
    closing_ = false;
    show();
    raise();
    activateWindow();
    if (!error.isEmpty()) showError(error);
}
void ReviewWindow::setLimits(qint64 animationMaxUs, int animationMaxEdge) {
    animationMaxUs_ = animationMaxUs;
    animationMaxEdge_ = animationMaxEdge;
    refreshLabels();
}
void ReviewWindow::refreshMaskList() {
    masks_->blockSignals(true);
    masks_->clear();
    int row = -1;
    for (int i = 0; i < edit_.masks.size(); ++i) {
        const auto& m = edit_.masks[i];
        masks_->addItem(maskNames_->name(m), QString::number(maskNames_->number(m)));
        masks_->setItemData(i, QStringLiteral("%1 %2×%3 at %4,%5 · %6–%7").arg(maskNames_->name(m)).arg(m.rect.width()).arg(m.rect.height()).arg(m.rect.x()).arg(m.rect.y()).arg(durationText(m.start), durationText(m.end)), Qt::ToolTipRole);
        if (m.id == selectedMask_) row = i;
    }
    masks_->setCurrentIndex(row);
    masks_->setAccessibleDescription(row < 0 ? QStringLiteral("No mask selected") : masks_->itemData(row, Qt::ToolTipRole).toString());
    masks_->setToolTip(row < 0 ? QStringLiteral("Choose the mask to edit") : masks_->itemData(row, Qt::ToolTipRole).toString());
    masks_->blockSignals(false);
    int widest = masks_->fontMetrics().horizontalAdvance(masks_->placeholderText());
    for (int i = 0; i < masks_->count(); ++i) widest = std::max(widest, masks_->fontMetrics().horizontalAdvance(masks_->itemText(i)));
    masks_->setFixedWidth(widest + Graphite::Metric::splitZone + 18);
}
void ReviewWindow::refreshTimeline() {
    timeline_->setTrim(edit_.trimStart, edit_.exportEnd(options_.durationUs));
    timeline_->setMasks(edit_.masks, selectedMask_);
}
void ReviewWindow::refreshLabels() {
    const qint64 at = playhead();
    const auto end = edit_.exportEnd(options_.durationUs);
    const bool retained = at >= edit_.trimStart && at < end;
    time_->setText(QStringLiteral("%1 / %2").arg(timeText(at), timeText(options_.durationUs)));
    time_->setAccessibleDescription(time_->text());
    playbackStatus_->setText(retained ? QStringLiteral("Preview muted") : QStringLiteral("Outside kept part"));
    playbackStatus_->show();
    time_->setToolTip(time_->text() + "\n" + (retained ? QStringLiteral("Playhead time / recording length") : QStringLiteral("The playhead is in a trimmed-away part; it is not exported")));
    const auto boundary = [](qint64 us) { auto text = durationText(us); if (text.endsWith(".0")) text.chop(2); return text; };
    retained_->setText(QStringLiteral("Keep %1–%2 · %3 s").arg(boundary(edit_.trimStart), boundary(end), seconds(end - edit_.trimStart)));
    retained_->setToolTip(QStringLiteral("Keeping %1–%2 (%3 of %4)").arg(timeText(edit_.trimStart), timeText(end), timeText(end - edit_.trimStart), timeText(options_.durationUs)));
    timeline_->setAccessibleDescription(QStringLiteral("Playhead %1; keeping %2–%3; %4 masks").arg(timeText(at), timeText(edit_.trimStart), timeText(end)).arg(edit_.masks.size()));
    // Mask settings are contextual (#283): the list once masks exist, the
    // times and Remove for a selected mask, the strength for a selected blur.
    const auto* mask = selectedMask();
    const bool anyBlur = view_->armed() == "blur" || std::any_of(edit_.masks.cbegin(), edit_.masks.cend(), [](const auto& mask) { return mask.kind == "blur"; });
    if (blurTipShown_ && !anyBlur) blurTipDismissed_ = true;
    const bool properties = !edit_.masks.isEmpty() || !view_->armed().isEmpty();
    maskGroup_->setVisible(properties);
    masks_->setVisible(true);
    masks_->setEnabled(!edit_.masks.isEmpty() && !exporting_);
    maskActive_->setVisible(true);
    maskTiming_->setVisible(true);
    maskTiming_->setEnabled(mask && !exporting_);
    removeMask_->setVisible(mask != nullptr);
    maskOptions_->setVisible(true);
    maskOptions_->setEnabled(mask && !exporting_);
    maskOptions_->setToolTip(mask ? QStringLiteral("Mask options: blur radius and Delete mask") : QStringLiteral("Select a mask first"));
    maskStart_->setToolTip(mask ? QStringLiteral("Start the selected mask at the playhead (Shift+I)") : QStringLiteral("Select a mask first"));
    maskEnd_->setToolTip(mask ? QStringLiteral("End the selected mask at the playhead (Shift+O)") : QStringLiteral("Select a mask first"));
    if (!mask) maskOptions_->menu()->close();
    const QString outside = QStringLiteral("Not at playhead");
    if (mask) {
        const QString startsAt = QStringLiteral("Starts at %1").arg(timeText(mask->start));
        static_cast<MaskInterval*>(maskActive_)->setInterval(QStringLiteral("%1–%2").arg(timeText(mask->start), timeText(mask->end)), at < mask->start ? startsAt : at >= mask->end ? outside : QString());
    } else static_cast<MaskInterval*>(maskActive_)->setInterval(QStringLiteral("—"), {});
    const bool warning = anyBlur && !blurTipDismissed_;
    QStringList intervals;
    for (const auto& item : edit_.masks) {
        const auto interval = QStringLiteral("%1–%2").arg(timeText(item.start), timeText(item.end));
        intervals << interval;
    }
    static_cast<MaskInterval*>(maskActive_)->reserve(intervals, anyBlur);
    static_cast<MaskInterval*>(maskActive_)->setWarning(warning ? Graphite::blurWarningText() : QString());
    static_cast<MaskInterval*>(maskActive_)->warningButton()->setEnabled(!exporting_ && (view_->armed() == "blur" || (mask && mask->kind == "blur")));
    if (warning) blurTipShown_ = true;
    findChild<Graphite::Banner*>("reviewBlurAdvice")->setVisible(mask && mask->kind == "blur");
    strengthControl_->setVisible(mask && mask->kind == "blur");
    if (mask) { strength_->blockSignals(true); strength_->setValue(mask->strength); strength_->blockSignals(false); }
    // Preview and export audio stay together, independently of the destination.
    const bool includesAudio = effectiveAudio(config_, options_.audioRecorded, audioIntent_);
    const QString audio = (!options_.audioRecorded ? QStringLiteral("No audio recorded") : config_.silent() ? QStringLiteral("No audio in %1").arg(config_.format.toUpper()) : includesAudio ? QStringLiteral("System audio included") : QStringLiteral("Audio off in export")) + QStringLiteral(" · Preview muted");
    audio_->setText(QStringLiteral("Silent preview"));
    audio_->setProperty("summary", audio);
    audio_->setToolTip(!options_.audioRecorded ? QStringLiteral("No system audio was recorded") : config_.silent() ? QStringLiteral("System audio was recorded; %1 exports are silent. Choose MP4 or WebM to keep it.").arg(config_.format.toUpper()) : includesAudio ? QStringLiteral("The export keeps the recorded system audio; the preview is silent") : QStringLiteral("System audio is turned off for this export"));
    audio_->setAccessibleName(QStringLiteral("Audio: ") + audio);
    formatAdvice_->setVisible(config_.format == "mp4" && config_.codec == "h265");
    exportAudio_->setAccessibleName(audio);
    exportAudio_->setToolTip(audio_->toolTip());
    placeTools();
    refreshStatus();
}
QList<ReviewWindow::StatusEntry> ReviewWindow::statusEntries() const {
    QList<StatusEntry> entries;
    if (!message_.text.isEmpty()) entries.append(message_);
    if (!savedPath_.isEmpty() && !savedDismissed_ && !exporting_)
        entries.append({(savedCurrent() ? QStringLiteral("Saved %1") : QStringLiteral("Earlier copy saved: %1")).arg(QFileInfo(savedPath_).fileName()),
            savedCurrent() ? QStringLiteral("success") : QStringLiteral("info"),
            {{QStringLiteral("Show in folder"), {{"action", "showInFolder"}}}, {QStringLiteral("Copy path"), {{"action", "copyPath"}}}, {QStringLiteral("Dismiss"), {{"action", "dismissSavedCopy"}}}}});
    if (exporting_) return entries;
    // A switch to a format this recording offers, when one would resolve the problem.
    const auto offerCompatibleVideo = [this]() -> std::optional<QPair<QString, QJsonObject>> {
        for (int i = 0; i < format_->count(); ++i)
            if (format_->itemData(i).toStringList() == QStringList{"mp4", "h264"}) return QPair<QString, QJsonObject>{QStringLiteral("Use MP4 (H.264)"), QJsonObject{{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}}};
        return std::nullopt;
    };
    if (config_.format == "mp4" && config_.codec == "h265") {
        StatusEntry codec{QStringLiteral("Use MP4 (H.264) for broader playback compatibility."), QStringLiteral("warning"), {}};
        if (const auto h264 = offerCompatibleVideo()) codec.actions.append(*h264);
        if (message_.tone != "error") entries.append(codec);
    }
    const auto end = edit_.exportEnd(options_.durationUs);
    if (config_.animation() && animationMaxUs_ > 0 && end - edit_.trimStart > animationMaxUs_) {
        const qint64 seconds = animationMaxUs_ / 1000000;
        StatusEntry trim{QStringLiteral("%1 is limited to %2 s and the kept part is %3. Trim it to %2 s or less before exporting.").arg(config_.format.toUpper()).arg(seconds).arg(timeText(end - edit_.trimStart)), QStringLiteral("warning"), {}};
        trim.actions.append({QStringLiteral("Keep the first %1 s").arg(seconds), QJsonObject{{"action", "trimToLimit"}}});
        if (const auto mp4 = offerCompatibleVideo()) trim.actions.append(*mp4);
        entries.append(trim);
    }
    if (config_.animation() && animationMaxEdge_ > 0 && !options_.canvas.isEmpty()) {
        const auto size = recording::outputSize(options_.canvas, config_.maxEdge);
        if (std::max(size.width(), size.height()) > animationMaxEdge_) {
            StatusEntry edge{QStringLiteral("%1 is limited to %2 px and this recording is %3 × %4 px, so the export will be refused.").arg(config_.format.toUpper()).arg(animationMaxEdge_).arg(size.width()).arg(size.height()), QStringLiteral("warning"), {}};
            if (const auto mp4 = offerCompatibleVideo()) edge.actions.append(*mp4);
            entries.append(edge);
        }
    }
    return entries;
}
void ReviewWindow::refreshStatus() {
    footerError_ = message_.tone == "error";
    playbackStatus_->setText(playhead() < edit_.trimStart || playhead() >= edit_.exportEnd(options_.durationUs) ? QStringLiteral("Outside kept part") : QStringLiteral("Preview muted"));
    playbackStatus_->show();
    placeDestination();
    auto entries = statusEntries();
    for (auto& entry : entries) {
        if (entry.tone != "error" || entry.text != message_.text) continue;
        const QString raw = entry.text;
        if (raw.contains("No space left", Qt::CaseInsensitive) || (raw.contains("disk", Qt::CaseInsensitive) && (raw.contains("full", Qt::CaseInsensitive) || raw.contains("space", Qt::CaseInsensitive))))
            entry.text = QStringLiteral("Disk is full. Free up space, then try again.");
        else if (raw.contains("Permission denied", Qt::CaseInsensitive))
            entry.text = QStringLiteral("Couldn't save the copy: permission denied. Choose another location, then try again.");
        else {
            entry.text = raw.section('\n', 0, 0);
            if (std::any_of(entry.actions.cbegin(), entry.actions.cend(), [](const auto& action) { return action.second["action"] == "retryExport"; }) && !entry.text.contains("save", Qt::CaseInsensitive) && !entry.text.contains("export", Qt::CaseInsensitive) && !entry.text.contains("upload", Qt::CaseInsensitive))
                entry.text.prepend(QStringLiteral("Couldn't export the recording: "));
        }
    }
    const bool blurGuidance = std::any_of(entries.cbegin(), entries.cend(), [](const auto& entry) { return entry.text == Graphite::blurWarningText(); });
    if (compactStatus_ || blurGuidance) {
        if (config_.format == "mp4" && config_.codec == "h265") {
            entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.text.startsWith("Use MP4 (H.264)"); }), entries.end());
            formatAdvice_->show();
        }
    }
    // One notice at a time; retain the other facts for agents and later recovery.
    if (!entries.isEmpty()) {
        const auto error = std::find_if(entries.cbegin(), entries.cend(), [](const auto& entry) { return entry.tone == "error"; });
        entries = {error == entries.cend() ? entries.first() : *error};
    }
    if (entries == shownStatus_) { updateMinimum(); return; }
    shownStatus_ = entries;
    if (std::any_of(entries.cbegin(), entries.cend(), [](const auto& entry) { return entry.text == Graphite::blurWarningText(); })) blurTipShown_ = true;
    errorDetails_->hide();
    errorDetails_->setParent(this);
    // Later: a status action's own click may be what changed the row.
    while (auto* item = statusLines_->takeAt(0)) { if (auto* w = item->widget()) { w->hide(); w->deleteLater(); } delete item; }
    for (const auto& entry : entries) {
        const auto tone = entry.tone == "error" ? Graphite::Banner::Tone::Error : entry.tone == "warning" ? Graphite::Banner::Tone::Warning : entry.tone == "success" ? Graphite::Banner::Tone::Success : Graphite::Banner::Tone::Info;
        auto* band = new Graphite::Banner(tone, {}, statusRow_);
        band->setObjectName("statusLine");
        const auto margins = band->layout()->contentsMargins();
        band->layout()->setContentsMargins(margins.left(), 4, margins.right(), 4);
        auto* line = new QWidget(band);
        auto* row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(reviewSpace(Graphite::Space::xs));
        // The words carry the tone; colour only repeats it.
        const bool saved = std::any_of(entry.actions.cbegin(), entry.actions.cend(), [](const auto& action) { return action.second["action"] == "copyPath"; });
        QLabel* text;
        if (saved) {
            auto* label = new Graphite::ElidedLabel(line);
            label->setText(entry.text);
            label->setToolTip(savedPath_);
            text = label;
        } else text = new QLabel(entry.text, line);
        text->setObjectName("status");
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(!saved);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setAccessibleName(entry.text);
        VisualStyle::role(text, entry.tone == "success" ? "success" : "secondary");
        VisualStyle::role(text, "meta");
        // No wider than its words, so the actions follow them; narrower, it wraps.
        text->setMaximumWidth(text->fontMetrics().horizontalAdvance(entry.text) + 4);
        row->addWidget(text, 1);
        for (const auto& [label, params] : entry.actions) {
            if (entry.tone == "error" && params["action"] == "dismissStatus") {
                row->addWidget(errorDetails_);
                errorDetails_->show();
            }
            auto* button = new QPushButton(label, line);
            button->setObjectName(params["action"] == "useSolidCover" ? "useRedact" : "statusAction");
            VisualStyle::role(button, "label");
            if (params["action"] == "dismissStatus" || params["action"] == "dismissBlurTip") {
                button->setText({});
                button->setIcon(Graphite::chromeIcon("close"));
                connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, button, [button] { button->setIcon(Graphite::chromeIcon("close")); });
                button->setAccessibleName(label);
                button->setToolTip(label);
                button->setFixedWidth(iconTarget());
            }
            if (params["action"] != "retryExport") VisualStyle::role(button, "quiet");
            // Compact action painting inside a full pointer target.
            button->setFixedWidth((params["action"] == "dismissStatus" || params["action"] == "dismissBlurTip") ? iconTarget() : int(std::ceil(QFontMetricsF(button->font()).horizontalAdvance(label))) + 28);
            button->setMinimumHeight(32);
            button->setFixedHeight(std::max(32, textControlHeight(button)));
            button->setProperty("driveAction", params);
            connect(button, &QPushButton::clicked, this, [this, params] { drive(params); });
            row->addWidget(button, 0, Qt::AlignVCenter);
        }
        row->addStretch(0);
        band->setContentWidget(line);
        statusLines_->addWidget(band);
        band->show();
        row->activate();
    }
    placeDestination();
    statusRow_->setVisible(!entries.isEmpty());
    statusLines_->invalidate();
    statusLines_->activate();
    layout()->activate();
    updateMinimum();
}
void ReviewWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placeDestination();
    placeTools();
    updateMinimum();
}
QSize ReviewWindow::availableReviewSize() const {
    if (!options_.availableSize.isEmpty()) return options_.availableSize;
    const auto* display = screen() ? screen() : QGuiApplication::primaryScreen();
    return display ? display->availableGeometry().size() - QSize(24, 48) : QSize(1342, 720);
}
void ReviewWindow::showAdvice(bool formatTips) {
    if (adviceDialog_) { auto* dialog = adviceDialog_.data(); adviceDialog_.clear(); dialog->close(); return; }
    const bool hasError = !formatTips && message_.tone == "error";
    const bool failed = hasError && std::any_of(message_.actions.cbegin(), message_.actions.cend(), [](const auto& action) { return action.second["action"] == "retryExport"; });
    auto* dialog = new QDialog(this);
    adviceDialog_ = dialog;
    dialog->setObjectName("reviewAdvice");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QString title = hasError ? (failed ? QStringLiteral("Export failed") : QStringLiteral("Recording error")) : QStringLiteral("Format tips");
    dialog->setWindowTitle(title);
    dialog->setAccessibleName(title);
    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll, 1);
    auto* body = new QWidget(scroll);
    scroll->setWidget(body);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(reviewSpace(Graphite::Space::xl), reviewSpace(Graphite::Space::l), reviewSpace(Graphite::Space::xl), reviewSpace(Graphite::Space::l));
    layout->setSpacing(reviewSpace(Graphite::Space::m));
    auto* heading = new QLabel(title, body);
    VisualStyle::role(heading, "sectionTitle");
    layout->addWidget(heading);
    auto* text = new QLabel(message_.text, body);
    text->setObjectName("reviewAdviceText");
    text->setProperty("plainText", hasError ? message_.text : audio_->property("summary").toString() + "\n\n" + formatHint(config_.format, config_.codec));
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    text->setFocusPolicy(Qt::StrongFocus);
    text->setAccessibleName(title);
    text->setVisible(hasError);
    layout->addWidget(text);
    if (!hasError) {
        auto* form = new QFormLayout;
        VisualStyle::styleForm(form);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        layout->addLayout(form);
        auto fact = [body](const QString& value) { auto* label = new QLabel(value, body); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); return label; };
        form->addRow("Audio", fact(audio_->property("summary").toString()));
        auto* formatRow = new QWidget(body);
        auto* formatLayout = new QVBoxLayout(formatRow);
        formatLayout->setContentsMargins(0, 0, 0, 0);
        formatLayout->setSpacing(reviewSpace(Graphite::Space::xs));
        formatLayout->addWidget(fact(formatHint(config_.format, config_.codec)));
        bool h264Available = false;
        for (int i = 0; i < format_->count(); ++i) h264Available |= format_->itemData(i).toStringList() == QStringList{"mp4", "h264"};
        if (config_.format == "mp4" && config_.codec == "h265" && h264Available) {
            auto* compatible = new QPushButton("Use MP4 (H.264)", formatRow);
            compatible->setProperty("graphiteFlush", true);
            VisualStyle::role(compatible, "quiet");
            connect(compatible, &QPushButton::clicked, this, [this, dialog] {
                drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}});
                dialog->close();
            });
            formatLayout->addWidget(compatible, 0, Qt::AlignLeft);
        }
        form->addRow("Format", formatRow);
    }
    layout->addStretch();
    auto* footer = new Graphite::Surface("bg", Qt::TopEdge, dialog);
    auto* actions = new QHBoxLayout(footer);
    actions->setContentsMargins(reviewSpace(Graphite::Space::l), reviewSpace(Graphite::Space::m), reviewSpace(Graphite::Space::l), reviewSpace(Graphite::Space::m));
    actions->setSpacing(reviewSpace(Graphite::Space::s));
    actions->addStretch();
    if (failed) {
        auto* retry = new QPushButton("Try again", footer);
        VisualStyle::role(retry, "primary");
        retry->setDefault(true);
        connect(retry, &QPushButton::clicked, this, [this, dialog] { dialog->close(); drive({{"action", "retryExport"}}); });
        actions->addWidget(retry);
    }
    auto* close = new QPushButton("Close", footer);
    connect(close, &QPushButton::clicked, this, [dialog] { dialog->close(); });
    actions->addWidget(close);
    root->addWidget(footer);
    const int detailsHeight = heading->sizeHint().height() + text->heightForWidth(592) + footer->sizeHint().height() + 62;
    dialog->resize(QSize(640, hasError ? detailsHeight : Graphite::scaled(360)).boundedTo(availableReviewSize()));
    dialog->show();
    if (hasError) text->setFocus();
}
void ReviewWindow::updateMinimum() {
    if (minimumPending_) return;
    minimumPending_ = true;
    // Reflow and wrapped labels settle before measuring the next minimum.
    QTimer::singleShot(0, this, [this] {
        minimumPending_ = false;
        placeTools();
        layout()->activate();
        placeDestination();
        layout()->activate();
        const auto available = availableReviewSize();
        setMinimumSize(minimumSizeHint());
        if (isVisible() && !initialFitDone_) {
            const double aspect = options_.canvas.isEmpty() ? 16.0 / 9 : double(options_.canvas.width()) / options_.canvas.height();
            const int height = std::min(defaultSize().height(), available.height());
            // Preserve frame gates, then prefer fewer control wraps and a useful
            // time axis. Later edits stay inside the user's chosen geometry.
            const auto* flow = static_cast<FlowRow*>(maskGroup_);
            const auto* trim = static_cast<const FlowRow*>(trimGroup_);
            const auto* footer = findChild<QWidget*>("reviewFooter");
            const int fixed = this->height() - view_->height() - (flow->isVisibleTo(this) ? flow->height() : 0) - trim->height() - timeline_->height()
                - footer->height() - (statusRow_->isVisible() ? statusRow_->height() : 0);
            const auto share = [&](int width, int controls, int timelineHeight) {
                const auto margins = footer->layout()->contentsMargins();
                const int metadataWidth = findChild<QWidget*>("reviewMetadata")->sizeHint().width();
                const bool longName = destinationMenu_->fontMetrics().horizontalAdvance(options_.destination) > reviewSpace(Graphite::Space::xl) * 10;
                const int nameHeight = destination_->fontMetrics().horizontalAdvance(options_.destination) <= width - 2 * reviewSpace(Graphite::Space::l) ? destination_->fontMetrics().tightBoundingRect(options_.destination).height() : destination_->heightForWidth(width - 2 * reviewSpace(Graphite::Space::l));
                const int destinationHeight = longName ? nameHeight + destinationMenu_->fontMetrics().height() : destinationMenu_->heightForWidth(std::max(destinationMenu_->minimumWidth(), width - 2 * reviewSpace(Graphite::Space::l) - metadataWidth - reviewSpace(Graphite::Space::s)));
                const int footerHeight = format_->height() + (footerFits(width - 2 * reviewSpace(Graphite::Space::l) - reviewSpace(Graphite::Space::xs)) ? 0 : std::max(destinationHeight, destinationMenu_->fontMetrics().height()) + (longName ? 0 : 4));
                const int chrome = fixed + trim->heightFor(width - 2 * reviewSpace(Graphite::Space::m)) + timelineHeight + footerHeight + margins.top() + margins.bottom();
                const QSize image = QSize(10000, int(10000 / aspect)).scaled(QSize(width - 8, height - chrome - controls), Qt::KeepAspectRatio);
                return double(image.width()) * image.height() / (double(width) * height);
            };
            int width = minimumWidth();
            double best = -1;
            const int axisMinimum = QFontMetrics(gutterFont()).horizontalAdvance(durationText(options_.durationUs)) * 4 + 96;
            for (int candidate = minimumWidth(); candidate <= available.width(); candidate += 2) {
                const int futureControls = edit_.masks.isEmpty() ? maskStart_->height() : 0;
                const int controls = std::max(futureControls, flow->heightFor(candidate - 2 * reviewSpace(Graphite::Space::m), true));
                const int timelineHeight = timeline_->heightForLanes(candidate - 2 * reviewSpace(Graphite::Space::m), std::max(2, timeline_->lanes()));
                const double steady = share(candidate, std::max(futureControls, flow->heightFor(candidate - 2 * reviewSpace(Graphite::Space::m))), timelineHeight);
                const double transient = share(candidate, controls + std::max(32, QFontMetrics(Graphite::font(Graphite::Type::metadata)).height() + 8) + 4, timelineHeight);
                const int inlineAxis = candidate - 2 * reviewSpace(Graphite::Space::m) - trackPad - ReviewTimeline::gutterWidth() - transport_->property("inlineWidth").toInt() - 12;
                const int axis = inlineAxis < 360 ? candidate - 2 * reviewSpace(Graphite::Space::m) - trackPad - ReviewTimeline::gutterWidth() : inlineAxis;
                const double organization = std::min(1.0, double(axis) / axisMinimum) - controls * 0.002;
                const double score = steady >= 0.605 && transient >= 0.535 ? 10 + organization + transient * 0.02 : steady >= 0.605 ? 1 + transient : steady;
                if (score > best) { best = score; width = candidate; }
            }
            resize(std::max(width, minimumWidth()), height);
            initialFitDone_ = true;
            placeTools();
            layout()->activate();
            updateMinimum();
        }
        const auto tight = [this] {
            const auto frame = view_->imageRect();
            const double share = double(frame.width()) * frame.height() / std::max(1, width() * height());
            return share < (shownStatus_.isEmpty() && !footerError_ ? 0.60 : 0.53);
        };
        if (!compactStatus_ && (minimumSizeHint().height() > std::min(compactReviewHeight, available.height()) || (isVisible() && tight()))) {
            compactStatus_ = true;
            formatAdvice_->setVisible(config_.format == "mp4" && config_.codec == "h265");
            formatAdvice_->setAccessibleName("Format tips");
            formatAdvice_->setToolTip("Format tips: compatibility and audio for each format");
            refreshStatus();
            layout()->activate();
        }
        if (!footerError_ && message_.tone == "error" && (minimumSizeHint().height() > available.height() || (isVisible() && tight()))) {
            footerError_ = true;
            refreshStatus();
            layout()->activate();
        }
        placeTools();
        layout()->activate();
        setMinimumSize(minimumSizeHint());

    });
}
void ReviewWindow::placeTools() {
    if (maskGroup_) static_cast<FlowRow*>(maskGroup_)->relayout();
    if (trimGroup_) static_cast<FlowRow*>(trimGroup_)->relayout();
    if (destinationMenu_) placeDestination();
}
QSize ReviewWindow::minimumSizeHint() const {
    if (!view_ || !maskGroup_ || !layout()) return QWidget::minimumSizeHint();
    // Every row at full size: the widest sets the width, and the mask row
    // wrapped at that width (with a blur's radius) sets the height.
    const auto* flow = static_cast<const FlowRow*>(maskGroup_);
    const auto rows = layout()->minimumSize();
    const int width = std::max({640, rows.width(), flow->minimumSizeHint().width() + 2 * reviewSpace(Graphite::Space::l)});
    int height = rows.height();
    if (flow->isVisibleTo(this)) height += flow->heightFor(width - 2 * reviewSpace(Graphite::Space::m), true) - flow->minimumSizeHint().height();
    // Status lines wrap at a narrow width; count them as they are laid out.
    if (statusRow_->isVisibleTo(this) && statusRow_->hasHeightForWidth()) height += std::max(0, statusRow_->heightForWidth(width) - statusRow_->minimumSizeHint().height());
    return {width, std::max(480, height)};
}
// Move the same footer controls at their measured fit threshold.
bool ReviewWindow::footerFits(int width) const {
    const auto mode = options_.sharingMode == "private" ? QStringLiteral("Private upload") : QStringLiteral("Shared link");
    int needed = destinationMenu_->fontMetrics().horizontalAdvance(options_.destination + QStringLiteral(" · ") + mode) + Graphite::Metric::splitZone + 24;
    for (auto* widget : std::initializer_list<QWidget*>{format_, findChild<QWidget*>("reviewMetadata"), save_, cancel_, upload_})
        needed += std::clamp(widget->sizeHint().width(), widget->minimumWidth(), widget->maximumWidth()) + 4;
    if (formatAdvice_->isVisibleTo(this)) needed += formatAdvice_->width() + 4;
    return width >= needed;
}
void ReviewWindow::placeDestination() {
    if (!destination_ || !exportAudio_) return;
    destination_->setText(options_.destination.isEmpty() ? options_.uploadSummary : options_.destination + QStringLiteral(" · ") + (options_.sharingMode == "private" ? QStringLiteral("Private upload") : QStringLiteral("Shared link")));
    destination_->setRevealText("Destination: " + options_.destination + "\nSharing: " + (options_.sharingMode == "private" ? "Private upload" : "Shared link") + "\n\n" + formatLabel(config_.format, config_.codec) + "\n" + formatHint(config_.format, config_.codec) + "\n" + audio_->property("summary").toString());
    destination_->setToolTip(options_.uploadSummary);
    destination_->setAccessibleName(options_.uploadSummary);
    exportAudio_->setText(effectiveAudio(config_, options_.audioRecorded, audioIntent_) ? QStringLiteral("System audio") : !options_.audioRecorded ? QStringLiteral("No audio recorded") : config_.silent() ? QStringLiteral("Silent %1").arg(config_.format.toUpper()) : QStringLiteral("Audio off"));
    const auto details = "Destination: " + options_.destination + "\nSharing: " + (options_.sharingMode == "private" ? "Private upload" : "Shared link") + "\n\n" + options_.uploadSummary + "\n" + formatLabel(config_.format, config_.codec) + "\n" + formatHint(config_.format, config_.codec) + "\n" + audio_->property("summary").toString();
    destinationFull_->setText(details);
    destinationMenu_->setText(destination_->text());
    destinationMenu_->setToolTip(details);
    destinationMenu_->setAccessibleName(destination_->text());
    destinationMenu_->setVisible(!options_.uploadSummary.isEmpty());
    auto* metadata = findChild<QWidget*>("reviewMetadata");
    const auto mode = options_.sharingMode == "private" ? QStringLiteral("Private upload") : QStringLiteral("Shared link");
    const int fullWidth = destinationMenu_->fontMetrics().horizontalAdvance(destination_->text()) + Graphite::Metric::splitZone + 24;
    const bool longName = destinationMenu_->fontMetrics().horizontalAdvance(options_.destination) > reviewSpace(Graphite::Space::xl) * 10;
    const int modeWidth = (longName ? destinationMenu_->fontMetrics().horizontalAdvance(mode) : std::min(destinationMenu_->fontMetrics().horizontalAdvance(destination_->text()), reviewSpace(Graphite::Space::xl) * 10)) + Graphite::Metric::splitZone + 24;
    destinationMenu_->setMinimumWidth(modeWidth);
    const int naturalDestination = longName ? modeWidth : fullWidth;
    destinationMenu_->setMaximumWidth(naturalDestination);
    auto* grid = qobject_cast<QGridLayout*>(findChild<QWidget*>("reviewActions")->layout());
    const int available = std::max(1, width() - 2 * reviewSpace(Graphite::Space::l));
    const bool wasSingle = grid->property("placed").toBool() && grid->property("singleRow").toBool();
    const bool single = footerFits(available + (wasSingle ? 4 : -4));
    const int destinationSpace = single ? naturalDestination : available - metadata->sizeHint().width() - reviewSpace(Graphite::Space::s);
    const bool splitName = longName && !single;
    destinationMenu_->setProperty("displayText", splitName ? QVariant(mode) : QVariant());
    destinationMenu_->setMaximumWidth(splitName ? modeWidth : fullWidth);
    destinationMenu_->setFixedHeight(single ? 32 : splitName ? destinationMenu_->fontMetrics().height() : destinationMenu_->heightForWidth(std::max(modeWidth, destinationSpace)));
    if (!grid->property("placed").toBool() || grid->property("singleRow").toBool() != single || grid->property("splitName").toBool() != splitName) {
        auto* nameLine = findChild<QWidget*>("reviewDestinationLine");
        auto* metaLine = findChild<QWidget*>("reviewMetadataLine");
        auto* actionLine = findChild<QWidget*>("reviewActionLine");
        for (auto* line : {metaLine, actionLine}) while (auto* item = line->layout()->takeAt(0)) delete item;
        for (auto* widget : std::initializer_list<QWidget*>{destinationMenu_, format_, formatAdvice_, metadata, save_, cancel_, upload_, nameLine, metaLine, actionLine}) grid->removeWidget(widget);
        for (int column = 0; column < 7; ++column) grid->setColumnStretch(column, 0);
        nameLine->setVisible(splitName);
        destination_->setVisible(splitName);
        metaLine->setVisible(!single);
        actionLine->setVisible(!single);
        if (single) {
            grid->addWidget(destinationMenu_, 0, 0, Qt::AlignLeft);
            grid->addWidget(format_, 0, 1);
            grid->addWidget(formatAdvice_, 0, 2);
            grid->addWidget(metadata, 0, 3);
            grid->addWidget(save_, 0, 4);
            grid->addWidget(cancel_, 0, 5);
            grid->addWidget(upload_, 0, 6);
            grid->setColumnStretch(0, 1);
        } else {
            auto* metaRow = static_cast<QHBoxLayout*>(metaLine->layout());
            metaRow->addWidget(destinationMenu_);
            metaRow->addWidget(metadata, 1);
            auto* actionRow = static_cast<QHBoxLayout*>(actionLine->layout());
            actionRow->addWidget(format_);
            actionRow->addWidget(formatAdvice_);
            actionRow->addStretch(1);
            for (auto* action : {save_, cancel_, upload_}) actionRow->addWidget(action);
            if (splitName) grid->addWidget(nameLine, 0, 0, 1, 7);
            grid->addWidget(metaLine, splitName ? 1 : 0, 0, 1, 7);
            grid->addWidget(actionLine, splitName ? 2 : 1, 0, 1, 7);
            grid->setVerticalSpacing(splitName ? 0 : reviewSpace(Graphite::Space::xs));
        }
        // Reparenting between footer rows hides widgets that were already shown.
        for (auto* widget : std::initializer_list<QWidget*>{format_, metadata, save_, cancel_, upload_}) widget->show();
        destinationMenu_->setVisible(!options_.uploadSummary.isEmpty());
        formatAdvice_->setVisible(config_.format == "mp4" && config_.codec == "h265");
        grid->setProperty("splitName", splitName);
        grid->setProperty("placed", true);
        grid->setProperty("singleRow", single);
        grid->invalidate();
    }
    // Settle child minima before the default fit measures the footer.
    metadata->layout()->activate();
    for (const auto* name : {"reviewMetadataLine", "reviewActionLine"})
        findChild<QWidget*>(name)->layout()->activate();
    grid->invalidate();
    grid->activate();
    if (splitName) {
        destination_->setText(options_.destination);
        const int nameWidth = std::max(1, available - Graphite::scaled(14) - 18);
        const bool fits = destination_->fontMetrics().horizontalAdvance(options_.destination) <= nameWidth;
        destination_->setWordWrap(!fits);
        destination_->setFixedHeight(fits ? destination_->fontMetrics().tightBoundingRect(options_.destination).height() : destination_->heightForWidth(nameWidth));
    }
    const int height = single ? 32 : splitName ? destinationMenu_->fontMetrics().height() : destinationMenu_->heightForWidth(destinationMenu_->width());
    if (destinationMenu_->height() != height) { destinationMenu_->setFixedHeight(height); grid->activate(); }
}
QJsonObject ReviewWindow::inspect() const {
    auto rect = [](const QRect& r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
    const auto* mask = selectedMask();
    const QRect video(view_->mapTo(this, view_->imageRect().topLeft()), view_->imageRect().size());
    const double area = double(std::max(1, width())) * std::max(1, height());
    QJsonArray masks;
    for (const auto& m : edit_.masks) masks.append(QJsonObject{{"id", m.id}, {"name", maskNames_->name(m)}, {"kind", m.kind}, {"timeline", rect(timeline_->maskRect(m.id).translated(timeline_->mapTo(this, QPoint())))}, {"timelineTarget", rect(timeline_->maskHitRect(m.id).translated(timeline_->mapTo(this, QPoint())))}});
    // The status row as shown: each line's words, tone and its actions as drive() calls.
    QJsonArray statusRow;
    QStringList lines;
    for (const auto& entry : shownStatus_) {
        QJsonArray actions;
        for (const auto& [label, params] : entry.actions) actions.append(QJsonObject{{"label", label}, {"drive", params}});
        statusRow.append(QJsonObject{{"text", entry.text}, {"tone", entry.tone}, {"actions", actions}});
        lines.append(entry.text);
    }
    const QString status = lines.join('\n');
    QJsonArray statusFacts;
    for (const auto& entry : statusEntries()) {
        QJsonArray actions;
        for (const auto& [label, params] : entry.actions) actions.append(QJsonObject{{"label", label}, {"drive", params}});
        statusFacts.append(QJsonObject{{"text", entry.text}, {"tone", entry.tone}, {"actions", actions}});
    }
    return QJsonObject{
        {"visible", isVisible()}, {"size", QJsonArray{width(), height()}}, {"minimumSize", QJsonArray{minimumSizeHint().expandedTo(minimumSize()).width(), minimumSizeHint().expandedTo(minimumSize()).height()}},
        {"statusFacts", statusFacts},
        {"scheme", Theme::name(Theme::scheme())}, {"exporting", exporting_},
        {"playbackStatus", playhead() < edit_.trimStart || playhead() >= edit_.exportEnd(options_.durationUs) ? QStringLiteral("Outside kept part") : QString()},
        {"playheadUs", double(playhead())}, {"playing", playing_}, {"speed", speed_->currentData().toDouble()},
        {"revision", double(edit_.revision)}, {"trim", QJsonObject{{"start", double(edit_.trimStart)}, {"end", double(edit_.exportEnd(options_.durationUs))}}},
        {"masks", masks}, {"selectedMaskId", mask ? QJsonValue(mask->id) : QJsonValue(QJsonValue::Null)},
        {"armed", view_->armed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->armed())},
        {"blurGuidance", QJsonObject{{"visible", maskGroup_->isVisibleTo(this) && !blurTipDismissed_ && (view_->armed() == "blur" || std::any_of(edit_.masks.cbegin(), edit_.masks.cend(), [](const auto& mask) { return mask.kind == "blur"; }))}, {"text", Graphite::blurWarningText()}, {"drive", QJsonObject{{"action", "showMaskOptions"}}}}},
        {"maskControls", QJsonObject{{"list", masks_->isVisibleTo(this)}, {"times", maskStart_->isVisibleTo(this)}, {"strength", strength_->isVisibleTo(this)}, {"remove", removeMask_->isVisibleTo(this)}}},
        {"maskOptions", QJsonObject{{"available", mask != nullptr}, {"blurRadiusAvailable", mask && mask->kind == "blur"}, {"deleteAvailable", mask != nullptr}, {"open", maskOptions_->menu()->isVisible()}, {"drive", QJsonObject{{"action", "showMaskOptions"}}}}},
        {"formatAdvice", QJsonObject{{"visible", formatAdvice_->isVisibleTo(this)}, {"drive", QJsonObject{{"action", "showFormatTips"}}}}},
        {"savedCopy", QJsonObject{{"path", savedPath_}, {"revision", double(savedRevision_)}, {"current", !savedPath_.isEmpty() && savedCurrent()}, {"showInFolder", lastShowInFolder_}}},
        {"selectedMaskName", mask ? QJsonValue(maskNames_->name(*mask)) : QJsonValue(QJsonValue::Null)},
        {"selectedMaskInterval", mask ? QJsonValue(QJsonObject{{"start", double(mask->start)}, {"end", double(mask->end)}, {"text", maskActive_->text()}}) : QJsonValue(QJsonValue::Null)},
        {"errorDetails", QJsonObject{{"visible", errorDetails_->isVisibleTo(this)}, {"message", footerError_ ? QJsonValue(message_.text) : QJsonValue(QJsonValue::Null)}, {"drive", QJsonObject{{"action", "showAdvice"}}}}}, {"adviceDialog", QJsonObject{{"open", bool(adviceDialog_ && adviceDialog_->isVisible())}, {"text", adviceDialog_ && adviceDialog_->isVisible() ? QJsonValue(adviceDialog_->findChild<QLabel*>("reviewAdviceText")->property("plainText").toString()) : QJsonValue(QJsonValue::Null)}}}, {"status", status}, {"statusRow", statusRow}, {"audio", audio_->property("summary").toString()},
        {"previewMuted", true}, {"audioRequested", audioIntent_}, {"audioEffective", effectiveAudio(config_, options_.audioRecorded, audioIntent_)}, {"destination", options_.destination}, {"sharingMode", options_.sharingMode}, {"uploadSummary", options_.uploadSummary},
        {"notice", notice_->isVisibleTo(this) ? QJsonValue(notice_->text()) : QJsonValue(QJsonValue::Null)},
        {"preview", QJsonObject{{"error", view_->previewError().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->previewError())}, {"stale", view_->stale()}, {"frameTimeUs", view_->hasFrame() ? QJsonValue(double(view_->frameTime())) : QJsonValue(QJsonValue::Null)}}},
        {"frameFocus", view_->hasFocus()},
        {"discardConfirmation", QJsonObject{{"open", bool(discard_)}, {"text", discard_ ? QJsonValue(discard_->text()) : QJsonValue(QJsonValue::Null)}}},
        {"fileDialog", chooser_->state()},
        {"format", QJsonObject{{"format", config_.format}, {"codec", config_.codec}}},
        {"video", QJsonObject{{"rect", rect(video)}, {"share", double(video.width()) * video.height() / area}}},
        {"timeline", QJsonObject{{"rect", rect(timeline_->geometry())}, {"lanes", timeline_->lanes()}, {"firstLane", timeline_->firstLane()}, {"laneSummary", timeline_->laneSummary()}, {"interactionRowHeight", rowHeight()}, {"gripTargetSize", gripTarget()}}},
        {"header", QJsonObject{{"visible", header_->isVisibleTo(this)}, {"rect", rect(header_->geometry())}, {"customFrame", Graphite::WindowHeader::customFrame()}}}};
}
ReviewWindow::DriveResult ReviewWindow::drive(const QJsonObject& params) {
    const auto action = params["action"].toString();
    const auto invalid = [](const QString& message) { return DriveResult{"invalid_request", message, {}}; };
    auto done = [this](const QString& error = {}) { return error.isEmpty() ? DriveResult{{}, {}, inspect()} : DriveResult{"invalid_request", error, {}}; };
    if (action == "inspect") return done();
    if (action == "closeMaskOptions") { maskOptions_->menu()->close(); return done(); }
    if (action == "showMaskOptions") {
        if (!selectedMask() || exporting_) return {"recording_state", "Select a mask to change its options", {}};
        maskOptions_->menu()->popup(maskOptions_->mapToGlobal(QPoint(0, maskOptions_->height())));
        return done();
    }
    if (action == "nextMaskLanes" || action == "previousMaskLanes") {
        if (action == "nextMaskLanes") timeline_->showMoreMasks(); else timeline_->showPreviousMasks();
        return done();
    }
    // The discard confirmation, as the Discard… button, the close button and its two answers.
    if (action == "keepReviewing" || action == "confirmDiscard") {
        if (!discard_) return {"recording_state", "No discard confirmation is open", {}};
        answerDiscard(action == "confirmDiscard");
        return done();
    }
    if (discard_) return {"recording_state", "The discard confirmation is open; answer it first (keepReviewing or confirmDiscard)", {}};
    // #259: the Save copy… chooser, as the button opens it; the copy is saved
    // when someone picks a file in it (recording.saveCopy saves to a path).
    if (action == "closeFileDialog") { chooser_->cancel(); return done(); }
    if (exporting_) return {"recording_state", "The review controls are disabled while an export runs", {}};
    if (!isVisible()) return {"recording_state", "The review window is closed", {}};
    if (chooser_->isOpen()) return {"recording_state", "The Save copy dialog is open; close it first (closeFileDialog)", {}};
    if (action == "saveCopyDialog") { chooseCopy(); return done(); }
    if (action == "discard") { askDiscard(); return done(); }
    if (action == "retryExport") {
        if (std::none_of(message_.actions.cbegin(), message_.actions.cend(), [](const auto& action) { return action.second["action"] == "retryExport"; })) return invalid("There is no failed export to retry");
        stopPlayback();
        emit retryExportRequested();
        return done();
    }
    if (action == "retryPreview") { frameTimer_.stop(); refreshFrame(); return done(); }
    if (action == "showAdvice" || action == "showFormatTips") { showAdvice(action == "showFormatTips"); return done(); }
    if (action == "dismissSavedCopy") { savedDismissed_ = true; refreshLabels(); return done(); }
    if (action == "copyPath" || action == "showInFolder") {
        if (savedPath_.isEmpty()) return invalid("Save a copy first");
        if (action == "copyPath") QGuiApplication::clipboard()->setText(savedPath_);
        else {
            QPointer<ReviewWindow> guard(this);
            host::showInFolder(savedPath_, [guard](const QJsonObject& result) {
                if (!guard) return;
                guard->lastShowInFolder_ = result;
                if (!result["opened"].toBool()) guard->showError(QStringLiteral("Couldn't open the folder. Copy the path and open it in your file manager."), false);
            });
        }
        return done();
    }
    if (action == "dismissStatus") {
        message_ = {};
        if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); }
        refreshLabels();
        return done();
    }
    if (action == "dismissBlurTip") { blurTipDismissed_ = true; refreshLabels(); return done(); }
    if (action == "seek") {
        const auto time = params["time"].toDouble(-1);
        if (!params["time"].isDouble() || time < 0 || time >= double(options_.durationUs) || time != std::floor(time)) return invalid("seek takes a clip time in microseconds within the recording");
        stopPlayback();
        seek(qint64(time));
        return done();
    }
    if (action == "play") { startPlayback(); return done(); }
    if (action == "pause") { stopPlayback(); return done(); }
    if (action == "speed") {
        const int index = speed_->findData(params["speed"].toDouble());
        if (!params["speed"].isDouble() || index < 0) return invalid("speed is 0.5, 1, 2 or 4");
        speed_->setCurrentIndex(index);
        return done();
    }
    if (action == "setIn" || action == "setOut") {
        stopPlayback();
        return done(setTrimAt(action == "setIn"));
    }
    if (action == "resetTrim") { stopPlayback(); propose(QJsonArray{QJsonObject{{"op", "resetTrim"}}}); return done(); }
    if (action == "addMask") {
        const auto kind = params["kind"].toString();
        const auto r = params["rect"].toArray();
        // Without a rect: the centred mask Enter places.
        if ((kind != "blur" && kind != "solid") || (params.contains("rect") && r.size() != 4)) return invalid("addMask takes kind blur|solid and an optional rect [x, y, width, height] in canvas pixels");
        const QRect rect = params.contains("rect") ? QRect(r[0].toInt(-1), r[1].toInt(-1), r[2].toInt(-1), r[3].toInt(-1)) : view_->defaultMaskRect();
        if (rect.x() < 0 || rect.y() < 0 || rect.width() < 2 || rect.height() < 2 || (!options_.canvas.isEmpty() && !QRect(QPoint(), options_.canvas).contains(rect))) return invalid("The mask rect must lie inside the canvas");
        stopPlayback();
        disarm();
        addMaskAt(kind, rect);
        refreshLabels();
        return done();
    }
    if (action == "selectMask") {
        const auto id = params["maskId"].isNull() || params["maskId"].isUndefined() ? QString() : params["maskId"].toString();
        if (!id.isEmpty() && !std::any_of(edit_.masks.cbegin(), edit_.masks.cend(), [&](const RecordingMask& m) { return m.id == id; })) return invalid("No mask has this ID");
        selectMask(id);
        return done();
    }
    if (action == "placeMask") {
        // The selected mask's rectangle, as a pointer drag or the frame's arrow keys set it.
        const auto r = params["rect"].toArray();
        if (!selectedMask()) return invalid("Select a mask first");
        if (r.size() != 4) return invalid("placeMask takes rect [x, y, width, height] in canvas pixels");
        const QRect rect(r[0].toInt(-1), r[1].toInt(-1), r[2].toInt(-1), r[3].toInt(-1));
        if (rect.x() < 0 || rect.y() < 0 || rect.width() < 2 || rect.height() < 2 || (!options_.canvas.isEmpty() && !QRect(QPoint(), options_.canvas).contains(rect))) return invalid("The mask rect must lie inside the canvas");
        stopPlayback();
        view_->commitNudge();
        emit view_->maskMoved(selectedMask_, rect);
        return done();
    }
    if (action == "useSolidCover") {
        const auto* mask = selectedMask();
        if (view_->armed() != "blur" && (!mask || mask->kind != "blur")) return invalid("Select a blur mask (or arm Add blur) first");
        stopPlayback();
        useSolidCover();
        return done();
    }
    if (action == "trimToLimit") {
        const auto end = edit_.exportEnd(options_.durationUs);
        if (!config_.animation() || animationMaxUs_ <= 0 || end - edit_.trimStart <= animationMaxUs_) return invalid("The kept part is within the format's length limit");
        stopPlayback();
        propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", double(edit_.trimStart + animationMaxUs_)}}});
        return done();
    }
    if (action == "setMaskIn" || action == "setMaskOut") {
        stopPlayback();
        return done(setMaskAt(action == "setMaskIn"));
    }
    if (action == "strength") {
        const auto* mask = selectedMask();
        const auto value = params["strength"].toDouble(-1);
        if (!mask || mask->kind != "blur") return invalid("Select a blur mask first");
        if (value < strength_->minimum() || value > strength_->maximum() || value != std::floor(value)) return invalid("strength is 2 to 64");
        strength_->setValue(int(value));
        return done();
    }
    if (action == "removeMask") {
        if (!selectedMask()) return invalid("Select a mask first");
        stopPlayback();
        removeSelected();
        return done();
    }
    if (action == "format") {
        const QStringList wanted{params["format"].toString(), params["codec"].toString()};
        for (int i = 0; i < format_->count(); ++i) if (format_->itemData(i).toStringList() == wanted) { format_->setCurrentIndex(i); return done(); }
        return invalid("This output format is not offered");
    }
    return invalid("Unknown review action");
}
void ReviewWindow::refreshFrame() {
    QImage image;
    qint64 actual = 0;
    const qint64 at = std::min<qint64>(playhead(), std::max<qint64>(0, options_.durationUs - 1));
    debugLog(QStringLiteral("review: frame at %1").arg(at));
    if (auto e = previewFrame(options_.workingClip, edit_, config_, at, 1280, options_.canvas, image, &actual)) {
        // Never a silent old frame: say so over it until a refresh succeeds.
        debugLog("review: preview failed " + e.message);
        QString reason;
        const QFileInfo clip(options_.workingClip);
        if (!clip.exists()) reason = QStringLiteral("the private recording file is missing");
        else if (!clip.isReadable()) reason = QStringLiteral("the private recording file isn't readable; check its permissions");
        else if (e.message.startsWith("Cannot open") || e.message.startsWith("Cannot read") || e.message.startsWith("Reading")) reason = QStringLiteral("the private recording file couldn't be read; check that it is intact");
        else if (e.message.startsWith("The working clip has no video")) reason = QStringLiteral("the private recording has no readable video track");
        else if (e.message.startsWith("No frame")) reason = QStringLiteral("no video frame is available at this time; try a nearby frame");
        else reason = QStringLiteral("the video frame couldn't be decoded; try a nearby frame or retry the preview");
        view_->setPreviewError(QStringLiteral("The frame at %1 couldn't be decoded (%2).").arg(timeText(at), reason));
        refreshLabels();
        return;
    }
    const bool hadError = !view_->previewError().isEmpty();
    view_->setFrame(image, at);
    if (hadError) refreshLabels();
    debugLog(QStringLiteral("review: frame %1x%2").arg(image.width()).arg(image.height()));
}
void ReviewWindow::startPlayback() {
    if (exporting_ || playing_) return;
    const qint64 end = edit_.exportEnd(options_.durationUs);
    qint64 start = playhead();
    // At or past the retained end, play the retained part again from its start.
    if (start >= end - 1 || start < edit_.trimStart) start = edit_.trimStart;
    disarm();
    playing_ = true;
    view_->setPlaying(true);
    play_->setIcon(VisualStyle::icon("pause"));
    play_->setAccessibleName("Pause");
    play_->setToolTip("Pause the preview (Space)");
    refreshLabels();
    auto stop = std::make_shared<std::atomic_bool>(false);
    playStop_ = stop;
    QPointer<ReviewWindow> guard(this);
    const QString clip = options_.workingClip;
    const QSize canvas = options_.canvas;
    const auto edit = edit_;
    const double speed = std::max(0.1, speed_->currentData().toDouble());
    auto* thread = QThread::create([guard, stop, clip, canvas, edit, start, end, speed] {
        FrameSequence frames;
        bool reachedEnd = false;
        if (!frames.open(clip, start)) {
            QElapsedTimer clock;
            clock.start();
            QImage image;
            qint64 frameUs = 0, shownAt = -1;
            // Clip time the wall clock has reached at this speed.
            const auto due = [&] { return start + qint64(clock.elapsed() * 1000 * speed); };
            // Frames already more than a frame behind the clock are decoded but not
            // drawn, unless nothing has been drawn for a quarter second, so a slow
            // machine or a large clip plays at speed with dropped frames.
            const auto wanted = [&](qint64 at) { return at >= end || at + 40000 >= due() || shownAt < 0 || at - shownAt >= 250000; };
            while (!stop->load() && frames.next(edit, 1280, canvas, image, frameUs, nullptr, wanted)) {
                if (frameUs >= end) { reachedEnd = true; break; }
                while (!stop->load() && due() < frameUs) QThread::msleep(4);
                if (stop->load()) break;
                shownAt = frameUs;
                QMetaObject::invokeMethod(guard, [guard, image, frameUs] {
                    if (!guard || !guard->playing_) return;
                    const bool hadError = !guard->view_->previewError().isEmpty();
                    guard->view_->setFrame(image, frameUs);
                    if (hadError) guard->refreshLabels();
                    guard->timeline_->setPlayhead(frameUs);
                    guard->refreshLabels();
                }, Qt::QueuedConnection);
            }
            if (!stop->load() && !reachedEnd) reachedEnd = true;
        }
        QMetaObject::invokeMethod(guard, [guard, stop, reachedEnd, end] {
            if (!guard || guard->playStop_ != stop) return;
            guard->stopPlayback();
            if (reachedEnd) guard->seek(std::max<qint64>(0, end - 1));
        }, Qt::QueuedConnection);
    });
    playThread_ = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
void ReviewWindow::stopPlayback() {
    if (playStop_) playStop_->store(true);
    if (!playing_) return;
    playing_ = false;
    view_->setPlaying(false);
    play_->setIcon(VisualStyle::icon("play"));
    play_->setAccessibleName("Play");
    play_->setToolTip("Play the retained part silently (Space)");
    refreshLabels();
}
ReviewWindow::~ReviewWindow() {
    if (playStop_) playStop_->store(true);
    if (playThread_) playThread_->wait();
}
bool ReviewWindow::chooseCopy() {
    if (exporting_ || chooser_->isOpen()) return false;
    const auto extension = config_.extension();
    FileDialog::Options options{FileDialog::Mode::Save, "Save a copy", QDir::homePath(), "recording." + extension, {{config_.format.toUpper(), {"*." + extension}}}};
    return chooser_->open(options, [this, extension](const FileDialog::Result& result) {
        if (result.failed()) { showError("Couldn't save a copy there: " + result.error, false); return; }
        if (!result.chosen() || exporting_) return;
        const auto& path = result.path;
        emit saveCopyRequested(path.endsWith("." + extension, Qt::CaseInsensitive) ? path : path + "." + extension, edit_.revision);
    });
}
void ReviewWindow::hideEvent(QHideEvent* e) {
    // Upload, cancel and a stop elsewhere hide the review: its chooser goes too
    // (minimizing is a spontaneous hide and leaves it open).
    if (!e->spontaneous()) {
        chooser_->cancel();
        if (discard_) answerDiscard(false);
        if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); }
    }
    QWidget::hideEvent(e);
}
void ReviewWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !(e->modifiers() & ~Qt::KeypadModifier) && !exporting_) { if (playing_) stopPlayback(); else startPlayback(); return; }
    QWidget::keyPressEvent(e);
}
void ReviewWindow::closeEvent(QCloseEvent* e) {
    // Closing the window would discard the recording: ask first, as Discard… does.
    if (!closing_ && !exporting_) { e->ignore(); askDiscard(); return; }
    e->accept();
}
void ReviewWindow::askDiscard() {
    if (exporting_ || closing_) return;
    if (discard_) { discard_->raise(); discard_->activateWindow(); return; }
    stopPlayback();
    view_->commitNudge();
    auto* box = Graphite::confirm(this, QStringLiteral("Discard recording"),
        QStringLiteral("Discard this recording and its edits?\nThe recording, its trim and its masks are deleted. Nothing has been uploaded; copies you saved stay."),
        QStringLiteral("Discard recording"), QStringLiteral("Keep reviewing"), true);
    auto* keep = box->button(QMessageBox::Cancel);
    keep->setObjectName("keepReviewing");
    box->setWindowModality(Qt::WindowModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box] {
        const bool discard = box->clickedButton() == box->findChild<QPushButton*>("confirmDiscard");
        if (discard_ == box) discard_ = nullptr;
        if (!discard || closing_) return;
        closing_ = true;
        emit cancelRequested();
    });
    discard_ = box;
    box->open();
    keep->setFocus();
}
void ReviewWindow::answerDiscard(bool discard) {
    if (!discard_) return;
    auto* box = discard_.data();
    if (auto* button = box->findChild<QPushButton*>(discard ? "confirmDiscard" : "keepReviewing")) button->click();
    else box->reject();
}
}
