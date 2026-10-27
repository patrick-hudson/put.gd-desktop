#include "ReviewWindow.h"
#include "ui/BlurRadius.h"
#include "EffectiveAudio.h"
#include <QGridLayout>
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
#include <QTextBrowser>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
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
}

namespace {
// Media overlays use fixed colours, not the app theme: they sit on captured
// content, which is as likely pale as dark (Graphite, Recording).
const QColor mediaAccent(0xE8, 0xB6, 0x6D);
QString clock(qint64 us) { const qint64 h = us / 10000; return QStringLiteral("%1:%2.%3").arg(h / 6000).arg((h / 100) % 60, 2, 10, QChar('0')).arg(h % 100, 2, 10, QChar('0')); }
QString seconds(qint64 us) {
    auto text = QString::number(double(us / 10000) / 100, 'f', 2);
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}
// Reserve the inactive suffix too, so playhead changes never reflow the frame.
class MaskInterval final : public QLabel {
public:
    using QLabel::QLabel;
    void setInterval(const QString& interval, bool active) {
        reserved_ = interval + QStringLiteral(" · Inactive at playhead");
        setText(active ? interval : reserved_);
    }
    QSize minimumSizeHint() const override {
        auto hint = QLabel::minimumSizeHint();
        hint.setHeight(inkHeight());
        return hint;
    }
    int heightForWidth(int width) const override {
        if (fontMetrics().horizontalAdvance(reserved_) <= width) return inkHeight();
        return fontMetrics().boundingRect(QRect(0, 0, std::max(1, width), 10000), Qt::TextWordWrap, reserved_).height();
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        if (fontMetrics().horizontalAdvance(reserved_) > contentsRect().width()) { QLabel::paintEvent(event); return; }
        // Match Graphite's optical baseline; line leading need not pad a single fact.
        QPainter painter(this);
        Graphite::paintText(painter, contentsRect(), text(), palette().color(foregroundRole()), font(), alignment(), Qt::ElideNone);
    }
private:
    int inkHeight() const { return fontMetrics().tightBoundingRect(reserved_).height() + 2; }
    QString reserved_;
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
    row->setContentsMargins(14, 8, 8, 8);
    row->setSpacing(12);
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
    const QString text = frame_.isNull() ? message : message + QStringLiteral("\nShowing an earlier frame (%1).").arg(clock(timeUs_));
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
            .arg(maskNames_->name(m)).arg(m.rect.width()).arg(m.rect.height()).arg(m.rect.x()).arg(m.rect.y()).arg(clock(m.start), clock(m.end));
        break;
    }
    if (!error_.isEmpty()) text.prepend(error_ + QStringLiteral(". "));
    if (accessibleDescription() == text) return;
    setAccessibleDescription(text);
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}
QRect FrameView::imageRect() const {
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (source.isEmpty()) return rect();
    const QSize scaled = source.scaled(size(), Qt::KeepAspectRatio);
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
QRectF mediaLabelRect(const QPointF& at, const QString& text, const QRectF& bounds) {
    const QFontMetricsF metrics(Graphite::font(Graphite::Type::metadata, true));
    QRectF box(at, QSizeF(std::min(metrics.horizontalAdvance(text) + 12, bounds.width()), metrics.height() + 6));
    if (box.right() > bounds.right()) box.moveRight(bounds.right());
    if (box.bottom() > bounds.bottom()) box.moveBottom(bounds.bottom());
    return box;
}
void mediaLabel(QPainter& painter, const QPointF& at, const QString& text, const QRectF& bounds) {
    const auto font = Graphite::font(Graphite::Type::metadata, true);
    const QRectF box = mediaLabelRect(at, text, bounds);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 175));
    painter.drawRoundedRect(box, 4, 4);
    Graphite::paintText(painter, box.adjusted(6, 0, -6, 0), text, Theme::onMedia(), font);
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
    painter.fillRect(rect(), QColor("#0d0f13"));
    const auto target = imageRect();
    if (!frame_.isNull()) painter.drawImage(target, frame_);
    // An earlier frame under a preview error is dimmed so it never reads as current.
    if (stale()) painter.fillRect(target, QColor(0, 0, 0, 140));
    const QRectF selectedView = [&] { for (const auto& m : masks_) if (m.id == selected_) return toView(m.rect); return QRectF(); }();
    for (const auto& mask : masks_) {
        const bool active = mask.start <= timeUs_ && timeUs_ < mask.end;
        const bool selected = mask.id == selected_;
        const auto r = toView(mask.rect);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mask.kind == "solid" ? QBrush(QColor(0, 0, 0, active ? 0 : 60)) : QBrush(QColor(255, 255, 255, active ? 0 : 30)));
        painter.drawRect(r);
        dualOutline(painter, r, selected ? mediaAccent : active ? QColor(255, 255, 255, 230) : QColor(255, 255, 255, 150), selected ? 2 : 1, active ? Qt::SolidLine : Qt::DashLine);
        if (selected) { painter.setBrush(mediaAccent); painter.setPen(QPen(QColor(0, 0, 0, 170), 1)); painter.drawRect(QRectF(r.right() - handleSize, r.bottom() - handleSize, handleSize, handleSize)); }
    }
    // Names last, over every outline. An unselected name that would not fit
    // inside its mask, or would cover the selected one's, is left to the list
    // and the timeline, so small frames never pile labels up.
    for (const auto& mask : masks_) {
        const bool selected = mask.id == selected_;
        const auto r = toView(mask.rect);
        const auto label = mediaLabelRect(r.topLeft() + QPointF(3, 3), maskNames_->name(mask), target);
        if (!selected && (label.width() > r.width() - 4 || label.height() > r.height() - 4 || label.intersects(selectedView))) continue;
        mediaLabel(painter, r.topLeft() + QPointF(3, 3), maskNames_->name(mask), target);
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
void FrameView::mouseMoveEvent(QMouseEvent* e) {
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
// Timeline geometry (#283): the clip track, then one thin lane per row of masks.
constexpr int trackPad = 16, trackTop = 10, trackHeight = 12, laneTop = 32, grip = 16;
int visibleLanes() { return 1; }
int laneHeight() { return Graphite::scaled(9); }
int laneStep() { return std::max(32, laneHeight()); }
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
    laneScroll_ = new QScrollBar(Qt::Vertical, this);
    laneScroll_->setObjectName("maskLaneScroll");
    laneScroll_->setAccessibleName("Mask lanes");
    laneScroll_->setPageStep(visibleLanes());
    laneScroll_->hide();
    connect(laneScroll_, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    setObjectName("timeline");
    setAccessibleName("Timeline playhead (seconds)");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}
void ReviewTimeline::setDuration(qint64 us) { duration_ = std::max<qint64>(1, us); trimEnd_ = std::min(trimEnd_, duration_); update(); }
void ReviewTimeline::setTrim(qint64 start, qint64 end) { trimStart_ = start; trimEnd_ = end; update(); }
void ReviewTimeline::setMasks(const QList<RecordingMask>& masks, const QString& selected) {
    maskNames_->observe(masks);
    masks_ = masks;
    selected_ = selected;
    const int before = lanes_;
    layoutLanes();
    laneScroll_->setRange(0, std::max(0, lanes_ - visibleLanes()));
    laneScroll_->setVisible(lanes_ > visibleLanes());
    for (int i = 0; i < masks_.size(); ++i) if (masks_[i].id == selected_) {
        const int lane = lane_[i];
        if (lane < laneScroll_->value()) laneScroll_->setValue(lane);
        else if (lane >= laneScroll_->value() + visibleLanes()) laneScroll_->setValue(lane - visibleLanes() + 1);
    }
    if (lanes_ != before) updateGeometry();
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
QRect ReviewTimeline::trackRect() const { return QRect(trackPad, trackTop, std::max(1, width() - 2 * trackPad - (laneScroll_->isHidden() ? 0 : laneScroll_->sizeHint().width())), trackHeight); }
QRect ReviewTimeline::laneRect(int lane) const { const auto track = trackRect(); return QRect(track.left(), laneTop + (lane - laneScroll_->value()) * laneStep() + laneStep() / 2 - (laneHeight() - 1) / 2, track.width(), laneHeight()); }
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
    return bar.isNull() ? QRect{} : QRect(bar.left() - 16, bar.center().y() - 16, std::max(32, bar.width() + 32), 32);
}
QSize ReviewTimeline::sizeHint() const { return {480, minimumSizeHint().height()}; }
QSize ReviewTimeline::minimumSizeHint() const { return {280, lanes_ ? laneTop + std::min(lanes_, visibleLanes()) * laneStep() : 32}; }
void ReviewTimeline::paintEvent(QPaintEvent*) {
    // Graphite: a quiet track, the retained interval framed in amber between
    // two grip handles, lightly dimmed trimmed-away ends, mask bars in lanes
    // and the playhead with its marker.
    const auto& p = Theme::current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto track = trackRect();
    const bool trimming = drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd;
    const qint64 start = trimming ? dragStart_ : trimStart_, end = trimming ? dragEnd_ : trimEnd_;
    painter.setPen(QPen(p.border, 1));
    painter.setBrush(p.surfaceRaised);
    painter.drawRoundedRect(QRectF(track).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    const QColor edge = isEnabled() ? p.accent : p.borderStrong;
    QColor kept = p.accent;
    kept.setAlpha(isEnabled() ? 70 : 30);
    const QRectF retained(xFor(start), track.top() + 0.5, xFor(end) - xFor(start), track.height() - 1);
    painter.setPen(QPen(edge, 1.5));
    painter.setBrush(kept);
    painter.drawRect(retained);
    painter.setPen(Qt::NoPen);
    for (const qint64 at : {start, end}) {
        const QRectF grip(xFor(at) - 3.5, track.top() - 3, 7, track.height() + 6);
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
        painter.drawRoundedRect(bar, 3, 3);
        // The kind at lane size: a blur is hatched, a solid cover filled.
        QColor ink = selected ? p.accent : p.textMuted;
        ink.setAlpha(mask.kind == "solid" ? 120 : 200);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mask.kind == "solid" ? QBrush(ink) : QBrush(ink, Qt::BDiagPattern));
        painter.drawRoundedRect(bar.adjusted(1.5, 1.5, -1.5, -1.5), 2, 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(selected ? p.accent : p.textMuted);
        painter.drawRoundedRect(QRectF(bar.left(), bar.top(), 2.5, bar.height()), 1, 1);
        const auto font = Graphite::font(Graphite::Type::metadata);
        // Thin lanes retain their pattern; the selected name stays full-size in the chooser.
        if (bar.width() > QFontMetrics(font).horizontalAdvance(maskNames_->name(mask)) + 12 && lane.height() >= QFontMetrics(font).height() + 4) {
            Graphite::paintText(painter, bar.adjusted(7, 0, -4, 0), maskNames_->name(mask), selected ? p.text : p.textSecondary, font);
        }
        if (selected) { painter.setBrush(p.accent); for (const int x : {left, right}) painter.drawRoundedRect(QRectF(x - 1.5, lane.top() - 2, 3, lane.height() + 4), 1, 1); }
    }
    // The playhead spans the track and the lanes.
    const int x = xFor(playhead_);
    painter.setPen(QPen(p.text, 1.5));
    painter.drawLine(QPointF(x, 3), QPointF(x, height() - 1));
    painter.setPen(Qt::NoPen);
    painter.setBrush(p.text);
    painter.drawPolygon(QPolygonF{QPointF(x - 4, 0), QPointF(x + 4, 0), QPointF(x, 5)});
    if (Graphite::keyboardFocus(this)) { painter.setPen(QPen(p.accent, 1)); painter.setBrush(Qt::NoBrush); painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5); }
}
void ReviewTimeline::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    const int width = laneScroll_->sizeHint().width();
    laneScroll_->setGeometry(this->width() - width, laneTop, width, std::max(1, height() - laneTop));
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
        const QRectF target(x - grip, std::clamp(y - 16, 0, std::max(0, height() - 32)), 32, 32);
        if (!target.contains(pos)) return;
        const auto distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (distance < nearest) { nearest = distance; result = kind; }
    };
    // Trim and masks have separate interaction rows; nearest wins within a row.
    const auto row = maskAt(pos);
    if (row.isEmpty()) {
        offer(xFor(trimStart_), trackRect().center().y(), Drag::TrimStart);
        offer(xFor(trimEnd_), trackRect().center().y(), Drag::TrimEnd);
    }
    if (const auto bar = maskRect(selected_); !bar.isNull() && row == selected_) {
        offer(bar.left(), bar.center().y(), Drag::MaskStart);
        offer(bar.left() + bar.width(), bar.center().y(), Drag::MaskEnd);
    }
    return result;
}
QString ReviewTimeline::maskAt(QPointF pos) const {
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
    // #283: the frame takes the window; the timeline, one row of tools and the
    // action footer sit under it.
    auto* frame = new QVBoxLayout(this);
    frame->setSizeConstraint(QLayout::SetNoConstraint);
    frame->setContentsMargins(0, 0, 0, 0);
    frame->setSpacing(0);
    // A one-line title preserves the frame area; the draft state stays in the
    // footer. With desktop decorations the compact header hides itself.
    header_ = Graphite::WindowHeader::install(this, frame, nullptr, Graphite::WindowHeader::Density::Compact);
    header_->setDocument(QStringLiteral("Review recording"));
    auto* root = new QVBoxLayout;
    // Compact bands leave room for the video even with enlarged text and status.
    root->setContentsMargins(12, 0, 12, 0);
    root->setSpacing(0);
    frame->addLayout(root, 1);
    // Why the capture ended when it stopped itself (#225); hidden otherwise.
    notice_ = new QLabel(this);
    notice_->setObjectName("notice");
    VisualStyle::role(notice_, "notice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setAccessibleName("Why the recording stopped");
    notice_->hide();
    root->addWidget(notice_);
    view_ = new FrameView(this, maskNames_);
    view_->setObjectName("frame");
    view_->setMinimumHeight(180);
    view_->setCanvas(options.canvas);
    view_->setAccessibleName("Recording frame");
    root->addWidget(view_, 1);
    auto tool = [this](const char* icon, const QString& name, const QString& tip) {
        auto* button = new QToolButton(this);
        button->setIcon(VisualStyle::icon(icon));
        button->setIconSize(QSize(20, 20));
        button->setToolTip(tip);
        button->setAccessibleName(name);
        button->setAutoRaise(true);
        button->setFixedSize(32, 32);
        return button;
    };
    // A short visible name for a group of icon tools, so Trim and Mask read apart.
    auto groupLabel = [this](const QString& text, const QString& name) { auto* label = new QLabel(text, this); label->setObjectName(name); VisualStyle::role(label, "meta"); return label; };
    // Timecodes in Plex Mono, as on the recording bar: digits keep their places.
    auto mono = [](QLabel* label, int pixels) { Graphite::setMono(label, pixels); };
    // The transport: play and speed, the timeline, the playhead time.
    transportGrid_ = new QGridLayout;
    transportGrid_->setContentsMargins(0, 0, 0, 0);
    transportGrid_->setSpacing(0);
    transportRow_ = new QHBoxLayout;
    auto* transport = transportRow_;
    transport->setSpacing(6);
    play_ = tool("play", "Play", "Play the retained part silently (Space)");
    play_->setObjectName("play");
    speed_ = VisualStyle::choiceBox(this);
    speed_->setObjectName("speed");
    speed_->setToolTip("Playback speed of the preview");
    speed_->setAccessibleName("Playback speed");
    for (const auto& [label, factor] : std::initializer_list<std::pair<const char*, double>>{{"0.5×", 0.5}, {"1×", 1.0}, {"2×", 2.0}, {"4×", 4.0}}) speed_->addItem(QString::fromUtf8(label), factor);
    speed_->setCurrentIndex(1);
    formatAdvice_ = tool("info", "Format tips", "Use MP4 (H.264) for broader playback compatibility. Show format tips.");
    formatAdvice_->setObjectName("formatAdvice");
    formatAdvice_->setProperty("driveAction", QJsonObject{{"action", "showAdvice"}});
    formatAdvice_->hide();
    connect(formatAdvice_, &QToolButton::clicked, this, [this] { drive({{"action", "showAdvice"}}); });
    timeline_ = new ReviewTimeline(this, maskNames_);
    timeline_->setMinimumWidth(280);
    timeline_->setDuration(options.durationUs);
    timeline_->setToolTip("Click or drag to scrub; drag the trim handles to trim. Drag a mask to move it in time, or a selected mask's ends to retime them. Arrows step, Home/End jump to the kept part.");
    time_ = new QLabel(this);
    time_->setObjectName("time");
    VisualStyle::role(time_, "secondary");
    mono(time_, Graphite::Type::metadata);
    time_->setAccessibleName("Playhead time");
    time_->setMinimumWidth(0);
    time_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    transport->addWidget(play_);
    transport->addWidget(speed_);
    transport->addWidget(formatAdvice_);
    transport->addWidget(timeline_, 1);
    auto* playbackInfo = new QWidget(this);
    auto* playbackLabels = new QVBoxLayout(playbackInfo);
    playbackLabels->setContentsMargins(0, 0, 0, 0);
    playbackLabels->setSpacing(2);
    playbackLabels->addWidget(time_);
    transport->addWidget(playbackInfo);
    transportGrid_->addLayout(transport, 0, 0, 1, 2);
    root->addLayout(transportGrid_);
    toolsGrid_ = new QGridLayout;
    toolsGrid_->setContentsMargins(0, 0, 0, 0);
    toolsGrid_->setSpacing(0);
    trimGroup_ = new QWidget(this);
    trimGroup_->setObjectName("trimControls");
    maskGroup_ = new Graphite::Surface("bg", Qt::LeftEdge, this);
    maskGroup_->setObjectName("maskControls");
    auto* trimRow = new QHBoxLayout(trimGroup_);
    auto* maskGroupRow = new QHBoxLayout(maskGroup_);
    for (auto* row : {trimRow, maskGroupRow}) { row->setContentsMargins(0, 0, 0, 0); row->setSpacing(4); }
    maskGroupRow->setContentsMargins(8, 0, 0, 0);
    trimStart_ = tool("trim-in", "Set trim start", "Keep from the playhead: set the trim start here (I)");
    trimEnd_ = tool("trim-out", "Set trim end", "Keep up to the playhead: set the trim end here (O)");
    resetTrim_ = tool("trim-reset", "Reset trim", "Keep the whole recording");
    retained_ = new QLabel(this);
    retained_->setObjectName("muted");
    VisualStyle::role(retained_, "secondary");
    mono(retained_, Graphite::Type::metadata);
    retained_->setAccessibleName("Kept interval");
    addBlur_ = tool("blur", "Blur", "Add a blur, then drag on the frame to place it or press Enter for a centered one. Blur is a visual effect; use Redact for private information.");
    addSolid_ = tool("redact", "Redact", "Redact: add a redaction, then drag on the frame to place it or press Enter for a centered one");
    for (auto* button : {addBlur_, addSolid_}) button->setCheckable(true);
    // The selected mask's settings; the whole group shows only once masks exist.
    maskTools_ = new QWidget(this);
    maskTools_->setObjectName("maskTools");
    masks_ = VisualStyle::choiceBox(maskTools_);
    masks_->setObjectName("masks");
    masks_->setAccessibleName("Masks");
    masks_->setToolTip("The mask to edit; click one on the frame or the timeline to select it");
    masks_->setPlaceholderText("Select a mask");
    masks_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    masks_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    masks_->setMinimumWidth(masks_->fontMetrics().horizontalAdvance(QStringLiteral("Redaction 8")) + Graphite::Metric::splitZone + 24);
    maskTools_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    maskStart_ = tool("mask-in", "Set mask start", "Start the selected mask at the playhead (Shift+I)");
    maskEnd_ = tool("mask-out", "Set mask end", "End the selected mask at the playhead (Shift+O)");
    for (auto* button : {trimStart_, trimEnd_, resetTrim_, maskStart_, maskEnd_}) {
        button->setText(button == resetTrim_ ? "Reset" : button == trimStart_ || button == maskStart_ ? "Start" : "End");
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        VisualStyle::role(button, "link");
        button->setMinimumSize((button->sizeHint() + QSize(4, 0)).expandedTo(QSize(32, 32)));
        button->setFixedHeight(std::max(32, button->fontMetrics().height() + 2));
    }
    auto* radius = new BlurRadius(2, 64, 16, true, maskTools_);
    strengthControl_ = radius;
    strength_ = radius->spinBox();
    removeMask_ = tool("remove", "Remove mask", "Remove the selected mask (Delete)");
    auto* maskLayout = new QVBoxLayout(maskTools_);
    maskLayout->setContentsMargins(0, 0, 0, 0);
    maskLayout->setSpacing(0);
    auto* maskRow = new QHBoxLayout;
    maskLayout->addLayout(maskRow);
    maskRow->setContentsMargins(0, 0, 0, 0);
    maskRow->setSpacing(4);
    maskActive_ = new MaskInterval(maskTools_);
    maskActive_->setObjectName("maskActiveInterval");
    maskActive_->setWordWrap(true);
    VisualStyle::role(maskActive_, "meta");
    Graphite::setMono(maskActive_, Graphite::Type::micro);
    maskLayout->addWidget(maskActive_);
    maskRow->addWidget(masks_, 1);
    for (auto* w : std::initializer_list<QWidget*>{maskStart_, maskEnd_, strengthControl_, removeMask_}) maskRow->addWidget(w);
    for (auto* w : std::initializer_list<QWidget*>{groupLabel("Trim", "trimGroup"), trimStart_, trimEnd_, resetTrim_, retained_}) trimRow->addWidget(w);
    trimRow->addStretch();
    for (auto* w : std::initializer_list<QWidget*>{groupLabel("Mask", "maskGroup"), addSolid_, addBlur_, maskTools_}) maskGroupRow->addWidget(w);
    maskGroupRow->setStretchFactor(maskTools_, 1);
    maskGroupRow->addStretch();
    toolsGrid_->addWidget(trimGroup_, 0, 0);
    toolsGrid_->addWidget(maskGroup_, 0, 1);
    root->addLayout(toolsGrid_);
    // Status belongs below the frame, so no guidance or recovery action hides video.
    statusRow_ = new Graphite::Surface("raised", Qt::TopEdge | Qt::BottomEdge, this);
    statusRow_->setObjectName("statusRow");
    statusLines_ = new QVBoxLayout(statusRow_);
    statusLines_->setContentsMargins(10, 0, 10, 0);
    statusLines_->setSpacing(2);
    statusRow_->hide();
    root->insertWidget(root->indexOf(view_) + 1, statusRow_);
    outputTools_ = new QWidget(this);
    auto* actions = new QHBoxLayout(outputTools_);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    format_ = VisualStyle::choiceBox(this);
    format_->setAccessibleName("Output format");
    for (const auto& format : options.formats) {
        const auto codecs = options.codecs[format].toArray();
        if (codecs.isEmpty()) format_->addItem(formatLabel(format, {}), QStringList{format, QString()});
        for (const auto& codec : codecs) { format_->addItem(formatLabel(format, codec.toString()), QStringList{format, codec.toString()}); format_->setItemData(format_->count() - 1, formatHint(format, codec.toString()), Qt::ToolTipRole); }
    }
    // What the export carries for sound, in a few words that never elide.
    audio_ = new QLabel(this);
    audio_->setObjectName("audioSummary");
    VisualStyle::role(audio_, "meta");
    exportAudio_ = new QLabel(this);
    exportAudio_->setObjectName("exportAudioSummary");
    VisualStyle::role(exportAudio_, "micro");
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
    VisualStyle::role(draft, "micro");

    actions->addWidget(format_);
    audio_->hide();
    playbackInfo->setMaximumWidth(std::max(190, time_->fontMetrics().horizontalAdvance(QStringLiteral("0:00.00 / 0:00.00"))));
    outputTools_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    destination_ = new Graphite::ElidedLabel(this);
    destination_->setText(options.uploadSummary);
    destination_->setRevealable(true);
    destination_->setRevealText("Destination: " + options.destination + "\nSharing: " + (options.sharingMode == "private" ? "Private upload" : "Shared link"));
    destination_->setObjectName("uploadDestination");
    destination_->setTextFormat(Qt::PlainText);
    destination_->setAccessibleName(options.uploadSummary);
    destination_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    VisualStyle::role(destination_, "secondary");
    destination_->setVisible(!options.uploadSummary.isEmpty());
    auto* footer = new Graphite::Surface("bg", Qt::TopEdge, this);
    footer->setObjectName("reviewFooter");
    footerGrid_ = new QGridLayout(footer);
    footerGrid_->setContentsMargins(12, 0, 12, 0);
    footerGrid_->setHorizontalSpacing(8);
    footerGrid_->setVerticalSpacing(0);
    destinationRow_ = new QWidget(footer);
    auto* destinationLayout = new QHBoxLayout(destinationRow_);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    destinationLayout->setSpacing(4);
    destinationFull_ = new QLabel(options.uploadSummary, destinationRow_);
    destinationFull_->setObjectName("uploadDestinationWrapped");
    destinationFull_->setTextFormat(Qt::PlainText);
    destinationFull_->setWordWrap(true);
    destinationFull_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    destinationFull_->setFocusPolicy(Qt::StrongFocus);
    VisualStyle::role(destinationFull_, "secondary");
    destinationFull_->hide();
    destinationLayout->addWidget(destinationFull_, 1);
    destinationLayout->addWidget(destination_);
    auto* facts = new QWidget(footer);
    facts->setObjectName("reviewMetadata");
    auto* factsRow = new QHBoxLayout(facts);
    factsRow->setContentsMargins(0, 0, 0, 0);
    factsRow->setSpacing(8);
    factsRow->addWidget(exportAudio_, 1);
    auto* divider = new Graphite::Surface("bg", Qt::LeftEdge, facts);
    divider->setFixedWidth(1);
    factsRow->addWidget(divider);
    factsRow->addWidget(draft);
    errorDetails_ = new QPushButton("Recording error · Details…", facts);
    errorDetails_->setObjectName("reviewErrorDetails");
    errorDetails_->setProperty("driveAction", QJsonObject{{"action", "showAdvice"}});
    VisualStyle::role(errorDetails_, "link");
    errorDetails_->setFixedHeight(std::max(32, errorDetails_->fontMetrics().height() + 4));
    errorDetails_->hide();
    connect(errorDetails_, &QPushButton::clicked, this, [this] { drive({{"action", "showAdvice"}}); });
    factsRow->addWidget(errorDetails_);

    exportAudio_->setWordWrap(true);
    footerGrid_->addWidget(facts, 1, 0, 1, 5);
    footerGrid_->addWidget(destinationRow_, 2, 1);
    footerGrid_->addWidget(outputTools_, 2, 0);
    footerGrid_->addWidget(save_, 2, 2);
    footerGrid_->addWidget(cancel_, 2, 3);
    footerGrid_->addWidget(upload_, 2, 4);
    footerGrid_->setColumnStretch(1, 1);
    frame->addWidget(footer);
    placeDestination();
    // Text stays enlarged; the review's fields need less vertical padding than forms.
    for (auto* field : std::initializer_list<QWidget*>{speed_, masks_, strength_, format_})
        field->setFixedHeight(std::max(32, field->fontMetrics().height() + 2));
    for (auto* action : std::initializer_list<QWidget*>{upload_, save_, cancel_})
        action->setFixedHeight(std::max(32, action->fontMetrics().height() + 2));
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
    for (auto* button : {trimStart_, trimEnd_, resetTrim_, addBlur_, addSolid_, removeMask_, maskStart_, maskEnd_}) connect(button, &QToolButton::clicked, this, &ReviewWindow::stopPlayback);
    for (auto* button : {upload_, save_, cancel_}) connect(button, &QPushButton::clicked, this, &ReviewWindow::stopPlayback);
    connect(trimStart_, &QToolButton::clicked, this, [this] { setTrimAt(true); });
    connect(trimEnd_, &QToolButton::clicked, this, [this] { setTrimAt(false); });
    connect(resetTrim_, &QToolButton::clicked, this, [this] { propose(QJsonArray{QJsonObject{{"op", "resetTrim"}}}); });
    connect(addBlur_, &QToolButton::clicked, this, [this](bool on) { if (on) armMask("blur"); else { disarm(); refreshLabels(); } });
    connect(addSolid_, &QToolButton::clicked, this, [this](bool on) { if (on) armMask("solid"); else { disarm(); refreshLabels(); } });
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
    // Top to bottom: the frame (keyboard mask editing), the transport, the tools, the footer.
    const QList<QWidget*> order{view_, play_, speed_, formatAdvice_, timeline_, trimStart_, trimEnd_, resetTrim_, addSolid_, addBlur_, masks_, maskStart_, maskEnd_, strength_, removeMask_, format_, save_, cancel_, upload_, destination_};
    for (int i = 1; i < order.size(); ++i) setTabOrder(order[i - 1], order[i]);
    timeline_->setFocus();
    // The default fits a 1366×768 desktop; a smaller screen gets what it has.
    QSize size = defaultSize().boundedTo(availableReviewSize());
    resize(size);
    placeTools();
    updateMinimum();
}
QSize ReviewWindow::defaultSize() {
    // Reserve the mask lane and transient guidance on first presentation.
    return {820, Graphite::WindowHeader::customFrame() ? 720 : 672};
}

RecordingMask* ReviewWindow::selectedMask() {
    auto it = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == selectedMask_; });
    return it == edit_.masks.end() ? nullptr : &*it;
}
const RecordingMask* ReviewWindow::selectedMask() const { return const_cast<ReviewWindow*>(this)->selectedMask(); }
QString ReviewWindow::timeText(qint64 us) const { return clock(us); }
void ReviewWindow::propose(const QJsonArray& commands) { if (!exporting_) emit editProposed(edit_.revision, commands); }
qint64 ReviewWindow::playhead() const { return timeline_->playhead(); }
void ReviewWindow::seek(qint64 us) { timeline_->setPlayhead(us); refreshLabels(); frameTimer_.start(); }
void ReviewWindow::selectMask(const QString& id) {
    selectedMask_ = std::any_of(edit_.masks.cbegin(), edit_.masks.cend(), [&](const RecordingMask& m) { return m.id == id; }) ? id : QString();
    refreshMaskList();
    view_->setMasks(edit_.masks, selectedMask_);
    refreshTimeline();
    refreshLabels();
}
void ReviewWindow::armMask(const QString& kind) {
    stopPlayback();
    addBlur_->setChecked(kind == "blur");
    addSolid_->setChecked(kind == "solid");
    view_->armCreate(kind);
    for (auto* s : {escape_, place_, placeEnter_}) s->setEnabled(true);
    refreshLabels();
}
void ReviewWindow::disarm() {
    view_->disarm();
    addBlur_->setChecked(false);
    addSolid_->setChecked(false);
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
    const auto refuse = [this](const QString& message) { showError(message); return message; };
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
    const auto refuse = [this](const QString& message) { showError(message); return message; };
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
    refreshLabels();
}
void ReviewWindow::setExporting(bool exporting, const QString& label) {
    stopPlayback();
    if (exporting) { disarm(); chooser_->cancel(); if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); } }
    if (exporting && discard_) answerDiscard(false);
    exporting_ = exporting;
    for (auto* w : std::initializer_list<QWidget*>{upload_, save_, cancel_, trimStart_, trimEnd_, resetTrim_, addBlur_, addSolid_, removeMask_, maskStart_, maskEnd_, strength_, masks_, format_, play_, speed_, timeline_, view_}) w->setEnabled(!exporting);
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
void ReviewWindow::showError(const QString& message) {
    message_ = message.isEmpty() ? StatusEntry{} : StatusEntry{message, QStringLiteral("error"), {{QStringLiteral("Dismiss"), QJsonObject{{"action", "dismissStatus"}}}}};
    refreshLabels();
}
void ReviewWindow::setNotice(const QString& text) {
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
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
        masks_->addItem(maskNames_->name(m));
        masks_->setItemData(i, QStringLiteral("%1 %2×%3 at %4,%5 · %6–%7").arg(maskNames_->name(m)).arg(m.rect.width()).arg(m.rect.height()).arg(m.rect.x()).arg(m.rect.y()).arg(timeText(m.start), timeText(m.end)), Qt::ToolTipRole);
        if (m.id == selectedMask_) row = i;
    }
    masks_->setCurrentIndex(row);
    masks_->blockSignals(false);
    int widest = masks_->fontMetrics().horizontalAdvance(QStringLiteral("Redaction 8"));
    for (int i = 0; i < masks_->count(); ++i) widest = std::max(widest, masks_->fontMetrics().horizontalAdvance(masks_->itemText(i)));
    masks_->setMinimumWidth(widest + Graphite::Metric::splitZone + 24);
}
void ReviewWindow::refreshTimeline() {
    timeline_->setTrim(edit_.trimStart, edit_.exportEnd(options_.durationUs));
    timeline_->setMasks(edit_.masks, selectedMask_);
}
void ReviewWindow::refreshLabels() {
    const qint64 at = playhead();
    const auto end = edit_.exportEnd(options_.durationUs);
    const bool retained = at >= edit_.trimStart && at < end;
    time_->setText(QStringLiteral("%1 / %2").arg(timeText(at), timeText(options_.durationUs)) + (retained ? QString() : QStringLiteral("\nOutside kept part")));
    time_->setToolTip(retained ? QStringLiteral("Playhead time / recording length") : QStringLiteral("The playhead is in a trimmed-away part; it is not exported"));
    retained_->setText(QStringLiteral("Kept: %1 s").arg(seconds(end - edit_.trimStart)));
    retained_->setToolTip(QStringLiteral("Keeping %1–%2 (%3 of %4)").arg(timeText(edit_.trimStart), timeText(end), timeText(end - edit_.trimStart), timeText(options_.durationUs)));
    timeline_->setAccessibleDescription(QStringLiteral("Playhead %1; keeping %2–%3; %4 masks").arg(timeText(at), timeText(edit_.trimStart), timeText(end)).arg(edit_.masks.size()));
    // Mask settings are contextual (#283): the list once masks exist, the
    // times and Remove for a selected mask, the strength for a selected blur.
    const auto* mask = selectedMask();
    maskTools_->setVisible(!edit_.masks.isEmpty());
    maskActive_->setVisible(mask != nullptr);
    if (mask) static_cast<MaskInterval*>(maskActive_)->setInterval(QStringLiteral("Applies %1–%2").arg(clock(mask->start), clock(mask->end)), at >= mask->start && at < mask->end);
    for (auto* w : std::initializer_list<QWidget*>{maskStart_, maskEnd_, removeMask_}) w->setVisible(mask != nullptr);
    strengthControl_->setVisible(mask && mask->kind == "blur");
    if (mask) { strength_->blockSignals(true); strength_->setValue(mask->strength); strength_->blockSignals(false); }
    // Preview and export audio stay together, independently of the destination.
    const bool includesAudio = effectiveAudio(config_, options_.audioRecorded, audioIntent_);
    const QString audio = QStringLiteral("Silent preview · ") + (!options_.audioRecorded ? QStringLiteral("No audio recorded") : config_.silent() ? QStringLiteral("No audio in %1").arg(config_.format.toUpper()) : includesAudio ? QStringLiteral("Export includes system audio") : QStringLiteral("Audio off in export"));
    audio_->setText(QStringLiteral("Silent preview"));
    audio_->setProperty("summary", audio);
    audio_->setToolTip(!options_.audioRecorded ? QStringLiteral("No system audio was recorded") : config_.silent() ? QStringLiteral("System audio was recorded; %1 exports are silent. Choose MP4 or WebM to keep it.").arg(config_.format.toUpper()) : includesAudio ? QStringLiteral("The export keeps the recorded system audio; the preview is silent") : QStringLiteral("System audio is turned off for this export"));
    audio_->setAccessibleName(QStringLiteral("Audio: ") + audio);
    exportAudio_->setText(audio);
    exportAudio_->setAccessibleName(audio);
    exportAudio_->setToolTip(audio_->toolTip());
    formatAdvice_->setVisible(compactStatus_ || (config_.format == "mp4" && config_.codec == "h265" && message_.tone == "error"));
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
    const auto offer = [this](const QString& format, const QString& codec) -> std::optional<QPair<QString, QJsonObject>> {
        for (int i = 0; i < format_->count(); ++i)
            if (format_->itemData(i).toStringList() == QStringList{format, codec}) return QPair<QString, QJsonObject>{QStringLiteral("Use ") + format_->itemText(i), QJsonObject{{"action", "format"}, {"format", format}, {"codec", codec}}};
        return std::nullopt;
    };
    if (config_.format == "mp4" && config_.codec == "h265") {
        StatusEntry codec{QStringLiteral("Use MP4 (H.264) for broader playback compatibility."), QStringLiteral("warning"), {}};
        if (const auto h264 = offer("mp4", "h264")) codec.actions.append(*h264);
        if (message_.tone != "error") entries.append(codec);
    }
    const auto end = edit_.exportEnd(options_.durationUs);
    if (config_.animation() && animationMaxUs_ > 0 && end - edit_.trimStart > animationMaxUs_) {
        const qint64 seconds = animationMaxUs_ / 1000000;
        StatusEntry trim{QStringLiteral("%1 is limited to %2 s and the kept part is %3. Trim it to %2 s or less before exporting.").arg(config_.format.toUpper()).arg(seconds).arg(timeText(end - edit_.trimStart)), QStringLiteral("warning"), {}};
        trim.actions.append({QStringLiteral("Keep the first %1 s").arg(seconds), QJsonObject{{"action", "trimToLimit"}}});
        if (const auto mp4 = offer("mp4", "h264")) trim.actions.append(*mp4);
        entries.append(trim);
    }
    if (config_.animation() && animationMaxEdge_ > 0 && !options_.canvas.isEmpty()) {
        const auto size = recording::outputSize(options_.canvas, config_.maxEdge);
        if (std::max(size.width(), size.height()) > animationMaxEdge_) {
            StatusEntry edge{QStringLiteral("%1 is limited to %2 px and this recording is %3 × %4 px, so the export will be refused.").arg(config_.format.toUpper()).arg(animationMaxEdge_).arg(size.width()).arg(size.height()), QStringLiteral("warning"), {}};
            if (const auto mp4 = offer("mp4", "h264")) edge.actions.append(*mp4);
            entries.append(edge);
        }
    }
    // The editor's privacy guidance, readable without hovering, whenever a blur is being placed or edited.
    const auto* mask = selectedMask();
    // Once acknowledged it stays out of the way of the frame; the tooltips keep it.
    if (!blurTipDismissed_ && (view_->armed() == "blur" || (view_->armed().isEmpty() && mask && mask->kind == "blur"))) {
        entries.append({QStringLiteral("For private information, use Redact."), QStringLiteral("warning"), {{QStringLiteral("Use Redact"), {{"action", "useSolidCover"}}}, {QStringLiteral("Got it"), {{"action", "dismissBlurTip"}}}}});
    }
    return entries;
}
void ReviewWindow::refreshStatus() {
    footerError_ = message_.tone == "error";
    errorDetails_->setVisible(footerError_);
    placeDestination();
    auto entries = statusEntries();
    for (auto& entry : entries) {
        if (entry.tone != "error" || entry.text != message_.text) continue;
        const QString raw = entry.text;
        if (raw.contains("No space left", Qt::CaseInsensitive) || (raw.contains("disk", Qt::CaseInsensitive) && (raw.contains("full", Qt::CaseInsensitive) || raw.contains("space", Qt::CaseInsensitive))))
            entry.text = QStringLiteral("Export failed: disk full. Free space, then retry.");
        else if (raw.contains("Permission denied", Qt::CaseInsensitive))
            entry.text = QStringLiteral("Export failed: permission denied. Choose another location, then retry.");
        else entry.text = raw.section('\n', 0, 0);
    }
    if (compactStatus_) {
        if (config_.format == "mp4" && config_.codec == "h265") {
            entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.text.startsWith("Use MP4 (H.264)"); }), entries.end());
            formatAdvice_->show();
        }
    }
    if (entries == shownStatus_) { updateMinimum(); return; }
    shownStatus_ = entries;
    // Later: a status action's own click may be what changed the row.
    while (auto* item = statusLines_->takeAt(0)) { if (auto* w = item->widget()) { w->hide(); w->deleteLater(); } delete item; }
    for (const auto& entry : entries) {
        auto* line = new QWidget(statusRow_);
        line->setObjectName("statusLine");
        auto* row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
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
        VisualStyle::role(text, entry.tone == "error" ? "error" : entry.tone == "warning" ? "warning" : entry.tone == "success" ? "success" : "secondary");
        // No wider than its words, so the actions follow them; narrower, it wraps.
        text->setMaximumWidth(text->fontMetrics().horizontalAdvance(entry.text) + 4);
        row->addWidget(text, 1);
        for (const auto& [label, params] : entry.actions) {
            auto* button = new QPushButton(label, line);
            button->setObjectName("statusAction");
            VisualStyle::role(button, "link");
            // Compact link painting inside a full pointer target.
            button->setMinimumSize(32, 32);
            button->setFixedHeight(std::max(32, button->fontMetrics().height() + 4));
            button->setProperty("driveAction", params);
            connect(button, &QPushButton::clicked, this, [this, params] { drive(params); });
            row->addWidget(button, 0, Qt::AlignVCenter);
        }
        row->addStretch(0);
        statusLines_->addWidget(line);
        line->show();
        row->activate();
    }
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
void ReviewWindow::showAdvice() {
    if (adviceDialog_) { auto* dialog = adviceDialog_.data(); adviceDialog_.clear(); dialog->close(); return; }
    auto* dialog = new QDialog(this);
    adviceDialog_ = dialog;
    dialog->setObjectName("reviewAdvice");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Recording details");
    auto* layout = new QVBoxLayout(dialog);
    auto* text = new QTextBrowser(dialog);
    text->setObjectName("reviewAdviceText");
    text->setPlainText((message_.text.isEmpty() ? QString() : message_.text + "\n\n") + audio_->property("summary").toString() + "\n\n" + formatHint(config_.format, config_.codec));
    text->setAccessibleName("Recording details");
    layout->addWidget(text);
    bool h264Available = false;
    for (int i = 0; i < format_->count(); ++i) h264Available |= format_->itemData(i).toStringList() == QStringList{"mp4", "h264"};
    if (config_.format == "mp4" && config_.codec == "h265" && h264Available) {
        auto* compatible = new QPushButton("Use MP4 (H.264)", dialog);
        connect(compatible, &QPushButton::clicked, this, [this, dialog] {
            drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}});
            dialog->close();
        });
        layout->addWidget(compatible, 0, Qt::AlignLeft);
    }
    if (footerError_) {
        auto* dismiss = new QPushButton("Dismiss error", dialog);
        connect(dismiss, &QPushButton::clicked, this, [this, dialog] { drive({{"action", "dismissStatus"}}); dialog->close(); });
        layout->addWidget(dismiss, 0, Qt::AlignLeft);
    }
    auto* close = new QPushButton("Close", dialog);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    layout->addWidget(close, 0, Qt::AlignRight);
    dialog->resize(QSize(640, 360).boundedTo(availableReviewSize()));
    dialog->show();
    text->setFocus();
}
void ReviewWindow::updateMinimum() {
    if (minimumPending_) return;
    minimumPending_ = true;
    // Reflow and wrapped labels settle before measuring the next minimum.
    QTimer::singleShot(0, this, [this] {
        minimumPending_ = false;
        placeTools();
        layout()->activate();
        const auto available = availableReviewSize();
        setMinimumSize(minimumSizeHint());
        if (isVisible() && !initialFitDone_) {
            const double aspect = options_.canvas.isEmpty() ? 16.0 / 9 : double(options_.canvas.width()) / options_.canvas.height();
            const int height = std::min(defaultSize().height(), available.height());
            // Fit the initial frame with room for the controls and one status row.
            // Later edits reflow inside the user's geometry instead of refitting it.
            const int width = std::min({defaultSize().width(), available.width(), int(std::round(height * aspect * 0.65))});
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
            formatAdvice_->show();
            formatAdvice_->setAccessibleName("Recording details");
            formatAdvice_->setToolTip("Preview audio and format tips; show recording details");
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
    if (!toolsGrid_) return;
    // Visibility changes invalidate nested hints on the next layout event. Measure
    // them now, before Qt could grow the window around a stale one-row layout.
    for (auto* widget : {maskTools_, trimGroup_, maskGroup_}) widget->layout()->invalidate();
    toolsGrid_->invalidate();
    const int maskWidth = maskGroup_->minimumSizeHint().width() + (maskGroup_->layout()->contentsMargins().left() ? 0 : 8);
    int trimWidth = retained_->sizeHint().width() + trimGroup_->findChild<QLabel*>("trimGroup")->sizeHint().width() + 20;
    for (auto* button : {trimStart_, trimEnd_, resetTrim_})
        trimWidth += button->fontMetrics().horizontalAdvance(button->text()) + button->iconSize().width() + 20;
    const bool stack = trimWidth + maskWidth + 24 > width();
    // The mask interval stays with its controls in both arrangements.
    maskActive_->setVisible(selectedMask() != nullptr);
    if (toolsPlaced_ && stack == toolsStacked_) return;
    toolsPlaced_ = true;
    toolsStacked_ = stack;
    static_cast<Graphite::Surface*>(maskGroup_)->setHairlines(stack ? Qt::TopEdge : Qt::LeftEdge);
    maskGroup_->layout()->setContentsMargins(stack ? 0 : 8, 0, 0, 0);
    toolsGrid_->removeWidget(maskGroup_);
    toolsGrid_->removeWidget(trimGroup_);
    transportGrid_->removeWidget(trimGroup_);
    transportGrid_->removeWidget(timeline_);
    transportRow_->removeWidget(timeline_);
    transportGrid_->removeItem(transportRow_);
    if (stack) {
        // Playback and Trim share a column beside the two-row timeline. Masks
        // remain a separate group below it, with their own interval metadata.
        transportGrid_->addLayout(transportRow_, 0, 0);
        transportGrid_->addWidget(trimGroup_, 1, 0);
        transportGrid_->addWidget(timeline_, 0, 1, 2, 1);
        transportGrid_->setHorizontalSpacing(8);
        transportGrid_->setColumnStretch(1, 1);
        toolsGrid_->addWidget(maskGroup_, 0, 0, 1, 2);
        toolsGrid_->setContentsMargins(0, 0, 0, 0);
    } else {
        transportRow_->insertWidget(transportRow_->count() - 1, timeline_, 1);
        transportGrid_->addLayout(transportRow_, 0, 0, 1, 2);
        transportGrid_->setHorizontalSpacing(0);
        transportGrid_->setColumnStretch(1, 0);
        toolsGrid_->addWidget(trimGroup_, 0, 0);
        toolsGrid_->addWidget(maskGroup_, 0, 1);
        toolsGrid_->setContentsMargins(0, 0, 0, 0);
    }
    toolsGrid_->setColumnStretch(0, stack ? 1 : 0);
    toolsGrid_->setColumnStretch(1, stack ? 0 : 1);
    updateGeometry();
}
QSize ReviewWindow::minimumSizeHint() const {
    if (!view_ || !maskGroup_ || !trimGroup_ || !outputTools_ || !layout()) return QWidget::minimumSizeHint();
    // Groups may stack: their sum must never become the window's hard minimum.
    const auto* footer = outputTools_->parentWidget();
    const int width = std::max({720, maskGroup_->minimumSizeHint().width() + 24,
        trimGroup_->minimumSizeHint().width() + 24, transportGrid_->minimumSize().width() + 24, footer && footer != this ? footer->minimumSizeHint().width() : 0});
    return {width, std::max(480, layout()->minimumSize().height())};
}
// Keep the complete destination beside the actions, or wrap it above them.
void ReviewWindow::placeDestination() {
    if (!destinationRow_) return;
    destination_->setToolTip(options_.uploadSummary);
    if (errorDetails_) errorDetails_->setText(QStringLiteral("Details…"));
    const auto margins = footerGrid_->contentsMargins();
    int room = width() - margins.left() - margins.right();
    for (auto* widget : std::initializer_list<QWidget*>{outputTools_, save_, cancel_, upload_})
        if (!widget->isHidden()) room -= widget->sizeHint().width() + footerGrid_->horizontalSpacing();
    const bool above = destination_->fontMetrics().horizontalAdvance(options_.uploadSummary) + Graphite::scaled(12) + 18 > room;
    if (above == destinationAbove_) return;
    destinationAbove_ = above;
    destinationFull_->setVisible(above);
    // In the wrapping row the same disclosure keeps its keyboard/mouse path.
    destination_->setSizePolicy(above ? QSizePolicy::Fixed : QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (above) destination_->setFixedWidth(Graphite::scaled(12) + 18);
    else { destination_->setMinimumWidth(0); destination_->setMaximumWidth(QWIDGETSIZE_MAX); }
    footerGrid_->removeWidget(destinationRow_);
    footerGrid_->addWidget(destinationRow_, above ? 0 : 2, above ? 0 : 1, 1, above ? 5 : 1);
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
    return QJsonObject{
        {"visible", isVisible()}, {"size", QJsonArray{width(), height()}}, {"minimumSize", QJsonArray{minimumSizeHint().expandedTo(minimumSize()).width(), minimumSizeHint().expandedTo(minimumSize()).height()}},
        {"scheme", Theme::name(Theme::scheme())}, {"exporting", exporting_},
        {"playheadUs", double(playhead())}, {"playing", playing_}, {"speed", speed_->currentData().toDouble()},
        {"revision", double(edit_.revision)}, {"trim", QJsonObject{{"start", double(edit_.trimStart)}, {"end", double(edit_.exportEnd(options_.durationUs))}}},
        {"masks", masks}, {"selectedMaskId", mask ? QJsonValue(mask->id) : QJsonValue(QJsonValue::Null)},
        {"armed", view_->armed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->armed())},
        {"maskControls", QJsonObject{{"list", maskTools_->isVisibleTo(this)}, {"times", maskStart_->isVisibleTo(this)}, {"strength", strength_->isVisibleTo(this)}, {"remove", removeMask_->isVisibleTo(this)}}},
        {"formatAdvice", QJsonObject{{"visible", formatAdvice_->isVisibleTo(this)}, {"drive", QJsonObject{{"action", "showAdvice"}}}}},
        {"savedCopy", QJsonObject{{"path", savedPath_}, {"revision", double(savedRevision_)}, {"current", !savedPath_.isEmpty() && savedCurrent()}, {"showInFolder", lastShowInFolder_}}},
        {"selectedMaskName", mask ? QJsonValue(maskNames_->name(*mask)) : QJsonValue(QJsonValue::Null)},
        {"selectedMaskInterval", mask ? QJsonValue(QJsonObject{{"start", double(mask->start)}, {"end", double(mask->end)}, {"text", maskActive_->text()}}) : QJsonValue(QJsonValue::Null)},
        {"errorDetails", QJsonObject{{"visible", errorDetails_->isVisibleTo(this)}, {"message", footerError_ ? QJsonValue(message_.text) : QJsonValue(QJsonValue::Null)}, {"drive", QJsonObject{{"action", "showAdvice"}}}}}, {"adviceDialog", QJsonObject{{"open", bool(adviceDialog_ && adviceDialog_->isVisible())}, {"text", adviceDialog_ && adviceDialog_->isVisible() ? QJsonValue(adviceDialog_->findChild<QTextBrowser*>("reviewAdviceText")->toPlainText()) : QJsonValue(QJsonValue::Null)}}}, {"status", status}, {"statusRow", statusRow}, {"audio", audio_->property("summary").toString()},
        {"previewMuted", true}, {"audioRequested", audioIntent_}, {"audioEffective", effectiveAudio(config_, options_.audioRecorded, audioIntent_)}, {"destination", options_.destination}, {"sharingMode", options_.sharingMode}, {"uploadSummary", options_.uploadSummary},
        {"notice", notice_->isVisibleTo(this) ? QJsonValue(notice_->text()) : QJsonValue(QJsonValue::Null)},
        {"preview", QJsonObject{{"error", view_->previewError().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->previewError())}, {"stale", view_->stale()}, {"frameTimeUs", view_->hasFrame() ? QJsonValue(double(view_->frameTime())) : QJsonValue(QJsonValue::Null)}}},
        {"frameFocus", view_->hasFocus()},
        {"discardConfirmation", QJsonObject{{"open", bool(discard_)}, {"text", discard_ ? QJsonValue(discard_->text()) : QJsonValue(QJsonValue::Null)}}},
        {"fileDialog", chooser_->state()},
        {"format", QJsonObject{{"format", config_.format}, {"codec", config_.codec}}},
        {"video", QJsonObject{{"rect", rect(video)}, {"share", double(video.width()) * video.height() / area}}},
        {"timeline", QJsonObject{{"rect", rect(timeline_->geometry())}, {"lanes", timeline_->lanes()}, {"interactionRowHeight", 32}, {"gripTargetSize", 32}}},
        {"header", QJsonObject{{"visible", header_->isVisibleTo(this)}, {"rect", rect(header_->geometry())}, {"customFrame", Graphite::WindowHeader::customFrame()}}}};
}
ReviewWindow::DriveResult ReviewWindow::drive(const QJsonObject& params) {
    const auto action = params["action"].toString();
    const auto invalid = [](const QString& message) { return DriveResult{"invalid_request", message, {}}; };
    auto done = [this](const QString& error = {}) { return error.isEmpty() ? DriveResult{{}, {}, inspect()} : DriveResult{"invalid_request", error, {}}; };
    if (action == "inspect") return done();
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
    if (action == "retryPreview") { frameTimer_.stop(); refreshFrame(); return done(); }
    if (action == "showAdvice") { showAdvice(); return done(); }
    if (action == "dismissSavedCopy") { savedDismissed_ = true; refreshLabels(); return done(); }
    if (action == "copyPath" || action == "showInFolder") {
        if (savedPath_.isEmpty()) return invalid("Save a copy first");
        if (action == "copyPath") QGuiApplication::clipboard()->setText(savedPath_);
        else {
            QPointer<ReviewWindow> guard(this);
            host::showInFolder(savedPath_, [guard](const QJsonObject& result) {
                if (!guard) return;
                guard->lastShowInFolder_ = result;
                if (!result["opened"].toBool()) guard->showError(QStringLiteral("Couldn't open the folder. Copy the path and open it in your file manager."));
            });
        }
        return done();
    }
    if (action == "dismissStatus") { message_ = {}; refreshLabels(); return done(); }
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
        if (result.failed()) { showError("Couldn't save a copy there: " + result.error); return; }
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
