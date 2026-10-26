#include "ReviewWindow.h"
#include "ClipExporter.h"
#include "DebugLog.h"
#include "Overlays.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
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
#include <utility>

namespace putgd::record {
namespace {
constexpr int handleSize = 8;
}

FrameView::FrameView(QWidget* parent) : QWidget(parent) { setMinimumSize(320, 180); setMouseTracking(true); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); }
void FrameView::setFrame(const QImage& image, qint64 timeUs) { frame_ = image; timeUs_ = timeUs; update(); }
void FrameView::setMasks(const QList<RecordingMask>& masks, const QString& selected) { masks_ = masks; selected_ = selected; update(); }
QRect FrameView::imageRect() const {
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (source.isEmpty()) return rect();
    const QSize scaled = source.scaled(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
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
void FrameView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#0d0f13"));
    const auto target = imageRect();
    if (!frame_.isNull()) painter.drawImage(target, frame_);
    for (const auto& mask : masks_) {
        const bool active = mask.start <= timeUs_ && timeUs_ < mask.end;
        const auto r = toView(mask.rect);
        QPen pen(mask.id == selected_ ? Theme::current().accent : active ? QColor(255, 255, 255, 200) : QColor(255, 255, 255, 90), mask.id == selected_ ? 2 : 1, active ? Qt::SolidLine : Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(mask.kind == "solid" ? QBrush(QColor(0, 0, 0, active ? 0 : 60)) : QBrush(QColor(155, 188, 255, active ? 0 : 30)));
        painter.drawRect(r);
        painter.setPen(Qt::white);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, mask.kind == "solid" ? QStringLiteral("solid") : QStringLiteral("blur"));
        if (mask.id == selected_) { painter.setBrush(Theme::current().accent); painter.setPen(Qt::NoPen); painter.drawRect(QRectF(r.right() - handleSize, r.bottom() - handleSize, handleSize, handleSize)); }
    }
    if (drag_ == Drag::Create) {
        // The area being dragged, clipped to the frame like the mask it becomes.
        const auto pending = QRectF(anchor_, current_).normalized().intersected(target);
        painter.setPen(QPen(Theme::current().accent, 1.5, Qt::DashLine));
        painter.setBrush(armed_ == "solid" ? QBrush(QColor(0, 0, 0, 120)) : QBrush(QColor(155, 188, 255, 70)));
        painter.drawRect(pending);
    }
    if (!armed_.isEmpty()) { painter.setPen(Qt::white); painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("Drag on the frame to place the %1 area").arg(armed_)); }
}
void FrameView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const auto p = e->position();
    if (!armed_.isEmpty()) { drag_ = Drag::Create; anchor_ = p; current_ = p; update(); return; }
    for (auto it = masks_.rbegin(); it != masks_.rend(); ++it) {
        const auto r = toView(it->rect);
        if (it->id == selected_ && QRectF(r.right() - handleSize - 2, r.bottom() - handleSize - 2, handleSize + 4, handleSize + 4).contains(p)) { drag_ = Drag::Resize; anchor_ = p; original_ = it->rect; return; }
        if (r.contains(p)) { selected_ = it->id; emit maskSelected(it->id); drag_ = Drag::Move; anchor_ = p; original_ = it->rect; update(); return; }
    }
    selected_.clear();
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
constexpr int trackPad = 8, trackTop = 5, trackHeight = 10, laneTop = 20, laneHeight = 5, laneGap = 2, bottomPad = 5, maxLanes = 4, grip = 6;
constexpr qint64 minimumSpanUs = 10000;
}

ReviewTimeline::ReviewTimeline(QWidget* parent) : QWidget(parent) {
    setObjectName("timeline");
    setAccessibleName("Timeline");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}
void ReviewTimeline::setDuration(qint64 us) { duration_ = std::max<qint64>(1, us); trimEnd_ = std::min(trimEnd_, duration_); update(); }
void ReviewTimeline::setTrim(qint64 start, qint64 end) { trimStart_ = start; trimEnd_ = end; update(); }
void ReviewTimeline::setMasks(const QList<RecordingMask>& masks, const QString& selected) {
    masks_ = masks;
    selected_ = selected;
    const int before = lanes_;
    layoutLanes();
    if (lanes_ != before) updateGeometry();
    update();
}
void ReviewTimeline::setPlayhead(qint64 us) { playhead_ = std::clamp<qint64>(us, 0, duration_ - 1); update(); }
// Masks that overlap in time go to separate lanes; past the last lane they share it.
void ReviewTimeline::layoutLanes() {
    lane_ = QList<int>(masks_.size(), 0);
    QList<int> order(masks_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return masks_[a].start < masks_[b].start; });
    QList<qint64> ends;
    for (const int i : order) {
        int lane = -1;
        for (int l = 0; l < ends.size(); ++l) if (ends[l] <= masks_[i].start) { lane = l; break; }
        if (lane < 0 && ends.size() < maxLanes) { ends.append(0); lane = int(ends.size()) - 1; }
        if (lane < 0) lane = int(std::min_element(ends.begin(), ends.end()) - ends.begin());
        ends[lane] = std::max(ends[lane], masks_[i].end);
        lane_[i] = lane;
    }
    lanes_ = int(ends.size());
}
QRect ReviewTimeline::trackRect() const { return QRect(trackPad, trackTop, std::max(1, width() - 2 * trackPad), trackHeight); }
QRect ReviewTimeline::laneRect(int lane) const { const auto track = trackRect(); return QRect(track.left(), laneTop + lane * (laneHeight + laneGap), track.width(), laneHeight); }
int ReviewTimeline::xFor(qint64 us) const { const auto track = trackRect(); return track.left() + int(double(track.width()) * std::clamp<qint64>(us, 0, duration_) / duration_); }
qint64 ReviewTimeline::timeAt(qreal x) const { const auto track = trackRect(); return std::clamp<qint64>(qint64((x - track.left()) * double(duration_) / track.width()), 0, duration_); }
QRect ReviewTimeline::maskRect(const QString& id) const {
    for (int i = 0; i < masks_.size(); ++i) {
        if (masks_[i].id != id) continue;
        const auto lane = laneRect(lane_.value(i));
        const int left = xFor(masks_[i].start);
        return QRect(left, lane.top(), std::max(3, xFor(masks_[i].end) - left), lane.height());
    }
    return {};
}
QSize ReviewTimeline::sizeHint() const { return {480, minimumSizeHint().height()}; }
QSize ReviewTimeline::minimumSizeHint() const { return {120, std::max(28, lanes_ ? laneTop + lanes_ * (laneHeight + laneGap) - laneGap + bottomPad : trackTop + trackHeight + bottomPad)}; }
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
        const auto& mask = masks_[i];
        const bool selected = mask.id == selected_;
        const bool retiming = selected && (drag_ == Drag::MaskStart || drag_ == Drag::MaskEnd);
        const auto lane = laneRect(lane_.value(i));
        const int left = xFor(retiming ? dragStart_ : mask.start), right = xFor(retiming ? dragEnd_ : mask.end);
        const QRectF bar = QRectF(left, lane.top(), std::max(3, right - left), lane.height()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(selected ? QPen(p.accent, 1.5) : QPen(p.border, 1));
        painter.setBrush(selected ? p.warningSurface : p.selected);
        painter.drawRoundedRect(bar, 3, 3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(selected ? p.accent : p.textMuted);
        painter.drawRoundedRect(QRectF(bar.left(), bar.top(), 2.5, bar.height()), 1, 1);
        if (bar.width() > 40 && lane.height() >= 12) {
            auto font = Graphite::font(Graphite::Type::micro);
            if (font.pixelSize() > lane.height() - 2) font.setPixelSize(std::max(8, lane.height() - 2));
            Graphite::paintText(painter, bar.adjusted(7, 0, -4, 0), mask.kind == "solid" ? QStringLiteral("Solid cover") : QStringLiteral("Blur"), selected ? p.text : p.textSecondary, font);
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
    if (hasFocus()) { painter.setPen(QPen(p.accent, 1)); painter.setBrush(Qt::NoBrush); painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5); }
}
void ReviewTimeline::seek(qint64 us) { setPlayhead(us); emit scrubbed(playhead_); }
void ReviewTimeline::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const auto pos = e->position();
    // The selected mask's edges, then the trim handles, then a mask bar; anything
    // else scrubs.
    if (const auto bar = maskRect(selected_); !bar.isNull() && pos.y() >= bar.top() - 3 && pos.y() <= bar.bottom() + 3) {
        const auto& mask = *std::find_if(masks_.cbegin(), masks_.cend(), [this](const RecordingMask& m) { return m.id == selected_; });
        const qreal toStart = std::abs(pos.x() - bar.left()), toEnd = std::abs(pos.x() - (bar.left() + bar.width()));
        if (std::min(toStart, toEnd) <= grip) { drag_ = toStart <= toEnd ? Drag::MaskStart : Drag::MaskEnd; dragStart_ = mask.start; dragEnd_ = mask.end; emit scrubStarted(); return; }
    }
    if (pos.y() < laneTop - 1) {
        const qreal toStart = std::abs(pos.x() - xFor(trimStart_)), toEnd = std::abs(pos.x() - xFor(trimEnd_));
        if (std::min(toStart, toEnd) <= grip) { drag_ = toStart < toEnd ? Drag::TrimStart : Drag::TrimEnd; dragStart_ = trimStart_; dragEnd_ = trimEnd_; emit scrubStarted(); return; }
    } else {
        for (int i = int(masks_.size()) - 1; i >= 0; --i) {
            if (!maskRect(masks_[i].id).adjusted(-2, -2, 2, 2).contains(pos.toPoint())) continue;
            selected_ = masks_[i].id;
            update();
            emit maskSelected(selected_);
            return;
        }
    }
    drag_ = Drag::Scrub;
    emit scrubStarted();
    seek(timeAt(pos.x()));
}
void ReviewTimeline::mouseMoveEvent(QMouseEvent* e) {
    const auto pos = e->position();
    if (drag_ == Drag::None) {
        const auto bar = maskRect(selected_);
        const bool overMask = !bar.isNull() && pos.y() >= bar.top() - 3 && pos.y() <= bar.bottom() + 3 && std::min(std::abs(pos.x() - bar.left()), std::abs(pos.x() - (bar.left() + bar.width()))) <= grip;
        const bool overTrim = pos.y() < laneTop - 1 && std::min(std::abs(pos.x() - xFor(trimStart_)), std::abs(pos.x() - xFor(trimEnd_))) <= grip;
        setCursor(overMask || overTrim ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    const qint64 at = timeAt(pos.x());
    if (drag_ == Drag::Scrub) { seek(at); return; }
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
    if (drag == Drag::MaskStart || drag == Drag::MaskEnd) {
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

ElidedLabel::ElidedLabel(QWidget* parent) : QWidget(parent) { setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); }
void ElidedLabel::setText(const QString& text) {
    if (text == text_) return;
    text_ = text;
    setToolTip(text);
    setAccessibleName(text);
    updateGeometry();
    update();
}
QSize ElidedLabel::sizeHint() const { return {fontMetrics().horizontalAdvance(text_), fontMetrics().height()}; }
QSize ElidedLabel::minimumSizeHint() const { return {0, fontMetrics().height()}; }
void ElidedLabel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(text_, Qt::ElideRight, width()));
}

ReviewWindow::ReviewWindow(const Options& options, const RecordingConfig& config, QWidget* parent) : QWidget(parent, Qt::Window), options_(options), config_(config), frameTimer_(this) {
    setObjectName("putgd-review");
    setWindowTitle("Review recording");
    // The Graphite controls (#198: in the interface's scheme).
    Fonts::load();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) VisualStyle::apply(*app);
    setAttribute(Qt::WA_DeleteOnClose, false);
    // #283: the frame takes the window; the timeline, one row of tools and the
    // action footer sit under it, under the shared document header.
    auto* frame = new QVBoxLayout(this);
    frame->setContentsMargins(0, 0, 0, 0);
    frame->setSpacing(0);
    // The document header replaces the desktop's title bar when put.gd draws
    // the frame; under a desktop frame the title bar names the window and the
    // frame keeps that height (#283's frame share).
    if (Graphite::WindowHeader::customFrame()) {
        header_ = Graphite::WindowHeader::install(this, frame);
        header_->setDocument("Review recording", "Not uploaded");
    }
    auto* root = new QVBoxLayout;
    root->setContentsMargins(12, 8, 12, 6);
    root->setSpacing(6);
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
    view_ = new FrameView(this);
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
        return button;
    };
    auto separator = [this] { auto* line = Graphite::rule(this, Qt::Vertical); line->setObjectName("separator"); line->setFixedHeight(20); return line; };
    auto* transport = new QHBoxLayout;
    transport->setSpacing(6);
    play_ = tool("play", "Play", "Play the retained part silently (Space)");
    play_->setObjectName("play");
    timeline_ = new ReviewTimeline(this);
    timeline_->setDuration(options.durationUs);
    timeline_->setToolTip("Click or drag to scrub; drag the trim handles or a selected mask's ends to retime them. Arrows step, Home/End jump to the kept part.");
    time_ = new QLabel(this);
    time_->setObjectName("time");
    VisualStyle::role(time_, "secondary");
    time_->setAccessibleName("Playhead time");
    time_->setMinimumWidth(time_->fontMetrics().horizontalAdvance(QStringLiteral("00:00.00 / 00:00.00 (trimmed)")));
    transport->addWidget(play_);
    transport->addWidget(timeline_, 1);
    transport->addWidget(time_);
    root->addLayout(transport);
    auto* tools = new QHBoxLayout;
    tools->setSpacing(4);
    speed_ = new QComboBox(this);
    speed_->setObjectName("speed");
    speed_->setToolTip("Playback speed of the preview");
    speed_->setAccessibleName("Playback speed");
    for (const auto& [label, factor] : std::initializer_list<std::pair<const char*, double>>{{"0.5×", 0.5}, {"1×", 1.0}, {"2×", 2.0}, {"4×", 4.0}}) speed_->addItem(QString::fromUtf8(label), factor);
    speed_->setCurrentIndex(1);
    trimStart_ = tool("trim-in", "Set trim start", "Keep from the playhead: set the trim start here (I)");
    trimEnd_ = tool("trim-out", "Set trim end", "Keep up to the playhead: set the trim end here (O)");
    resetTrim_ = tool("trim-reset", "Reset trim", "Keep the whole recording");
    retained_ = new QLabel(this);
    retained_->setObjectName("muted");
    VisualStyle::role(retained_, "meta");
    retained_->setAccessibleName("Kept interval");
    addBlur_ = tool("blur", "Add blur", "Add a blur, then drag on the frame to place it");
    addSolid_ = tool("redact", "Add solid cover", "Add a solid cover, then drag on the frame to place it");
    for (auto* button : {addBlur_, addSolid_}) button->setCheckable(true);
    // The selected mask's settings; the whole group shows only once masks exist.
    maskTools_ = new QWidget(this);
    maskTools_->setObjectName("maskTools");
    auto* maskRow = new QHBoxLayout(maskTools_);
    maskRow->setContentsMargins(0, 0, 0, 0);
    maskRow->setSpacing(4);
    masks_ = new QComboBox(maskTools_);
    masks_->setObjectName("masks");
    masks_->setAccessibleName("Masks");
    masks_->setToolTip("The mask to edit; click one on the frame or the timeline to select it");
    masks_->setPlaceholderText("Select a mask");
    masks_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    maskStart_ = tool("mask-in", "Set mask start", "Start the selected mask at the playhead (Shift+I)");
    maskEnd_ = tool("mask-out", "Set mask end", "End the selected mask at the playhead (Shift+O)");
    strengthLabel_ = new QLabel("Blur", maskTools_);
    strengthLabel_->setObjectName("muted");
    VisualStyle::role(strengthLabel_, "meta");
    strength_ = new QSpinBox(maskTools_);
    strength_->setRange(2, 64);
    strength_->setValue(16);
    strength_->setAccessibleName("Blur strength");
    strength_->setToolTip("Blur strength of the selected blur");
    strengthLabel_->setBuddy(strength_);
    removeMask_ = tool("remove", "Remove mask", "Remove the selected mask (Delete)");
    for (auto* w : std::initializer_list<QWidget*>{masks_, maskStart_, maskEnd_, strengthLabel_, strength_, removeMask_}) maskRow->addWidget(w);
    for (auto* w : std::initializer_list<QWidget*>{speed_, separator(), trimStart_, trimEnd_, resetTrim_, retained_, separator(), addBlur_, addSolid_, maskTools_}) tools->addWidget(w);
    tools->addStretch(1);
    root->addLayout(tools);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(6);
    format_ = new QComboBox(this);
    format_->setAccessibleName("Output format");
    for (const auto& format : options.formats) {
        const auto codecs = options.codecs[format].toArray();
        if (codecs.isEmpty()) format_->addItem(formatLabel(format, {}), QStringList{format, QString()});
        for (const auto& codec : codecs) { format_->addItem(formatLabel(format, codec.toString()), QStringList{format, codec.toString()}); format_->setItemData(format_->count() - 1, formatHint(format, codec.toString()), Qt::ToolTipRole); }
    }
    status_ = new ElidedLabel(this);
    status_->setObjectName("status");
    VisualStyle::role(status_, "warning");
    upload_ = new QPushButton("Upload", this);
    upload_->setObjectName("upload");
    upload_->setIcon(VisualStyle::icon("upload"));
    VisualStyle::role(upload_, "primary");
    upload_->setToolTip("Upload the reviewed clip with its trim and masks (the preview is silent; video exports keep the recorded system audio)");
    save_ = new QPushButton("Save copy…", this);
    save_->setIcon(VisualStyle::icon("save"));
    save_->setToolTip("Export the reviewed clip to a file on this computer. Nothing is uploaded.");
    cancel_ = new QPushButton("Discard", this);
    VisualStyle::role(cancel_, "quiet");
    auto* output = new QLabel("Output", this);
    VisualStyle::role(output, "meta");
    output->setBuddy(format_);
    actions->setContentsMargins(12, 6, 12, 6);
    actions->addWidget(output);
    actions->addWidget(format_);
    actions->addWidget(status_, 1);
    actions->addWidget(save_);
    actions->addWidget(cancel_);
    actions->addWidget(upload_);
    auto* footer = new Graphite::Surface("bg", Qt::TopEdge, this);
    footer->setObjectName("reviewFooter");
    footer->setLayout(actions);
    frame->addWidget(footer);
    // Keyboard (#283): Space plays and pauses, I/O set the trim, Shift+I/O the
    // selected mask's times, Delete removes it, Escape drops an unplaced mask.
    auto shortcut = [this](const QKeySequence& key, auto slot) { auto* s = new QShortcut(key, this); s->setContext(Qt::WindowShortcut); connect(s, &QShortcut::activated, this, slot); return s; };
    shortcut(Qt::Key_Space, [this] { if (playing_) stopPlayback(); else startPlayback(); });
    shortcut(Qt::Key_I, [this] { stopPlayback(); setTrimAt(true); });
    shortcut(Qt::Key_O, [this] { stopPlayback(); setTrimAt(false); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_I), [this] { stopPlayback(); setMaskAt(true); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_O), [this] { stopPlayback(); setMaskAt(false); });
    shortcut(Qt::Key_Delete, [this] { stopPlayback(); removeSelected(); });
    escape_ = shortcut(Qt::Key_Escape, [this] { disarm(); refreshLabels(); });
    escape_->setEnabled(false);
    frameTimer_.setSingleShot(true);
    frameTimer_.setInterval(qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") > 0 ? qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") : 60);
    connect(&frameTimer_, &QTimer::timeout, this, &ReviewWindow::refreshFrame);
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
        if (config_.silent()) config_.audio.system = false;
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
    connect(cancel_, &QPushButton::clicked, this, [this] { closing_ = true; emit cancelRequested(); });
    setConfig(config);
    setEdit({});
    const QList<QWidget*> order{play_, timeline_, speed_, trimStart_, trimEnd_, resetTrim_, addBlur_, addSolid_, masks_, maskStart_, maskEnd_, strength_, removeMask_, format_, save_, cancel_, upload_};
    for (int i = 1; i < order.size(); ++i) setTabOrder(order[i - 1], order[i]);
    timeline_->setFocus();
    // The default fits a 1366×768 desktop; a smaller screen gets what it has.
    QSize size = defaultSize();
    if (const auto* screen = QGuiApplication::primaryScreen()) size = size.boundedTo(screen->availableGeometry().size() - QSize(24, 48));
    resize(size.expandedTo(minimumSizeHint()));
}
RecordingMask* ReviewWindow::selectedMask() {
    auto it = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == selectedMask_; });
    return it == edit_.masks.end() ? nullptr : &*it;
}
const RecordingMask* ReviewWindow::selectedMask() const { return const_cast<ReviewWindow*>(this)->selectedMask(); }
QString ReviewWindow::timeText(qint64 us) const { const qint64 h = us / 10000; return QStringLiteral("%1:%2.%3").arg(h / 6000).arg((h / 100) % 60, 2, 10, QChar('0')).arg(h % 100, 2, 10, QChar('0')); }
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
    escape_->setEnabled(true);
    status_->setText(kind == "blur" ? QStringLiteral("Drag on the frame to place the blur") : QStringLiteral("Drag on the frame to place the cover"));
}
void ReviewWindow::disarm() {
    view_->disarm();
    addBlur_->setChecked(false);
    addSolid_->setChecked(false);
    escape_->setEnabled(false);
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
void ReviewWindow::setEdit(const RecordingEdit& edit) {
    // A mask the user just added becomes the selection; a removed one leaves none.
    QStringList before;
    for (const auto& mask : edit_.masks) before.append(mask.id);
    edit_ = edit;
    for (const auto& mask : edit_.masks) if (!before.contains(mask.id)) selectedMask_ = mask.id;
    if (!selectedMask()) selectedMask_.clear();
    refreshMaskList();
    view_->setMasks(edit_.masks, selectedMask_);
    refreshTimeline();
    refreshLabels();
    frameTimer_.start();
}
void ReviewWindow::setConfig(const RecordingConfig& config) {
    config_ = config;
    for (int i = 0; i < format_->count(); ++i) { const auto value = format_->itemData(i).toStringList(); if (value.size() == 2 && value[0] == config.format && value[1] == config.codec) { format_->blockSignals(true); format_->setCurrentIndex(i); format_->blockSignals(false); } }
    refreshLabels();
}
void ReviewWindow::setExporting(bool exporting, const QString& label) {
    stopPlayback();
    if (exporting) { disarm(); chooser_->cancel(); }
    exporting_ = exporting;
    for (auto* w : std::initializer_list<QWidget*>{upload_, save_, cancel_, trimStart_, trimEnd_, resetTrim_, addBlur_, addSolid_, removeMask_, maskStart_, maskEnd_, strength_, masks_, format_, play_, speed_, timeline_}) w->setEnabled(!exporting);
    status_->setText(label);
}
void ReviewWindow::showError(const QString& message) { status_->setText(message); }
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
        masks_->addItem(QStringLiteral("%1 %2").arg(m.kind == "solid" ? "Cover" : "Blur").arg(i + 1));
        masks_->setItemData(i, QStringLiteral("%1 %2×%3 at %4,%5 · %6 – %7").arg(m.kind == "solid" ? "Solid cover" : "Blur").arg(m.rect.width()).arg(m.rect.height()).arg(m.rect.x()).arg(m.rect.y()).arg(timeText(m.start), timeText(m.end)), Qt::ToolTipRole);
        if (m.id == selectedMask_) row = i;
    }
    masks_->setCurrentIndex(row);
    masks_->blockSignals(false);
}
void ReviewWindow::refreshTimeline() {
    timeline_->setTrim(edit_.trimStart, edit_.exportEnd(options_.durationUs));
    timeline_->setMasks(edit_.masks, selectedMask_);
}
void ReviewWindow::refreshLabels() {
    const qint64 at = playhead();
    const auto end = edit_.exportEnd(options_.durationUs);
    const bool retained = at >= edit_.trimStart && at < end;
    time_->setText(QStringLiteral("%1 / %2").arg(timeText(at), timeText(options_.durationUs)) + (retained ? QString() : QStringLiteral(" (trimmed)")));
    retained_->setText(QStringLiteral("%1 – %2").arg(timeText(edit_.trimStart), timeText(end)));
    retained_->setToolTip(QStringLiteral("Keeping %1 – %2 (%3 of %4)").arg(timeText(edit_.trimStart), timeText(end), timeText(end - edit_.trimStart), timeText(options_.durationUs)));
    timeline_->setAccessibleDescription(QStringLiteral("Playhead %1; keeping %2 – %3; %4 masks").arg(timeText(at), timeText(edit_.trimStart), timeText(end)).arg(edit_.masks.size()));
    // Mask settings are contextual (#283): the list once masks exist, the
    // times and Remove for a selected mask, the strength for a selected blur.
    const auto* mask = selectedMask();
    maskTools_->setVisible(!edit_.masks.isEmpty());
    for (auto* w : std::initializer_list<QWidget*>{maskStart_, maskEnd_, removeMask_}) w->setVisible(mask != nullptr);
    strengthLabel_->setVisible(mask && mask->kind == "blur");
    strength_->setVisible(mask && mask->kind == "blur");
    if (mask) { strength_->blockSignals(true); strength_->setValue(mask->strength); strength_->blockSignals(false); }
    if (!exporting_ && view_->armed().isEmpty()) {
        auto status = (config_.silent() ? QStringLiteral("%1 is silent; the export has no audio").arg(config_.format.toUpper()) : options_.audioRecorded ? QStringLiteral("System audio recorded") : QStringLiteral("No audio")) + (config_.format == "mp4" && config_.codec == "h265" ? QStringLiteral(" · H.265 does not play in Firefox or Chrome on Linux") : QString{});
        if (config_.animation() && animationMaxUs_ > 0 && end - edit_.trimStart > animationMaxUs_) status += QStringLiteral(" · %1 is limited to %2 s: trim the clip").arg(config_.format.toUpper()).arg(animationMaxUs_ / 1000000);
        if (config_.animation() && animationMaxEdge_ > 0 && !options_.canvas.isEmpty()) { const auto size = recording::outputSize(options_.canvas, config_.maxEdge); if (std::max(size.width(), size.height()) > animationMaxEdge_) status += QStringLiteral(" · larger than %1 px; the export will be refused").arg(animationMaxEdge_); }
        if (playing_) status = QStringLiteral("Playing the retained part (silent preview; the export keeps the audio)");
        status_->setText(status);
    }
}
QJsonObject ReviewWindow::inspect() const {
    auto rect = [](const QRect& r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
    const auto* mask = selectedMask();
    const QRect video(view_->mapTo(this, view_->imageRect().topLeft()), view_->imageRect().size());
    const double area = double(std::max(1, width())) * std::max(1, height());
    QJsonArray masks;
    for (const auto& m : edit_.masks) masks.append(QJsonObject{{"id", m.id}, {"kind", m.kind}, {"timeline", rect(timeline_->maskRect(m.id).translated(timeline_->mapTo(this, QPoint())))}});
    return QJsonObject{
        {"visible", isVisible()}, {"size", QJsonArray{width(), height()}}, {"minimumSize", QJsonArray{minimumSizeHint().width(), minimumSizeHint().height()}},
        {"scheme", Theme::name(Theme::scheme())}, {"exporting", exporting_},
        {"playheadUs", double(playhead())}, {"playing", playing_}, {"speed", speed_->currentData().toDouble()},
        {"revision", double(edit_.revision)}, {"trim", QJsonObject{{"start", double(edit_.trimStart)}, {"end", double(edit_.exportEnd(options_.durationUs))}}},
        {"masks", masks}, {"selectedMaskId", mask ? QJsonValue(mask->id) : QJsonValue(QJsonValue::Null)},
        {"armed", view_->armed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->armed())},
        {"maskControls", QJsonObject{{"list", maskTools_->isVisibleTo(this)}, {"times", maskStart_->isVisibleTo(this)}, {"strength", strength_->isVisibleTo(this)}, {"remove", removeMask_->isVisibleTo(this)}}},
        {"status", status_->text()}, {"notice", notice_->isVisibleTo(this) ? QJsonValue(notice_->text()) : QJsonValue(QJsonValue::Null)},
        {"fileDialog", chooser_->state()},
        {"format", QJsonObject{{"format", config_.format}, {"codec", config_.codec}}},
        {"video", QJsonObject{{"rect", rect(video)}, {"share", double(video.width()) * video.height() / area}}},
        {"timeline", QJsonObject{{"rect", rect(timeline_->geometry())}, {"lanes", timeline_->lanes()}}}};
}
ReviewWindow::DriveResult ReviewWindow::drive(const QJsonObject& params) {
    const auto action = params["action"].toString();
    const auto invalid = [](const QString& message) { return DriveResult{"invalid_request", message, {}}; };
    auto done = [this](const QString& error = {}) { return error.isEmpty() ? DriveResult{{}, {}, inspect()} : DriveResult{"invalid_request", error, {}}; };
    if (action == "inspect") return done();
    // #259: the Save copy… chooser, as the button opens it; the copy is saved
    // when someone picks a file in it (recording.saveCopy saves to a path).
    if (action == "closeFileDialog") { chooser_->cancel(); return done(); }
    if (exporting_) return {"recording_state", "The review controls are disabled while an export runs", {}};
    if (!isVisible()) return {"recording_state", "The review window is closed", {}};
    if (chooser_->isOpen()) return {"recording_state", "The Save copy dialog is open; close it first (closeFileDialog)", {}};
    if (action == "saveCopyDialog") { chooseCopy(); return done(); }
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
        if ((kind != "blur" && kind != "solid") || r.size() != 4) return invalid("addMask takes kind blur|solid and rect [x, y, width, height] in canvas pixels");
        const QRect rect(r[0].toInt(-1), r[1].toInt(-1), r[2].toInt(-1), r[3].toInt(-1));
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
    if (auto e = previewFrame(options_.workingClip, edit_, config_, at, 1280, options_.canvas, image, &actual)) { debugLog("review: preview failed " + e.message); return; }
    view_->setFrame(image, at);
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
                    guard->view_->setFrame(image, frameUs);
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
    return chooser_->open(options, [this, extension](const QString& path) {
        if (path.isEmpty() || exporting_) return;
        emit saveCopyRequested(path.endsWith("." + extension, Qt::CaseInsensitive) ? path : path + "." + extension, edit_.revision);
    });
}
void ReviewWindow::hideEvent(QHideEvent* e) {
    // Upload, cancel and a stop elsewhere hide the review: its chooser goes too
    // (minimizing is a spontaneous hide and leaves it open).
    if (!e->spontaneous()) chooser_->cancel();
    QWidget::hideEvent(e);
}
void ReviewWindow::closeEvent(QCloseEvent* e) {
    // Closing the window is a discard; nothing was uploaded.
    if (!closing_ && !exporting_) { closing_ = true; emit cancelRequested(); }
    e->accept();
}
}
