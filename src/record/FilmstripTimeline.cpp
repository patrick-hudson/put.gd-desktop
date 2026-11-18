#include "FilmstripTimeline.h"
#include "Duration.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QResizeEvent>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace putgd::record {
using Theme::Palette;
namespace {
// Geometry that does not follow the font (mock/review4.html, paintTimeline).
constexpr int kMargin = 20;        // the strip's inset on both sides
constexpr int kTop = 2;            // above the ruler
constexpr int kStripHeight = 72;   // the filmstrip's tiles
constexpr int kRulerGap = 4;       // ruler to strip
constexpr int kLanesGap = 8;       // strip to the covers track
constexpr int kBottom = 4;
constexpr int kBracketWidth = 13;  // a trim bracket's body, outside the kept range
constexpr int kBracketArm = 16;    // its arms along the strip's top and bottom
constexpr int kEdgeHit = 7;        // half the pointer target of a selected edit's edge
constexpr int kTilesAtFit = 12;    // across the default strip width
constexpr int kDefaultStrip = 1120;
constexpr int kMaxTiles = 24;
constexpr int kSkimRestMs = 120;
constexpr int kZoomLevels[] = {1, 2, 4, 8, 16};

// Everything that follows the text size comes from QFontMetrics, with the
// mock's pixel sizes (ruler 22, lane 13, at normal text) as the floor.
struct Metrics {
    QFont ruler, caption, bar, chip, flag, hint;
    int rulerHeight = 0, laneHeight = 0, chipHeight = 0, flagHeight = 0;
    int stripY = 0, lanesTop = 0, lanesHeight = 0, height = 0;
};
const Metrics& metrics() {
    static Metrics cached;
    static int scaleKey = -1;
    static QString uiFamily, monoFamily;
    const int key = Ui::scaled(1000);
    if (key == scaleKey && uiFamily == Ui::uiFamily() && monoFamily == Ui::monoFamily()) return cached;
    scaleKey = key;
    uiFamily = Ui::uiFamily();
    monoFamily = Ui::monoFamily();
    Metrics l;
    l.ruler = Ui::monoFont(11);
    l.caption = Ui::font(11);
    l.bar = Ui::font(11);
    l.bar.setWeight(QFont::Medium);
    l.chip = Ui::font(12);
    l.chip.setWeight(QFont::Medium);
    l.flag = Ui::monoFont(11);
    l.hint = Ui::font(12);
    l.rulerHeight = std::max(Ui::scaled(22), QFontMetrics(l.ruler).height() + 6);
    l.laneHeight = std::max(Ui::scaled(13), QFontMetrics(l.bar).tightBoundingRect(QStringLiteral("Redaction Blur 0123456789")).height() + 4);
    l.chipHeight = std::max(Ui::scaled(24), QFontMetrics(l.chip).height() + 8);
    l.flagHeight = std::max(Ui::scaled(17), QFontMetrics(l.flag).height() + 2);
    l.stripY = kTop + l.rulerHeight + kRulerGap;
    l.lanesTop = l.stripY + kStripHeight + kLanesGap;
    l.lanesHeight = 2 * l.laneHeight + 4;
    l.height = l.lanesTop + l.lanesHeight + kBottom;
    cached = l;
    return cached;
}

QColor withAlpha(QColor c, int alpha) { c.setAlpha(alpha); return c; }
bool dark() { return Theme::scheme() == Theme::Scheme::Dark; }

// The timeline's colours, every one a Theme token (or a token at an alpha, or a
// mix of two): mock review4.html's DARK / LIGHTP tables mapped to Theme.cpp.
struct Colors {
    QColor bg, tick, tickMinor, label, dim, cutFill, hatch, cutEdge, chip, chipText, tint, accent, onAccent;
    QColor idleBar, idleBarText, lane, play, playOutline, flag, flagText, hint, separator, placeholder;
};
Colors colors() {
    const Palette& p = Theme::current();
    Colors c;
    c.bg = p.bg;
    c.tick = p.borderStrong;
    c.tickMinor = withAlpha(p.borderStrong, 110);
    c.label = p.textMuted;
    c.dim = withAlpha(p.bg, 188);
    c.cutFill = withAlpha(p.surface, 225);
    c.hatch = withAlpha(p.text, dark() ? 38 : 72);
    c.cutEdge = withAlpha(p.text, dark() ? 128 : 140);
    c.chip = withAlpha(p.bg, dark() ? 230 : 242);
    c.chipText = p.textSecondary;
    c.tint = withAlpha(p.accent, 51);
    c.accent = p.accent;
    c.onAccent = p.onAccent;
    c.idleBar = Ui::mix(p.surfaceRaised, p.text, 0.28);
    c.idleBarText = p.text;
    c.lane = withAlpha(p.text, 11);
    c.play = p.text;
    c.playOutline = dark() ? withAlpha(p.well, 190) : p.bg;
    c.flag = p.text;
    c.flagText = p.bg;
    c.hint = p.textMuted;
    c.separator = dark() ? withAlpha(p.well, 160) : withAlpha(p.text, 64);
    c.placeholder = Ui::mix(p.surface, p.text, 0.03);
    return c;
}

QString clock(qint64 us) { return durationText(us); }
QString spanText(qint64 a, qint64 b) { return QStringLiteral("%1–%2").arg(clock(a), clock(b)); }
// "6.5 s", "6 s"; a minute or more reads as a clock.
QString lengthText(qint64 us) {
    if (us >= 60000000) return clock(us);
    const qint64 tenths = (us + 50000) / 100000;
    return tenths % 10 == 0 ? QStringLiteral("%1 s").arg(tenths / 10) : QStringLiteral("%1.%2 s").arg(tenths / 10).arg(tenths % 10);
}
QString cutLabel(qint64 length) { return QStringLiteral("Cut · %1").arg(lengthText(length)); }

QString rulerText(qint64 us, qint64 stepUs, bool hours) {
    if (hours) {
        const qint64 s = us / 1000000;
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    }
    if (stepUs < 1000000) return clock(us);
    const qint64 s = us / 1000000;
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
struct Steps { double major, minor; };  // seconds
constexpr Steps kSteps[] = {{0.2, 0.05}, {0.5, 0.1}, {1, 0.25}, {2, 0.5}, {4, 1}, {10, 2}, {20, 5}, {30, 10}, {60, 15}, {120, 30}, {300, 60}, {600, 120}, {1200, 300}, {1800, 600}, {3600, 900}, {7200, 1800}, {14400, 3600}};

QPainterPath bracketPath(const QRectF& r, qreal rad, bool left) {
    QPainterPath path;
    if (left) {
        path.moveTo(r.right(), r.top());
        path.lineTo(r.left() + rad, r.top());
        path.arcTo(QRectF(r.left(), r.top(), 2 * rad, 2 * rad), 90, 90);
        path.lineTo(r.left(), r.bottom() - rad);
        path.arcTo(QRectF(r.left(), r.bottom() - 2 * rad, 2 * rad, 2 * rad), 180, 90);
        path.lineTo(r.right(), r.bottom());
    } else {
        path.moveTo(r.left(), r.top());
        path.lineTo(r.right() - rad, r.top());
        path.arcTo(QRectF(r.right() - 2 * rad, r.top(), 2 * rad, 2 * rad), 90, -90);
        path.lineTo(r.right(), r.bottom() - rad);
        path.arcTo(QRectF(r.right() - 2 * rad, r.bottom() - 2 * rad, 2 * rad, 2 * rad), 0, -90);
        path.lineTo(r.left(), r.bottom());
    }
    path.closeSubpath();
    return path;
}
// The scissors of the Cut tool (24-unit outline), drawn here because IconArt has none yet.
void paintScissors(QPainter& p, const QRectF& area, const QColor& color) {
    p.save();
    p.translate(area.topLeft());
    p.scale(area.width() / 24, area.height() / 24);
    QPen pen(color, 1.9);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(6, 18), 3, 3);
    p.drawEllipse(QPointF(6, 6), 3, 3);
    p.drawLine(QPointF(8.5, 7.5), QPointF(20, 18));
    p.drawLine(QPointF(8.5, 16.5), QPointF(20, 6));
    p.restore();
}
}

// The timeline reads as one slider (the playhead) whose children name every
// handle, cut and cover bar. The children are looked up by index at call time
// so they never go stale when the edit changes.
class TimelineAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit TimelineAccessible(FilmstripTimeline* timeline) : QAccessibleWidget(timeline, QAccessible::Slider) {}
    ~TimelineAccessible() override { for (const auto id : ids_) if (id) QAccessible::deleteAccessibleInterface(id); }
    void* interface_cast(QAccessible::InterfaceType type) override { return type == QAccessible::ValueInterface ? static_cast<QAccessibleValueInterface*>(this) : QAccessibleWidget::interface_cast(type); }
    int childCount() const override { return int(timeline()->parts().size()); }
    QAccessibleInterface* child(int index) const override;
    int indexOfChild(const QAccessibleInterface* child) const override;
    QAccessibleInterface* childAt(int x, int y) const override;
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Value) return QStringLiteral("%1 of %2").arg(clock(playhead()), clock(timeline()->duration()));
        return QAccessibleWidget::text(type);
    }
    QVariant currentValue() const override { return double(playhead()) / 1000000; }
    QVariant minimumValue() const override { return 0.0; }
    QVariant maximumValue() const override { return double(timeline()->duration()) / 1000000; }
    QVariant minimumStepSize() const override { return 0.1; }
    void setCurrentValue(const QVariant& value) override {
        if (!timeline()->isEnabled()) return;
        const double seconds = value.toDouble();
        if (!std::isfinite(seconds)) return;
        emit timeline()->scrubStarted();
        emit timeline()->seekRequested(qRound64(std::max(0.0, seconds) * 1000000));
    }
private:
    FilmstripTimeline* timeline() const { return static_cast<FilmstripTimeline*>(object()); }
    qint64 playhead() const { return timeline()->playhead(); }
    mutable QList<QAccessible::Id> ids_;
};

namespace {
class PartAccessible final : public QAccessibleInterface {
public:
    PartAccessible(FilmstripTimeline* timeline, int index) : timeline_(timeline), index_(index) {}
    bool isValid() const override { return !timeline_.isNull(); }
    QObject* object() const override { return nullptr; }
    QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(timeline_.data()); }
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QString text(QAccessible::Text type) const override {
        const auto part = current();
        if (type == QAccessible::Name) return part.name;
        if (type == QAccessible::Description) return part.kind.endsWith(QLatin1String("-start")) || part.kind.endsWith(QLatin1String("-end")) ? QStringLiteral("Drag to move this edge") : QString();
        return {};
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override {
        if (timeline_.isNull()) return {};
        const auto part = current();
        return QRect(timeline_->mapToGlobal(part.rect.topLeft()), part.rect.size());
    }
    QAccessible::Role role() const override {
        const auto kind = current().kind;
        return kind.startsWith(QLatin1String("trim")) || kind.endsWith(QLatin1String("-start")) || kind.endsWith(QLatin1String("-end")) ? QAccessible::Grip : QAccessible::ListItem;
    }
    QAccessible::State state() const override {
        QAccessible::State s;
        const auto part = current();
        s.selectable = part.kind == QLatin1String("cut") || part.kind == QLatin1String("cover");
        if (s.selectable) {
            const auto selected = timeline_->inspect().value("selectedEdit").toObject();
            s.selected = selected.value("id").toString() == part.id && selected.value("kind").toString() == part.kind;
        }
        return s;
    }
private:
    FilmstripTimeline::Part current() const {
        if (timeline_.isNull()) return {};
        return timeline_->parts().value(index_);
    }
    QPointer<FilmstripTimeline> timeline_;
    int index_;
};
}
QAccessibleInterface* TimelineAccessible::child(int index) const {
    if (index < 0 || index >= childCount()) return nullptr;
    while (ids_.size() <= index) ids_.append(QAccessible::registerAccessibleInterface(new PartAccessible(timeline(), int(ids_.size()))));
    return QAccessible::accessibleInterface(ids_[index]);
}
int TimelineAccessible::indexOfChild(const QAccessibleInterface* child) const {
    for (int i = 0; i < ids_.size(); ++i) if (QAccessible::accessibleInterface(ids_[i]) == child) return i;
    return -1;
}
QAccessibleInterface* TimelineAccessible::childAt(int x, int y) const {
    const auto local = timeline()->mapFromGlobal(QPoint(x, y));
    const auto list = timeline()->parts();
    for (int i = list.size() - 1; i >= 0; --i) if (list[i].rect.contains(local)) return child(i);
    return nullptr;
}

namespace {
QAccessibleInterface* timelineAccessible(const QString& name, QObject* object) {
    return name == QLatin1String("putgd::record::FilmstripTimeline") ? new TimelineAccessible(static_cast<FilmstripTimeline*>(object)) : nullptr;
}
}

FilmstripTimeline::FilmstripTimeline(std::shared_ptr<MaskNames> names, QWidget* parent) : QWidget(parent), names_(names ? std::move(names) : std::make_shared<MaskNames>()) {
    static const bool installed = [] { QAccessible::installFactory(timelineAccessible); return true; }();
    Q_UNUSED(installed);
    setObjectName("timeline");
    setAccessibleName("Recording timeline");
    setAccessibleDescription("Click to move the playhead, drag across the filmstrip to select time, drag a bracket to trim");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    skimRest_.setSingleShot(true);
    skimRest_.setInterval(kSkimRestMs);
    connect(&skimRest_, &QTimer::timeout, this, [this] { if (skim_) emit skimRested(*skim_); });
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

// ---- model ----
void FilmstripTimeline::setTileSource(TileSource source) { tiles_ = std::move(source); update(); }
void FilmstripTimeline::tilesChanged() { update(); }
void FilmstripTimeline::setDuration(qint64 durationUs, int fps) {
    duration_ = std::max<qint64>(1, durationUs);
    fps_ = std::max(1, fps);
    playhead_ = std::clamp<qint64>(playhead_, 0, duration_);
    const auto before = visibleSpan();
    scrollUs_ = std::clamp<qint64>(scrollUs_, 0, std::max<qint64>(0, duration_ - visibleLength()));
    update();
    if (visibleSpan() != before) emit visibleSpanChanged();
}
void FilmstripTimeline::setEdit(const RecordingEdit& edit) {
    names_->observe(edit.masks);
    const int cuts = int(edit_.cuts.size()), covers = int(edit_.masks.size());
    // A bracket, cut edge or cover end dragged from an older edit would write
    // over the newer one on release: the drag ends here, proposing nothing.
    const bool edgeDrag = drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd || drag_ == Drag::CutStart || drag_ == Drag::CutEnd || drag_ == Drag::CoverStart || drag_ == Drag::CoverEnd;
    const bool cancelDrag = edgeDrag && edit.revision != edit_.revision;
    edit_ = edit;
    if (cancelDrag) {
        drag_ = Drag::None;
        dragId_.clear();
        unsetCursor();
        emit edgeDragFinished();
    }
    if (selectedKind_ == "cut" && std::none_of(edit_.cuts.cbegin(), edit_.cuts.cend(), [this](const RecordingCut& c) { return c.id == selectedId_; })) selectedKind_.clear(), selectedId_.clear();
    if (selectedKind_ == "cover" && std::none_of(edit_.masks.cbegin(), edit_.masks.cend(), [this](const RecordingMask& m) { return m.id == selectedId_; })) selectedKind_.clear(), selectedId_.clear();
    layoutCovers();
    if (cuts != edit_.cuts.size() || covers != edit_.masks.size()) {
        QAccessibleEvent reorder(this, QAccessible::ObjectReorder);
        QAccessible::updateAccessibility(&reorder);
    }
    update();
}
void FilmstripTimeline::setPlayhead(qint64 sourceUs) {
    const auto at = std::clamp<qint64>(sourceUs, 0, duration_);
    if (at == playhead_) return;
    playhead_ = at;
    QAccessibleValueChangeEvent changed(this, double(playhead_) / 1000000);
    QAccessible::updateAccessibility(&changed);
    revealPlayhead();
    update();
}
void FilmstripTimeline::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    if (playing_) clearSkim();
}
void FilmstripTimeline::setTimeSelection(std::optional<recording::Span> span) {
    selection_ = span;
    if (selection_) selectedKind_.clear(), selectedId_.clear();
    update();
}
void FilmstripTimeline::setSelectedEdit(const QString& kind, const QString& id) { selectLocally(kind, id); }
void FilmstripTimeline::selectLocally(const QString& kind, const QString& id) {
    selectedKind_ = kind;
    selectedId_ = kind.isEmpty() ? QString() : id;
    if (!kind.isEmpty()) selection_.reset();
    layoutCovers();
    update();
}
void FilmstripTimeline::skimAt(std::optional<qint64> sourceUs) {
    if (!sourceUs) { clearSkim(); return; }
    if (playing_) return;
    const qint64 at = std::clamp<qint64>(*sourceUs, 0, std::max<qint64>(0, duration_ - 1));
    if (skim_ == at) { restartSkimTimer(); return; }
    skim_ = at;
    skimExact_ = false;
    update();
    emit skimMoved(at);
    restartSkimTimer();
}
void FilmstripTimeline::setSkimExact(bool exact) { if (skimExact_ != exact) { skimExact_ = exact; update(); } }

// ---- time and frames ----
qint64 FilmstripTimeline::frameUs() const { return std::max<qint64>(1, qRound64(1e6 / fps_)); }
qint64 FilmstripTimeline::snap(qint64 us) const {
    const qint64 n = qRound64(double(std::clamp<qint64>(us, 0, duration_)) * fps_ / 1e6);
    return std::min(duration_, qRound64(double(n) * 1e6 / fps_));
}
qint64 FilmstripTimeline::visibleLength() const { return std::max<qint64>(1, duration_ / zoom_); }
recording::Span FilmstripTimeline::visibleSpan() const { return {scrollUs_, std::min(duration_, scrollUs_ + visibleLength())}; }
int FilmstripTimeline::stripLeft() const { return kMargin; }
int FilmstripTimeline::stripWidth() const { return std::max(1, width() - 2 * kMargin); }
QRect FilmstripTimeline::rulerRect() const { return QRect(stripLeft(), kTop, stripWidth(), metrics().rulerHeight); }
QRect FilmstripTimeline::stripRect() const { return QRect(stripLeft(), metrics().stripY, stripWidth(), kStripHeight); }
QRect FilmstripTimeline::coversRect() const { return QRect(stripLeft(), metrics().lanesTop, stripWidth(), metrics().lanesHeight); }
QRect FilmstripTimeline::laneRect(int lane) const { return QRect(stripLeft(), metrics().lanesTop + 2 + lane * metrics().laneHeight, stripWidth(), metrics().laneHeight - 1); }
int FilmstripTimeline::xFor(qint64 sourceUs) const {
    const double x = stripLeft() + double(sourceUs - scrollUs_) * stripWidth() / double(visibleLength());
    return int(std::lround(std::clamp(x, -1e6, 1e6)));
}
qint64 FilmstripTimeline::timeAt(qreal x) const {
    const double t = double(scrollUs_) + (x - stripLeft()) * double(visibleLength()) / stripWidth();
    return std::clamp<qint64>(qint64(std::llround(t)), 0, duration_);
}
void FilmstripTimeline::setScroll(qint64 startUs) {
    const auto clamped = std::clamp<qint64>(startUs, 0, std::max<qint64>(0, duration_ - visibleLength()));
    if (clamped == scrollUs_) return;
    scrollUs_ = clamped;
    update();
    emit visibleSpanChanged();
}
void FilmstripTimeline::revealPlayhead() {
    if (zoom_ <= 1) return;
    const auto span = visibleSpan();
    const qint64 len = visibleLength();
    // Playing follows with a 10 % lead on the left; a seek that lands off screen centres.
    if (playing_) { if (playhead_ < span.start || playhead_ > span.start + len * 95 / 100) setScroll(playhead_ - len / 10); }
    else if (playhead_ < span.start || playhead_ > span.end) setScroll(playhead_ - len / 2);
}
void FilmstripTimeline::setZoom(int level, std::optional<qint64> anchorUs) {
    int next = 1;
    for (const int candidate : kZoomLevels) if (std::abs(candidate - level) <= std::abs(next - level)) next = candidate;
    if (next == zoom_) return;
    const auto before = visibleSpan();
    const qint64 anchor = std::clamp<qint64>(anchorUs.value_or(before.start + visibleLength() / 2), 0, duration_);
    // Keep the anchor under the same x; an anchor off screen is held at the nearest edge.
    const double fraction = std::clamp(double(anchor - scrollUs_) / double(visibleLength()), 0.0, 1.0);
    zoom_ = next;
    scrollUs_ = std::clamp<qint64>(anchor - qint64(std::llround(fraction * double(visibleLength()))), 0, std::max<qint64>(0, duration_ - visibleLength()));
    update();
    emit zoomChanged(zoom_);
    emit visibleSpanChanged();
}
QList<qint64> FilmstripTimeline::tileTimes() const {
    const int count = std::clamp((stripWidth() * kTilesAtFit + kDefaultStrip - 1) / kDefaultStrip, 1, kMaxTiles);
    const auto span = visibleSpan();
    const qint64 length = span.length();
    QList<qint64> times;
    for (int i = 0; i < count; ++i) times.append(span.start + qint64(std::llround((i + 0.5) * double(length) / count)));
    if (count > 2) { QList<qint64> ordered{times.first(), times.last()}; for (int i = 1; i < count - 1; ++i) ordered.append(times[i]); return ordered; }
    if (count == 2) return {times[0], times[1]};
    return times;
}

// ---- covers ----
// Two lanes, first fit by start; the selected cover always gets a lane.
void FilmstripTimeline::layoutCovers() {
    const auto& masks = edit_.masks;
    coverLane_ = QList<int>(masks.size(), -1);
    QList<int> order(masks.size());
    for (int i = 0; i < masks.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return masks[a].start < masks[b].start; });
    const auto place = [&](int selectedFirst) {
        QList<QPair<qint64, qint64>> used[2];
        coverLane_.fill(-1);
        const auto tryPlace = [&](int i) {
            const qint64 a = masks[i].start, b = std::max(masks[i].end, a + 1);
            for (int lane = 0; lane < 2; ++lane) {
                const bool free = std::none_of(used[lane].cbegin(), used[lane].cend(), [&](const auto& u) { return a < u.second && u.first < b; });
                if (free) { used[lane].append({a, b}); coverLane_[i] = lane; return; }
            }
        };
        if (selectedFirst >= 0) tryPlace(selectedFirst);
        for (const int i : order) if (i != selectedFirst) tryPlace(i);
    };
    place(-1);
    int selected = -1;
    if (selectedKind_ == "cover") for (int i = 0; i < masks.size(); ++i) if (masks[i].id == selectedId_) selected = i;
    if (selected >= 0 && coverLane_[selected] < 0) place(selected);
    lanesUsed_ = 0;
    hiddenCovers_ = 0;
    for (const int lane : coverLane_) { if (lane < 0) ++hiddenCovers_; else lanesUsed_ = std::max(lanesUsed_, lane + 1); }
}
QRect FilmstripTimeline::coverBarRect(const QString& id) const {
    for (int i = 0; i < edit_.masks.size(); ++i) {
        if (edit_.masks[i].id != id) continue;
        if (coverLane_.value(i, -1) < 0) return {};
        const auto lane = laneRect(coverLane_[i]);
        const int left = xFor(edit_.masks[i].start);
        return QRect(left, lane.top(), std::max(3, xFor(edit_.masks[i].end) - left), lane.height());
    }
    return {};
}
QRect FilmstripTimeline::cutRect(const QString& id) const {
    for (const auto& cut : edit_.cuts) if (cut.id == id) {
        const int left = xFor(cut.start);
        return QRect(left, metrics().stripY, std::max(1, xFor(cut.end) - left), kStripHeight);
    }
    return {};
}
QRect FilmstripTimeline::trimHandleRect(bool start) const {
    const int x = xFor(start ? edit_.trimStart : edit_.exportEnd(duration_));
    return QRect(start ? x - kBracketWidth : x, metrics().stripY - 2, kBracketWidth, kStripHeight + 4);
}

// ---- size ----
QSize FilmstripTimeline::sizeHint() const { return {480, metrics().height}; }
QSize FilmstripTimeline::minimumSizeHint() const { return {240, metrics().height}; }

// ---- inspect, parts ----
QJsonObject FilmstripTimeline::inspect() const {
    const auto span = visibleSpan();
    int shown = 0, placeholders = 0;
    for (const qint64 t : tileTimes()) (tiles_ && !tiles_(t).isNull() ? shown : placeholders)++;
    QJsonValue selection = QJsonValue::Null, skim = QJsonValue::Null;
    if (selection_) selection = QJsonObject{{"start", double(selection_->start)}, {"end", double(selection_->end)}};
    if (skim_) skim = QJsonObject{{"time", double(*skim_)}, {"exact", skimExact_}};
    return {{"zoom", zoom_},
            {"visible", QJsonObject{{"start", double(span.start)}, {"end", double(span.end)}}},
            {"selection", selection},
            {"selectedEdit", QJsonObject{{"kind", selectedKind_}, {"id", selectedId_}}},
            {"skim", skim},
            {"lanes", lanesUsed_},
            {"hiddenCovers", hiddenCovers_},
            {"tiles", QJsonObject{{"shown", shown}, {"placeholders", placeholders}}}};
}
QList<FilmstripTimeline::Part> FilmstripTimeline::parts() const {
    QList<Part> list;
    const qint64 trimEnd = edit_.exportEnd(duration_);
    list.append({"trim-start", {}, QStringLiteral("Trim start, %1").arg(clock(edit_.trimStart)), trimHandleRect(true)});
    list.append({"trim-end", {}, QStringLiteral("Trim end, %1").arg(clock(trimEnd)), trimHandleRect(false)});
    for (const auto& cut : edit_.cuts) {
        list.append({"cut", cut.id, QStringLiteral("Cut, %1, %2 left out").arg(spanText(cut.start, cut.end), lengthText(cut.end - cut.start)), cutRect(cut.id)});
        if (selectedKind_ == "cut" && selectedId_ == cut.id) {
            list.append({"cut-start", cut.id, QStringLiteral("Cut start edge, %1").arg(clock(cut.start)), QRect(xFor(cut.start) - kEdgeHit, metrics().stripY, 2 * kEdgeHit, kStripHeight)});
            list.append({"cut-end", cut.id, QStringLiteral("Cut end edge, %1").arg(clock(cut.end)), QRect(xFor(cut.end) - kEdgeHit, metrics().stripY, 2 * kEdgeHit, kStripHeight)});
        }
    }
    for (const auto& mask : edit_.masks) {
        const auto bar = coverBarRect(mask.id);
        if (bar.isNull()) continue;
        list.append({"cover", mask.id, QStringLiteral("%1, %2").arg(names_->name(mask), spanText(mask.start, mask.end)), bar});
        if (selectedKind_ == "cover" && selectedId_ == mask.id) {
            list.append({"cover-start", mask.id, QStringLiteral("%1 start edge, %2").arg(names_->name(mask), clock(mask.start)), QRect(bar.left() - kEdgeHit, bar.top() - 2, 2 * kEdgeHit, bar.height() + 4)});
            list.append({"cover-end", mask.id, QStringLiteral("%1 end edge, %2").arg(names_->name(mask), clock(mask.end)), QRect(bar.right() - kEdgeHit, bar.top() - 2, 2 * kEdgeHit, bar.height() + 4)});
        }
    }
    if (hiddenCovers_ > 0) {
        QStringList hidden;
        for (int i = 0; i < edit_.masks.size(); ++i) if (coverLane_.value(i, -1) < 0) hidden << names_->name(edit_.masks[i]);
        const QFontMetrics fm(metrics().bar);
        const int w = fm.horizontalAdvance(QStringLiteral("+%1").arg(hiddenCovers_)) + 12;
        const auto lane = laneRect(1);
        list.append({"more", {}, QStringLiteral("%1 more covers not shown: %2").arg(hiddenCovers_).arg(hidden.join(QStringLiteral(", "))), QRect(lane.right() - w - 2, lane.top(), w, lane.height())});
    }
    return list;
}

// ---- painting ----
void FilmstripTimeline::paintEvent(QPaintEvent*) {
    const auto& pal = Theme::current();
    const auto c = colors();
    const auto& L = metrics();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), c.bg);
    const auto strip = stripRect();
    const auto span = visibleSpan();
    const bool trimming = drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd;
    const qint64 trimA = trimming ? dragStart_ : edit_.trimStart, trimB = trimming ? dragEnd_ : edit_.exportEnd(duration_);
    const auto liveCut = [&](const RecordingCut& cut) { return (drag_ == Drag::CutStart || drag_ == Drag::CutEnd) && dragId_ == cut.id ? recording::Span{dragStart_, dragEnd_} : recording::Span{cut.start, cut.end}; };
    const auto liveCover = [&](const RecordingMask& mask) { return (drag_ == Drag::CoverStart || drag_ == Drag::CoverEnd) && dragId_ == mask.id ? recording::Span{dragStart_, dragEnd_} : recording::Span{mask.start, mask.end}; };
    const auto xAt = [&](qint64 t) { return double(xFor(t)); };

    // The skim flag first: ruler labels under it are skipped.
    QRectF flagRect;
    QString flagText;
    if (skim_ && drag_ == Drag::None) {
        const QFontMetrics fm(L.flag);
        flagText = (skimExact_ ? QString() : QStringLiteral("≈")) + clock(*skim_);
        const double w = fm.horizontalAdvance(flagText) + 12;
        const double x = std::clamp<double>(xAt(*skim_) - w / 2, 0, std::max(0.0, width() - w));
        flagRect = QRectF(x, kTop - 2, w, L.flagHeight);
    }

    // Ruler in source time, captioned at its right end.
    {
        const QFontMetrics rulerMetrics(L.ruler), captionMetrics(L.caption);
        const bool hours = duration_ >= 3600LL * 1000000;
        const QString caption = QStringLiteral("recording time");
        const int captionWidth = captionMetrics.horizontalAdvance(caption) + 8;
        const int labelWidth = rulerMetrics.horizontalAdvance(hours ? QStringLiteral("0:00:00") : QStringLiteral("0:00.0"));
        const double pxPerSecond = double(strip.width()) * zoom_ / (double(duration_) / 1e6);
        Steps steps = kSteps[std::size(kSteps) - 1];
        for (const auto& candidate : kSteps) if (candidate.major * pxPerSecond >= labelWidth + 56) { steps = candidate; break; }
        const qint64 majorUs = std::llround(steps.major * 1e6), minorUs = std::llround(steps.minor * 1e6);
        const bool tenths = majorUs < 1000000;
        const QRectF ruler(strip.left(), kTop, strip.width(), L.rulerHeight);
        for (qint64 k = (span.start + minorUs - 1) / minorUs; k * minorUs <= span.end; ++k) {
            const qint64 t = k * minorUs;
            const bool major = t % majorUs == 0;
            const int x = xFor(t), len = major ? 10 : 5;
            p.fillRect(QRect(x, int(ruler.bottom()) - len, 1, len), major ? c.tick : c.tickMinor);
            if (!major || t >= duration_) continue;
            const QString label = tenths ? clock(t) : rulerText(t, majorUs, hours);
            const double lw = rulerMetrics.horizontalAdvance(label);
            const QRectF at(x + 4, ruler.top(), lw + 2, ruler.height() - 6);
            if (at.right() > strip.right() - captionWidth) continue;
            if (!flagRect.isNull() && at.intersects(flagRect.adjusted(-6, 0, 6, 0))) continue;
            Ui::paintText(p, at, label, c.label, L.ruler, Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideNone);
        }
        Ui::paintText(p, QRectF(ruler.left(), ruler.top(), ruler.width(), ruler.height() - 6), caption, c.label, L.caption, Qt::AlignRight | Qt::AlignVCenter, Qt::ElideNone);
    }

    // Filmstrip: tiles, then the dimmed ends, cuts, tint and selection inside one rounded clip.
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(QRectF(strip), 6, 6);
    p.setClipPath(clip);
    {
        const int count = std::clamp((strip.width() * kTilesAtFit + kDefaultStrip - 1) / kDefaultStrip, 1, kMaxTiles);
        const qint64 length = span.length();
        for (int i = 0; i < count; ++i) {
            const QRect slot(strip.left() + int(std::lround(double(i) * strip.width() / count)), strip.top(), int(std::lround(double(i + 1) * strip.width() / count)) - int(std::lround(double(i) * strip.width() / count)), strip.height());
            QImage image;
            if (tiles_) image = tiles_(span.start + qint64(std::llround((i + 0.5) * double(length) / count)));
            if (image.isNull()) { p.fillRect(slot, c.placeholder); continue; }
            // Cover the slot, cropping the picture's sides or bottom.
            QRectF source(QPointF(0, 0), QSizeF(image.size()));
            const double slotAspect = double(slot.width()) / slot.height(), imageAspect = source.width() / source.height();
            if (imageAspect > slotAspect) { const double w = source.height() * slotAspect; source = QRectF((source.width() - w) / 2, 0, w, source.height()); }
            else { const double h = source.width() / slotAspect; source = QRectF(0, (source.height() - h) / 2, source.width(), h); }
            p.drawImage(QRectF(slot), image, source);
            if (i > 0) p.fillRect(QRectF(slot.left() - 0.5, slot.top(), 1, slot.height()), c.separator);
        }
    }
    const double left = strip.left(), right = strip.right() + 1;
    p.fillRect(QRectF(left, strip.top(), std::clamp(xAt(trimA), left, right) - left, strip.height()), c.dim);
    p.fillRect(QRectF(std::clamp(xAt(trimB), left, right), strip.top(), right - std::clamp(xAt(trimB), left, right), strip.height()), c.dim);
    for (const auto& cut : edit_.cuts) {
        const auto live = liveCut(cut);
        const double a = xAt(live.start), b = xAt(live.end);
        if (b < left || a > right) continue;
        p.fillRect(QRectF(a, strip.top(), b - a, strip.height()), c.cutFill);
        p.save();
        p.setClipRect(QRectF(a, strip.top(), b - a, strip.height()), Qt::IntersectClip);
        p.setPen(QPen(c.hatch, 1.5));
        for (double x = a - kStripHeight; x < b; x += 10) p.drawLine(QPointF(x, strip.bottom() + 1), QPointF(x + kStripHeight, strip.top()));
        p.restore();
    }
    if (selectedKind_ == "cover") for (const auto& mask : edit_.masks) if (mask.id == selectedId_) {
        const auto live = liveCover(mask);
        const double a = xAt(live.start), b = xAt(live.end);
        p.fillRect(QRectF(a, strip.top(), b - a, strip.height()), c.tint);
        p.fillRect(QRectF(a, strip.bottom() + 1 - 3, b - a, 3), c.accent);
    }
    if (selectedKind_ == "trim") {
        p.setPen(QPen(c.accent, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(xAt(trimA), strip.top() + 1.5, xAt(trimB) - xAt(trimA), strip.height() - 3));
    }
    std::optional<recording::Span> shownSelection = selection_;
    if (drag_ == Drag::Select) shownSelection = recording::Span{dragStart_, dragEnd_};
    if (shownSelection && !shownSelection->empty()) {
        const QRectF box(xAt(shownSelection->start), strip.top() + 0.75, xAt(shownSelection->end) - xAt(shownSelection->start), strip.height() - 1.5);
        p.fillRect(box, withAlpha(c.accent, 30));
        QPen dashed(c.accent, 1.5, Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawRect(box);
    }
    p.restore();

    // Cut chips and edges (outside the clip so a chip can overhang a short cut's tiles).
    for (const auto& cut : edit_.cuts) {
        const auto live = liveCut(cut);
        const double a = xAt(live.start), b = xAt(live.end);
        if (b < left || a > right) continue;
        const bool selected = selectedKind_ == "cut" && selectedId_ == cut.id;
        p.setPen(Qt::NoPen);
        p.setBrush(c.cutEdge);
        p.drawRect(QRectF(a, strip.top(), 1.5, strip.height()));
        p.drawRect(QRectF(b - 1.5, strip.top(), 1.5, strip.height()));
        if (selected) {
            p.setPen(QPen(c.accent, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(a + 1, strip.top() + 1, b - a - 2, strip.height() - 2));
        }
        const double visibleA = std::max(a, left), visibleB = std::min(b, right);
        const QPointF centre((visibleA + visibleB) / 2, strip.center().y() + 0.5);
        const QFontMetrics fm(L.chip);
        const QString label = cutLabel(live.end - live.start);
        const double chipWidth = fm.horizontalAdvance(label) + 24, room = visibleB - visibleA - 8;
        p.setPen(Qt::NoPen);
        p.setBrush(c.chip);
        if (chipWidth <= room) {
            p.drawRoundedRect(QRectF(centre.x() - chipWidth / 2, centre.y() - L.chipHeight / 2.0, chipWidth, L.chipHeight), L.chipHeight / 2.0, L.chipHeight / 2.0);
            Ui::paintText(p, QRectF(centre.x() - chipWidth / 2, centre.y() - L.chipHeight / 2.0, chipWidth, L.chipHeight), label, c.chipText, L.chip, Qt::AlignCenter, Qt::ElideNone);
        } else if (room >= L.chipHeight) {
            p.drawEllipse(QRectF(centre.x() - L.chipHeight / 2.0, centre.y() - L.chipHeight / 2.0, L.chipHeight, L.chipHeight));
            const double glyph = L.chipHeight * 0.6;
            paintScissors(p, QRectF(centre.x() - glyph / 2, centre.y() - glyph / 2, glyph, glyph), c.chipText);
        }
    }

    // Trim brackets: accent bodies outside the kept range with arms along its top and bottom.
    for (const bool start : {true, false}) {
        const qint64 t = start ? trimA : trimB;
        if (t < span.start || t > span.end) continue;
        const double x = xAt(t);
        const double bx = start ? x - kBracketWidth : x;
        const QRectF body(bx, strip.top() - 2, kBracketWidth, strip.height() + 4);
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        p.drawPath(bracketPath(body, 5, start));
        p.fillRect(QRectF(start ? x : x - kBracketArm, strip.top() - 2, kBracketArm, 2), c.accent);
        p.fillRect(QRectF(start ? x : x - kBracketArm, strip.bottom() + 1, kBracketArm, 2), c.accent);
        p.fillRect(QRectF(bx + 5.5, strip.center().y() - 8.5, 2, 18), c.onAccent);
    }

    // Covers track.
    {
        const auto lanes = coversRect();
        p.setPen(Qt::NoPen);
        p.setBrush(c.lane);
        p.drawRoundedRect(QRectF(lanes).adjusted(0, 0, 0, 0), 4, 4);
        if (edit_.masks.isEmpty()) {
            Ui::paintText(p, QRectF(lanes.left() + 10, lanes.top(), lanes.width() - 20, lanes.height()), QStringLiteral("No covers yet"), c.hint, L.hint, Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            p.save();
            QPainterPath laneClip;
            laneClip.addRoundedRect(QRectF(lanes), 4, 4);
            p.setClipPath(laneClip);
            for (int i = 0; i < edit_.masks.size(); ++i) {
                if (coverLane_.value(i, -1) < 0) continue;
                const auto& mask = edit_.masks[i];
                const bool selected = selectedKind_ == "cover" && selectedId_ == mask.id;
                const auto live = liveCover(mask);
                const auto lane = laneRect(coverLane_[i]);
                const double a = xAt(live.start), b = std::max(xAt(live.end), a + 3);
                if (b < lanes.left() || a > lanes.right()) continue;
                const QRectF bar(a, lane.top(), b - a, lane.height());
                p.setPen(Qt::NoPen);
                p.setBrush(selected ? c.accent : c.idleBar);
                p.drawRoundedRect(bar, 3, 3);
                const QColor ink = selected ? c.onAccent : c.idleBarText;
                const QRectF text(std::max(bar.left(), double(lanes.left())) + 8, bar.top(), std::min(bar.right(), double(lanes.right() + 1)) - std::max(bar.left(), double(lanes.left())) - 16, bar.height());
                if (text.width() > 12) Ui::paintText(p, text, names_->name(mask), ink, L.bar, Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideRight);
                if (selected) { p.setBrush(ink); p.drawRect(QRectF(bar.left() + 2, bar.top() + 3, 2, bar.height() - 6)); p.drawRect(QRectF(bar.right() - 4, bar.top() + 3, 2, bar.height() - 6)); }
            }
            p.restore();
            if (hiddenCovers_ > 0) {
                const QFontMetrics fm(L.bar);
                const QString more = QStringLiteral("+%1").arg(hiddenCovers_);
                const auto lane = laneRect(1);
                const QRectF chip(lane.right() - fm.horizontalAdvance(more) - 12 - 2, lane.top(), fm.horizontalAdvance(more) + 12, lane.height());
                p.setPen(Qt::NoPen);
                p.setBrush(pal.surfaceRaised);
                p.drawRoundedRect(chip, 3, 3);
                Ui::paintText(p, chip, more, pal.textSecondary, L.bar, Qt::AlignCenter, Qt::ElideNone);
            }
        }
    }

    // Grips of a selected cut, then the playhead above them.
    if (selectedKind_ == "cut") for (const auto& cut : edit_.cuts) if (cut.id == selectedId_) {
        const auto live = liveCut(cut);
        for (const qint64 t : {live.start, live.end}) {
            if (t < span.start || t > span.end) continue;
            const double x = xAt(t), y = strip.center().y();
            p.setPen(Qt::NoPen);
            p.setBrush(c.playOutline);
            p.drawRoundedRect(QRectF(x - 5, y - 13, 10, 26), 3, 3);
            p.setBrush(c.accent);
            p.drawRoundedRect(QRectF(x - 4, y - 12, 8, 24), 3, 3);
        }
    }
    if (playhead_ >= span.start && playhead_ <= span.end) {
        const double x = xAt(playhead_);
        const double end = strip.bottom() + 1 + 4;
        p.setPen(Qt::NoPen);
        p.setBrush(c.playOutline);
        p.drawRect(QRectF(x - 2, kTop + 4, 4, end - kTop - 4));
        p.setBrush(c.play);
        p.drawRect(QRectF(x - 1, kTop + 4, 2, end - kTop - 4));
        const QPolygonF cap{QPointF(x - 7, kTop), QPointF(x + 7, kTop), QPointF(x, kTop + 8)};
        p.setPen(QPen(c.playOutline, 1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawPolygon(cap);
    }

    // The skim line and its own time flag.
    if (!flagRect.isNull()) {
        const double x = xAt(*skim_);
        p.setOpacity(0.7);
        p.fillRect(QRectF(x - 0.5, strip.top() - 6, 1, strip.height() + 12), c.play);
        p.setOpacity(1);
        p.setPen(Qt::NoPen);
        p.setBrush(c.flag);
        p.drawRoundedRect(flagRect, 4, 4);
        Ui::paintText(p, flagRect, flagText, c.flagText, L.flag, Qt::AlignCenter, Qt::ElideNone);
    }
    if (Ui::keyboardFocus(this)) {
        p.setPen(QPen(pal.accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    }
}

// ---- pointer ----
FilmstripTimeline::Handle FilmstripTimeline::handleAt(QPointF pos) const {
    Handle best;
    double nearest = 1e9;
    const auto offer = [&](double centre, Drag kind, const QString& id) {
        const double distance = std::abs(pos.x() - centre);
        if (distance < nearest) { nearest = distance; best = {kind, id}; }
    };
    const auto strip = stripRect();
    if (pos.y() >= strip.top() - 3 && pos.y() <= strip.bottom() + 3) {
        const auto span = visibleSpan();
        const qint64 trimEnd = edit_.exportEnd(duration_);
        if (edit_.trimStart >= span.start && edit_.trimStart <= span.end) { const auto r = trimHandleRect(true).adjusted(-4, 0, 4, 0); if (r.contains(pos.toPoint())) offer(trimHandleRect(true).center().x(), Drag::TrimStart, {}); }
        if (trimEnd >= span.start && trimEnd <= span.end) { const auto r = trimHandleRect(false).adjusted(-4, 0, 4, 0); if (r.contains(pos.toPoint())) offer(trimHandleRect(false).center().x(), Drag::TrimEnd, {}); }
        if (selectedKind_ == "cut") for (const auto& cut : edit_.cuts) if (cut.id == selectedId_) {
            if (std::abs(pos.x() - xFor(cut.start)) <= kEdgeHit) offer(xFor(cut.start), Drag::CutStart, cut.id);
            if (std::abs(pos.x() - xFor(cut.end)) <= kEdgeHit) offer(xFor(cut.end), Drag::CutEnd, cut.id);
        }
    }
    if (selectedKind_ == "cover") {
        const auto bar = coverBarRect(selectedId_);
        if (!bar.isNull() && pos.y() >= bar.top() - 3 && pos.y() <= bar.bottom() + 3) {
            if (std::abs(pos.x() - bar.left()) <= kEdgeHit - 1) offer(bar.left(), Drag::CoverStart, selectedId_);
            if (std::abs(pos.x() - bar.right() - 1) <= kEdgeHit - 1) offer(bar.right() + 1, Drag::CoverEnd, selectedId_);
        }
    }
    return best;
}
QString FilmstripTimeline::cutAt(QPointF pos) const {
    const auto strip = stripRect();
    if (pos.y() < strip.top() || pos.y() > strip.bottom() || pos.x() < strip.left() || pos.x() > strip.right() + 1) return {};
    for (const auto& cut : edit_.cuts) if (cutRect(cut.id).contains(pos.toPoint())) return cut.id;
    return {};
}
QString FilmstripTimeline::coverAt(QPointF pos) const {
    if (!coversRect().contains(pos.toPoint())) return {};
    for (const auto& mask : edit_.masks) if (coverBarRect(mask.id).adjusted(0, -1, 0, 1).contains(pos.toPoint())) return mask.id;
    return {};
}
void FilmstripTimeline::clearSkim() {
    skimRest_.stop();
    if (!skim_) return;
    skim_.reset();
    skimExact_ = false;
    update();
    emit skimLeft();
}
void FilmstripTimeline::restartSkimTimer() { skimRest_.start(); }

void FilmstripTimeline::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    setFocus(Qt::MouseFocusReason);
    clearSkim();
    const auto pos = e->position();
    const auto strip = stripRect();
    const double clampedX = std::clamp<double>(pos.x(), strip.left(), strip.right() + 1);
    const auto handle = handleAt(pos);
    if (handle.drag != Drag::None) {
        drag_ = handle.drag;
        dragId_ = handle.id;
        if (drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd) { dragStart_ = edit_.trimStart; dragEnd_ = edit_.exportEnd(duration_); }
        else if (drag_ == Drag::CutStart || drag_ == Drag::CutEnd) { for (const auto& cut : edit_.cuts) if (cut.id == dragId_) { dragStart_ = cut.start; dragEnd_ = cut.end; } }
        else { for (const auto& mask : edit_.masks) if (mask.id == dragId_) { dragStart_ = mask.start; dragEnd_ = mask.end; } }
        return;
    }
    const auto cover = coverAt(pos);
    if (!cover.isEmpty()) { selectLocally("cover", cover); emit coverSelected(cover); return; }
    if (pos.y() < strip.top()) {
        // The ruler: a click seeks and clears; dragging scrubs.
        drag_ = Drag::Scrub;
        clearSelections();
        emit scrubStarted();
        emit seekRequested(timeAt(clampedX));
        return;
    }
    if (pos.y() <= strip.bottom() + 1) {
        const auto cut = cutAt(pos);
        if (!cut.isEmpty()) { selectLocally("cut", cut); emit cutSelected(cut); return; }
        drag_ = Drag::Strip;
        anchorUs_ = timeAt(clampedX);
        pressX_ = pos.x();
        return;
    }
}
void FilmstripTimeline::mouseMoveEvent(QMouseEvent* e) {
    const auto pos = e->position();
    const auto strip = stripRect();
    const double clampedX = std::clamp<double>(pos.x(), strip.left(), strip.right() + 1);
    const qint64 minimum = frameUs();
    switch (drag_) {
    case Drag::None: {
        const bool over = handleAt(pos).drag != Drag::None;
        setCursor(over ? Qt::SizeHorCursor : Qt::ArrowCursor);
        // Paused hover over the ruler or the strip skims; the covers track and playing do not.
        if (playing_ || pos.y() < 0 || pos.y() > strip.bottom() + 4 || pos.x() < strip.left() || pos.x() > strip.right() + 1) { clearSkim(); return; }
        skimAt(timeAt(pos.x()));
        return;
    }
    case Drag::Scrub: emit seekRequested(timeAt(clampedX)); return;
    case Drag::TrimStart: dragStart_ = std::clamp<qint64>(snap(timeAt(clampedX)), 0, std::max<qint64>(0, snap(dragEnd_ - minimum))); emit trimEdgeDragged(true, dragStart_); update(); return;
    case Drag::TrimEnd: dragEnd_ = std::clamp<qint64>(snap(timeAt(clampedX)), std::min(duration_, snap(dragStart_ + minimum)), duration_); emit trimEdgeDragged(false, dragEnd_); update(); return;
    case Drag::CutStart: case Drag::CoverStart:
        dragStart_ = std::clamp<qint64>(snap(timeAt(clampedX)), 0, std::max<qint64>(0, snap(dragEnd_ - minimum)));
        emit editEdgeDragged(drag_ == Drag::CutStart ? QStringLiteral("cut") : QStringLiteral("cover"), dragId_, true, dragStart_);
        update();
        return;
    case Drag::CutEnd: case Drag::CoverEnd:
        dragEnd_ = std::clamp<qint64>(snap(timeAt(clampedX)), std::min(duration_, snap(dragStart_ + minimum)), duration_);
        emit editEdgeDragged(drag_ == Drag::CutEnd ? QStringLiteral("cut") : QStringLiteral("cover"), dragId_, false, dragEnd_);
        update();
        return;
    case Drag::Strip:
        if (std::abs(pos.x() - pressX_) < QApplication::startDragDistance()) return;
        drag_ = Drag::Select;
        [[fallthrough]];
    case Drag::Select: {
        const qint64 a = snap(anchorUs_), b = snap(timeAt(clampedX));
        dragStart_ = std::min(a, b);
        dragEnd_ = std::max(a, b);
        update();
        return;
    }
    }
}
void FilmstripTimeline::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || drag_ == Drag::None) return;
    const auto drag = std::exchange(drag_, Drag::None);
    const auto id = std::exchange(dragId_, QString());
    const auto strip = stripRect();
    const qint64 minimum = frameUs() - 1;
    update();
    switch (drag) {
    case Drag::TrimStart: case Drag::TrimEnd:
        emit edgeDragFinished();
        if (dragStart_ != edit_.trimStart || dragEnd_ != edit_.exportEnd(duration_)) emit trimProposed(dragStart_, dragEnd_);
        break;
    case Drag::CutStart: case Drag::CutEnd:
        emit edgeDragFinished();
        for (const auto& cut : edit_.cuts) if (cut.id == id && (cut.start != dragStart_ || cut.end != dragEnd_)) emit cutAdjusted(id, dragStart_, dragEnd_);
        break;
    case Drag::CoverStart: case Drag::CoverEnd:
        emit edgeDragFinished();
        for (const auto& mask : edit_.masks) if (mask.id == id && (mask.start != dragStart_ || mask.end != dragEnd_)) emit coverRetimed(id, dragStart_, dragEnd_);
        break;
    case Drag::Strip: {
        // A click on the strip: seek there and clear whatever is selected.
        clearSelections();
        emit seekRequested(timeAt(std::clamp<double>(e->position().x(), strip.left(), strip.right() + 1)));
        emit selectionCleared();
        break;
    }
    case Drag::Select:
        if (dragEnd_ - dragStart_ >= minimum) {
            selection_ = recording::Span{dragStart_, dragEnd_};
            selectedKind_.clear();
            selectedId_.clear();
            layoutCovers();
            emit timeSelected(dragStart_, dragEnd_);
        }
        break;
    case Drag::Scrub: case Drag::None: break;
    }
    mouseMoveEvent(e);
}
void FilmstripTimeline::leaveEvent(QEvent* e) {
    if (drag_ == Drag::None) clearSkim();
    QWidget::leaveEvent(e);
}

void FilmstripTimeline::clearSelections() {
    selection_.reset();
    selectedKind_.clear();
    selectedId_.clear();
    layoutCovers();
    update();
}

// ---- keyboard, wheel, tooltips ----
void FilmstripTimeline::keyPressEvent(QKeyEvent* e) {
    const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (plain && e->key() == Qt::Key_Escape && (selection_ || !selectedKind_.isEmpty())) {
        clearSelections();
        emit selectionCleared();
        e->accept();
        return;
    }
    if (plain && (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight)) {
        // [ and ] set the time selection's start or end at the playhead; with none, it runs to the kept footage's other end.
        const bool start = e->key() == Qt::Key_BracketLeft;
        const qint64 at = snap(playhead_), frame = frameUs();
        qint64 a = selection_ ? selection_->start : edit_.trimStart, b = selection_ ? selection_->end : edit_.exportEnd(duration_);
        if (start) { a = at; if (b - a < frame - 1) b = std::min(duration_, snap(a + frame)); }
        else { b = at; if (b - a < frame - 1) a = std::max<qint64>(0, snap(b - frame)); }
        if (b - a < frame - 1) { e->accept(); return; }
        selection_ = recording::Span{a, b};
        selectedKind_.clear();
        selectedId_.clear();
        layoutCovers();
        update();
        emit timeSelected(a, b);
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}
void FilmstripTimeline::wheelEvent(QWheelEvent* e) {
    const int delta = e->angleDelta().x() != 0 && e->angleDelta().y() == 0 ? e->angleDelta().x() : e->angleDelta().y();
    if (e->modifiers() & Qt::ControlModifier) {
        // One zoom step per wheel notch (touchpads accumulate to one).
        wheelRemainder_ += delta;
        const int notches = wheelRemainder_ / 120;
        if (notches == 0) { e->accept(); return; }
        wheelRemainder_ -= notches * 120;
        const qint64 anchor = timeAt(e->position().x());
        int index = 0;
        for (int i = 0; i < int(std::size(kZoomLevels)); ++i) if (kZoomLevels[i] == zoom_) index = i;
        index = std::clamp(index + (notches > 0 ? 1 : -1), 0, int(std::size(kZoomLevels)) - 1);
        setZoom(kZoomLevels[index], anchor);
        e->accept();
        return;
    }
    if (zoom_ <= 1 || delta == 0) { e->ignore(); return; }
    // Above Fit, the wheel and Shift+wheel scroll: a notch is a tenth of the visible span.
    setScroll(scrollUs_ - qint64(double(delta) / 120.0 * double(visibleLength()) * 0.1));
    e->accept();
}
bool FilmstripTimeline::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(e);
        const auto list = parts();
        // Later entries (edges, bars) sit on top of the earlier ones.
        for (int i = list.size() - 1; i >= 0; --i) {
            if (!list[i].rect.contains(help->pos())) continue;
            QString text = list[i].name;
            if (list[i].kind == "cut") text += QStringLiteral(". Click to select it; its panel can restore it.");
            else if (list[i].kind == "cover") text += QStringLiteral(". Click to select it.");
            else if (list[i].kind.startsWith(QLatin1String("trim"))) text += QStringLiteral(". Drag to trim.");
            QToolTip::showText(help->globalPos(), text, this);
            return true;
        }
        QToolTip::hideText();
        e->ignore();
        return true;
    }
    return QWidget::event(e);
}
void FilmstripTimeline::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    setScroll(scrollUs_);
    emit visibleSpanChanged();
}
}
