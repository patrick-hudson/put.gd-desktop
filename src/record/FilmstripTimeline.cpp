#include "FilmstripTimeline.h"
#include "Duration.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QAccessible>
#include <QAccessibleWidget>
#include <QFontMetrics>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QResizeEvent>
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
constexpr int kOverviewGap = 4;    // covers track to the overview track
constexpr int kOverviewHeight = 6; // the overview track, shown above Fit
constexpr int kBottom = 4;
constexpr int kBracketWidth = 13;  // a trim bracket's body, outside the kept range
constexpr int kBracketArm = 16;    // its arms along the strip's top and bottom
constexpr int kTilesAtFit = 12;    // across the default strip width
constexpr int kDefaultStrip = 1120;
constexpr int kMaxTiles = 24;
// Pointer model (feel spec A2-A4, A7, A9, A12, B1).
constexpr int kTrimPad = 8;        // a trim bracket's zone past its body, both sides
constexpr int kEdgeZone = 8;       // a cut or cover edge's zone, each side
constexpr int kBodyKeep = 8;       // a narrow item's body keeps this much between its edge zones
constexpr int kMinItemHit = 18;    // items narrower than this get this hit width
constexpr int kPlayheadZone = 6;   // the playhead's zone each side, ruler and strip
constexpr int kPlayheadCap = 18;   // its cap's zone width on the ruler
constexpr int kSelectionZone = 6;  // a time selection edge's zone each side
constexpr int kDeadZone = 3;       // edge, bar, scrub and selection-edge drags start past it
constexpr int kSelectStart = 4;    // a press on empty strip turns into a time selection past it
constexpr int kSkimClickMove = 2;  // no skim after a click until the pointer moves this far
constexpr int kSnapReach = 6;      // snapping reach
constexpr int kAutoscrollZone = 24;
constexpr int kAutoscrollMs = 16;
constexpr int kSkimRestMs = 40;
constexpr int kSkimIntentMs = 80;
constexpr int kIntentMoves = 4;    // the pointer deltas hover intent weighs
constexpr int kPixelZoomStep = 60; // trackpad pixels per zoom level with Ctrl

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
    l.laneHeight = std::max(Ui::scaled(20), QFontMetrics(l.bar).tightBoundingRect(QStringLiteral("Redaction Blur 0123456789")).height() + 8);
    l.chipHeight = std::max(Ui::scaled(24), QFontMetrics(l.chip).height() + 8);
    l.flagHeight = std::max(Ui::scaled(17), QFontMetrics(l.flag).height() + 2);
    l.stripY = kTop + l.rulerHeight + kRulerGap;
    l.lanesTop = l.stripY + kStripHeight + kLanesGap;
    l.lanesHeight = 2 * l.laneHeight + 4;
    l.height = l.lanesTop + l.lanesHeight + kOverviewGap + kOverviewHeight + kBottom;
    cached = l;
    return cached;
}

QColor withAlpha(QColor c, int alpha) { c.setAlpha(alpha); return c; }
bool dark() { return Theme::scheme() == Theme::Scheme::Dark; }

// The timeline's colours, every one a Theme token (or a token at an alpha, or a
// mix of two): mock review4.html's DARK / LIGHTP tables mapped to Theme.cpp.
struct Colors {
    QColor bg, tick, tickMinor, label, dim, cutFill, hatch, cutEdge, chip, chipText, tint, accent, accentHover, onAccent;
    QColor idleBar, idleBarHover, idleBarText, lane, play, playOutline, flag, flagText, hint, separator, placeholder;
    QColor lift, more, moreHover, thumb, thumbHover;
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
    c.accentHover = p.accentHover;
    c.onAccent = p.onAccent;
    c.idleBar = Ui::mix(p.surfaceRaised, p.text, 0.28);
    c.idleBarHover = Ui::mix(p.surfaceRaised, p.text, 0.4);
    c.idleBarText = p.text;
    c.lane = withAlpha(p.text, 11);
    c.play = p.text;
    c.playOutline = dark() ? withAlpha(p.well, 190) : p.bg;
    c.flag = p.text;
    c.flagText = p.bg;
    c.hint = p.textMuted;
    c.separator = dark() ? withAlpha(p.well, 160) : withAlpha(p.text, 64);
    c.placeholder = Ui::mix(p.surface, p.text, 0.03);
    c.lift = withAlpha(p.text, dark() ? 20 : 26);
    c.more = p.surfaceRaised;
    c.moreHover = Ui::mix(p.surfaceRaised, p.text, 0.14);
    c.thumb = withAlpha(p.text, dark() ? 70 : 80);
    c.thumbHover = withAlpha(p.text, dark() ? 120 : 130);
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
// A drag readout's signed change, always with a sign and one decimal: "+0.4 s", "−1.2 s".
QString deltaText(qint64 us) {
    const qint64 tenths = (std::abs(us) + 50000) / 100000;
    return QStringLiteral("%1%2.%3 s").arg(us < 0 && tenths ? QStringLiteral("−") : QStringLiteral("+")).arg(tenths / 10).arg(tenths % 10);
}
// Every digit as 0: a readout's widest text in a monospaced font, so its width never jitters.
QString widest(QString text) {
    for (auto& ch : text) if (ch.isDigit()) ch = QChar('0');
    return text;
}

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
        if (type == QAccessible::Description) {
            if (part.kind.endsWith(QLatin1String("-start")) || part.kind.endsWith(QLatin1String("-end"))) return QStringLiteral("Drag to move this edge");
            if (part.kind == QLatin1String("playhead")) return QStringLiteral("Drag to scrub");
            if (part.kind == QLatin1String("cover")) return QStringLiteral("Drag to move it in time");
            if (part.kind == QLatin1String("overview")) return QStringLiteral("Drag to scroll the timeline");
            if (part.kind == QLatin1String("more")) return QStringLiteral("Opens the Edits list");
        }
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
        if (kind == QLatin1String("overview")) return QAccessible::ScrollBar;
        if (kind == QLatin1String("more")) return QAccessible::Button;
        return kind.startsWith(QLatin1String("trim")) || kind == QLatin1String("playhead") || kind.endsWith(QLatin1String("-start")) || kind.endsWith(QLatin1String("-end")) ? QAccessible::Grip : QAccessible::ListItem;
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
    const int index = timeline()->partAt(timeline()->mapFromGlobal(QPoint(x, y)));
    return index >= 0 ? child(index) : nullptr;
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
    setAccessibleDescription("Click to move the playhead, drag across the filmstrip to select time, drag a bracket to trim, drag an edge or a cover to retime it");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_AcceptTouchEvents, false);
    skimRest_.setSingleShot(true);
    skimRest_.setInterval(kSkimRestMs);
    connect(&skimRest_, &QTimer::timeout, this, [this] { if (skim_) emit skimRested(*skim_); });
    skimIntentTimer_.setSingleShot(true);
    connect(&skimIntentTimer_, &QTimer::timeout, this, [this] {
        // The pointer rested over the ruler or strip long enough: intent if its last motion was mostly horizontal.
        if (drag_ != Drag::None || !inSkimArea_ || skimIntent_ || !skimming_ || playing_ || !lastMove_ || skimBlockedAt_ || !horizontalIntent()) return;
        skimIntent_ = true;
        hoverLine_.reset();
        skimAt(timeAt(lastMove_->x()));
    });
    autoscroll_.setInterval(kAutoscrollMs);
    connect(&autoscroll_, &QTimer::timeout, this, &FilmstripTimeline::autoscrollTick);
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
bool FilmstripTimeline::edgeDrag() const {
    return drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd || drag_ == Drag::CutStart || drag_ == Drag::CutEnd || drag_ == Drag::CoverStart || drag_ == Drag::CoverEnd || drag_ == Drag::CoverSlide;
}
void FilmstripTimeline::setEdit(const RecordingEdit& edit) {
    names_->observe(edit.masks);
    const int cuts = int(edit_.cuts.size()), covers = int(edit_.masks.size());
    // A bracket, cut edge or cover dragged from an older edit would write over
    // the newer one on release: the drag ends here, proposing nothing.
    const bool cancelDrag = edgeDrag() && edit.revision != edit_.revision;
    edit_ = edit;
    if (cancelDrag) {
        drag_ = Drag::None;
        armed_ = false;
        dragId_.clear();
        snapGuide_.reset();
        autoscroll_.stop();
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
    if (drag_ == Drag::None && lastMove_) updateHover(*lastMove_);
    update();
}
void FilmstripTimeline::setPlayhead(qint64 sourceUs) {
    const auto at = std::clamp<qint64>(sourceUs, 0, duration_);
    if (at == playhead_) return;
    playhead_ = at;
    QAccessibleValueChangeEvent changed(this, double(playhead_) / 1000000);
    QAccessible::updateAccessibility(&changed);
    // A press or drag in progress never scrolls the timeline under the pointer.
    if (drag_ == Drag::None) revealPlayhead();
    if (drag_ == Drag::None && lastMove_ && underMouse()) updateHover(*lastMove_);
    update();
}
void FilmstripTimeline::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    if (playing_) { leaveSkimArea(); }
}
void FilmstripTimeline::setTimeSelection(std::optional<recording::Span> span) {
    selection_ = span;
    if (selection_) selectedKind_.clear(), selectedId_.clear();
    if (drag_ == Drag::None && lastMove_) updateHover(*lastMove_);
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
void FilmstripTimeline::setSnapping(bool on) {
    if (snapping_ == on) return;
    snapping_ = on;
    if (!on && snapGuide_) { snapGuide_.reset(); update(); }
    emit snappingChanged(on);
}
void FilmstripTimeline::setSkimming(bool on) {
    if (skimming_ == on) return;
    skimming_ = on;
    skimIntent_ = false;
    skimIntentTimer_.stop();
    if (!on) {
        const auto at = skim_;
        clearSkim();
        if (at && inSkimArea_) hoverLine_ = at;  // the hairline and its time stay
    }
    update();
    emit skimmingChanged(on);
}
void FilmstripTimeline::skimAt(std::optional<qint64> sourceUs) {
    if (!sourceUs) { hoverLine_.reset(); clearSkim(); update(); return; }
    if (playing_) return;
    const qint64 at = std::clamp<qint64>(*sourceUs, 0, std::max<qint64>(0, duration_ - 1));
    if (!skimming_) {
        // Skimming off: only the hairline and its time; the viewer never changes on hover.
        if (hoverLine_ != at) { hoverLine_ = at; update(); }
        return;
    }
    hoverLine_.reset();
    if (skim_ == at) { restartSkimTimer(); return; }
    skim_ = at;
    skimExact_ = false;
    update();
    emit skimMoved(at);
    restartSkimTimer();
}
void FilmstripTimeline::setSkimExact(bool exact) { if (skimExact_ != exact) { skimExact_ = exact; update(); } }
void FilmstripTimeline::markSelectionEdge(bool start) {
    // The selection's start or end at the playhead; with none, it runs to the kept footage's other end.
    const qint64 at = snap(playhead_), frame = frameUs();
    qint64 a = selection_ ? selection_->start : edit_.trimStart, b = selection_ ? selection_->end : edit_.exportEnd(duration_);
    if (start) { a = at; if (b - a < frame - 1) b = std::min(duration_, snap(a + frame)); }
    else { b = at; if (b - a < frame - 1) a = std::max<qint64>(0, snap(b - frame)); }
    if (b - a < frame - 1) return;
    selection_ = recording::Span{a, b};
    selectedKind_.clear();
    selectedId_.clear();
    layoutCovers();
    update();
    emit timeSelected(a, b);
}

// ---- time and frames ----
qint64 FilmstripTimeline::frameUs() const { return std::max<qint64>(1, qRound64(1e6 / fps_)); }
qint64 FilmstripTimeline::snap(qint64 us) const {
    const qint64 n = qRound64(double(std::clamp<qint64>(us, 0, duration_)) * fps_ / 1e6);
    return std::min(duration_, qRound64(double(n) * 1e6 / fps_));
}
qint64 FilmstripTimeline::visibleLength() const { return std::max<qint64>(1, duration_ / zoom_); }
double FilmstripTimeline::usPerPixel() const { return double(visibleLength()) / stripWidth(); }
recording::Span FilmstripTimeline::visibleSpan() const { return {scrollUs_, std::min(duration_, scrollUs_ + visibleLength())}; }
int FilmstripTimeline::stripLeft() const { return kMargin; }
int FilmstripTimeline::stripWidth() const { return std::max(1, width() - 2 * kMargin); }
QRect FilmstripTimeline::rulerRect() const { return QRect(stripLeft(), kTop, stripWidth(), metrics().rulerHeight); }
QRect FilmstripTimeline::stripRect() const { return QRect(stripLeft(), metrics().stripY, stripWidth(), kStripHeight); }
QRect FilmstripTimeline::coversRect() const { return QRect(stripLeft(), metrics().lanesTop, stripWidth(), metrics().lanesHeight); }
// A lane's whole row (a bar's hit area); the bar fills it minus 1 px above and below.
QRect FilmstripTimeline::laneRect(int lane) const { return QRect(stripLeft(), metrics().lanesTop + 2 + lane * metrics().laneHeight, stripWidth(), metrics().laneHeight); }
QRect FilmstripTimeline::overviewRect() const {
    if (zoom_ <= 1) return {};
    return QRect(stripLeft(), metrics().lanesTop + metrics().lanesHeight + kOverviewGap, stripWidth(), kOverviewHeight);
}
QRect FilmstripTimeline::overviewThumb() const {
    const auto track = overviewRect();
    if (track.isNull()) return {};
    const auto span = visibleSpan();
    const double scale = double(track.width()) / double(duration_);
    int left = track.left() + int(std::lround(double(span.start) * scale)), right = track.left() + int(std::lround(double(span.end) * scale));
    if (right - left < 16) { const int mid = (left + right) / 2; left = std::max(track.left(), mid - 8); right = std::min(track.right() + 1, left + 16); left = right - 16; }
    return QRect(left, track.top(), right - left, track.height());
}
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

// ---- zoom ----
// The ladder 1, 2, 3, 4, 6, 8, 12, 16, 24, ... up to the first level at which a
// frame is about 8 px wide (at least Fit).
int FilmstripTimeline::maxZoom() const {
    const int width = this->width() >= 2 * kMargin + 64 ? stripWidth() : kDefaultStrip;
    const double frames = double(duration_) * fps_ / 1e6;
    const double needed = 8.0 * frames / width;
    if (needed <= 1) return 1;
    for (int base = 2; base <= (1 << 20); base *= 2)
        for (const int level : {base, base * 3 / 2}) if (level >= needed) return level;
    return 1 << 20;
}
QList<int> FilmstripTimeline::zoomLevels() const {
    const int top = maxZoom();
    QList<int> levels{1};
    for (int base = 2; base <= top; base *= 2)
        for (const int level : {base, base * 3 / 2}) if (level <= top) levels.append(level);
    return levels;
}
void FilmstripTimeline::setZoom(int level, std::optional<qint64> anchorUs) {
    const auto levels = zoomLevels();
    int next = 1;
    for (const int candidate : levels) if (std::abs(candidate - level) <= std::abs(next - level)) next = candidate;
    if (next == zoom_) return;
    const auto before = visibleSpan();
    // Without an anchor: the playhead when it is on screen (it stays put), else the view's centre.
    const bool playheadShown = playhead_ >= before.start && playhead_ <= before.end;
    const qint64 anchor = std::clamp<qint64>(anchorUs.value_or(playheadShown ? playhead_ : before.start + visibleLength() / 2), 0, duration_);
    // Keep the anchor under the same x; an anchor off screen is held at the nearest edge.
    const double fraction = std::clamp(double(anchor - scrollUs_) / double(visibleLength()), 0.0, 1.0);
    zoom_ = next;
    scrollUs_ = std::clamp<qint64>(anchor - qint64(std::llround(fraction * double(visibleLength()))), 0, std::max<qint64>(0, duration_ - visibleLength()));
    if (drag_ == Drag::None && lastMove_) updateHover(*lastMove_);
    update();
    emit zoomChanged(zoom_);
    emit visibleSpanChanged();
}
void FilmstripTimeline::zoomIn(std::optional<qint64> anchorUs) {
    for (const int level : zoomLevels()) if (level > zoom_) { setZoom(level, anchorUs); return; }
}
void FilmstripTimeline::zoomOut(std::optional<qint64> anchorUs) {
    const auto levels = zoomLevels();
    for (int i = int(levels.size()) - 1; i >= 0; --i) if (levels[i] < zoom_) { setZoom(levels[i], anchorUs); return; }
}
void FilmstripTimeline::zoomFit() { setZoom(1); }
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
        const auto lane = laneRect(coverLane_[i]).adjusted(0, 1, 0, -1);
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
// The "+n" chip's own space at the right end of the lower lane.
QRect FilmstripTimeline::moreRect() const {
    if (hiddenCovers_ <= 0) return {};
    const QFontMetrics fm(metrics().bar);
    const int w = fm.horizontalAdvance(QStringLiteral("+%1").arg(hiddenCovers_)) + 12;
    const auto lane = laneRect(1).adjusted(0, 1, 0, -1);
    return QRect(lane.right() + 1 - w - 2, lane.top(), w, lane.height());
}

// ---- size ----
QSize FilmstripTimeline::sizeHint() const { return {480, metrics().height}; }
QSize FilmstripTimeline::minimumSizeHint() const { return {240, metrics().height}; }

// ---- hit geometry: one list for hover, cursor, press and parts (A1, A2, A12) ----
QList<FilmstripTimeline::Zone> FilmstripTimeline::zones() const {
    QList<Zone> list;
    const auto strip = stripRect();
    const auto span = visibleSpan();
    const double stripL = strip.left(), stripR = strip.right() + 1;
    const auto shown = [&](qint64 t) { return t >= span.start && t <= span.end; };
    const auto box = [](double l, double r, int top, int bottom) {
        const int a = int(std::floor(l)), b = int(std::ceil(r));
        return QRect(a, top, std::max(1, b - a), bottom - top);
    };
    // An item's edge zones: kEdgeZone each side of each edge, shrunk toward the
    // outside on a narrow item so its body keeps kBodyKeep; and its body, at
    // least kMinItemHit wide.
    const auto item = [&](double a, double b, int top, int bottom, Target startT, Target endT, Target bodyT, const QString& id, bool startShown, bool endShown) {
        const double c = (a + b) / 2;
        const double half = std::max(double(kMinItemHit), b - a) / 2;
        list.append({bodyT, id, box(std::max(c - half, stripL), std::min(c + half, stripR), top, bottom), c, c});
        if (startShown) list.append({startT, id, box(a - kEdgeZone, std::min(a + kEdgeZone, c - kBodyKeep / 2.0), top, bottom), a, a});
        if (endShown) list.append({endT, id, box(std::max(b - kEdgeZone, c + kBodyKeep / 2.0), b + kEdgeZone, top, bottom), b, b});
    };
    // Trim brackets: the 13 px body plus kTrimPad both sides, ruler top to strip bottom + 3.
    const int trimTop = kTop, trimBottom = strip.bottom() + 1 + 3;
    const qint64 trimEnd = edit_.exportEnd(duration_);
    if (shown(edit_.trimStart)) { const double x = xFor(edit_.trimStart); list.append({Target::TrimStart, {}, box(x - kBracketWidth - kTrimPad, x + kTrimPad, trimTop, trimBottom), x - kBracketWidth, x}); }
    if (shown(trimEnd)) { const double x = xFor(trimEnd); list.append({Target::TrimEnd, {}, box(x - kTrimPad, x + kBracketWidth + kTrimPad, trimTop, trimBottom), x, x + kBracketWidth}); }
    // The playhead: ±6 px across ruler and strip, its cap 18 px wide on the ruler.
    if (shown(playhead_)) {
        const double x = xFor(playhead_);
        list.append({Target::Playhead, {}, box(x - kPlayheadCap / 2.0, x + kPlayheadCap / 2.0, kTop, strip.top()), x, x});
        list.append({Target::Playhead, {}, box(x - kPlayheadZone, x + kPlayheadZone, strip.top(), strip.bottom() + 1 + 3), x, x});
    }
    const int edgeTop = strip.top() - 3, edgeBottom = strip.bottom() + 1 + 3;
    if (selection_ && !selection_->empty()) {
        for (const bool start : {true, false}) {
            const qint64 t = start ? selection_->start : selection_->end;
            if (!shown(t)) continue;
            const double x = xFor(t);
            list.append({start ? Target::SelectionStart : Target::SelectionEnd, {}, box(x - kSelectionZone, x + kSelectionZone, edgeTop, edgeBottom), x, x});
        }
    }
    for (const auto& cut : edit_.cuts) {
        if (cut.end < span.start || cut.start > span.end) continue;
        item(xFor(cut.start), xFor(cut.end), strip.top(), strip.bottom() + 1, Target::CutStart, Target::CutEnd, Target::Cut, cut.id, shown(cut.start), shown(cut.end));
    }
    // A bar's hit area is its whole lane row.
    for (int i = 0; i < edit_.masks.size(); ++i) {
        const int lane = coverLane_.value(i, -1);
        if (lane < 0) continue;
        const auto& mask = edit_.masks[i];
        if (mask.end < span.start || mask.start > span.end) continue;
        const auto row = laneRect(lane);
        const double a = xFor(mask.start), b = std::max<double>(xFor(mask.end), a + 3);
        item(a, b, row.top(), row.bottom() + 1, Target::CoverStart, Target::CoverEnd, Target::Cover, mask.id, shown(mask.start), shown(mask.end));
    }
    if (const auto more = moreRect(); !more.isNull()) list.append({Target::More, {}, more, 0, 0});
    if (const auto track = overviewRect(); !track.isNull()) list.append({Target::Overview, {}, track.adjusted(0, -2, 0, 2), 0, 0});
    return list;
}
FilmstripTimeline::Hit FilmstripTimeline::hitAt(QPointF pos) const {
    const auto list = zones();
    const auto inside = [&](const Zone& z) { return QRectF(z.rect).contains(pos); };
    const auto hit = [](const Zone& z) { return Hit{z.target, z.id, z.rect}; };
    // The "+n" chip owns its reserved space.
    for (const auto& z : list) if (z.target == Target::More && inside(z)) return hit(z);
    // (1) The nearest edge within its zone; overlapping zones split at the midpoint.
    const Zone* best = nullptr;
    double nearest = 1e9;
    for (const auto& z : list) {
        const bool edge = z.target == Target::TrimStart || z.target == Target::TrimEnd || z.target == Target::SelectionStart || z.target == Target::SelectionEnd
            || z.target == Target::CutStart || z.target == Target::CutEnd || z.target == Target::CoverStart || z.target == Target::CoverEnd;
        if (!edge || !inside(z)) continue;
        const double d = pos.x() < z.edgeX ? z.edgeX - pos.x() : pos.x() > z.edgeX2 ? pos.x() - z.edgeX2 : 0.0;
        if (d < nearest) { nearest = d; best = &z; }
    }
    if (best) return hit(*best);
    // (2) The playhead, (3) a cut or cover body, then the overview track.
    for (const auto& z : list) if (z.target == Target::Playhead && inside(z)) return hit(z);
    for (const auto& z : list) if ((z.target == Target::Cut || z.target == Target::Cover) && inside(z)) return hit(z);
    for (const auto& z : list) if (z.target == Target::Overview && inside(z)) return hit(z);
    // (4) Empty ruler, strip or lane.
    const auto strip = stripRect();
    if (pos.y() >= 0 && pos.y() < strip.top()) return {Target::Ruler, {}, {}};
    if (pos.y() >= strip.top() && pos.y() <= strip.bottom() + 1) return {Target::Strip, {}, {}};
    if (coversRect().contains(pos.toPoint())) return {Target::Lane, {}, {}};
    return {};
}
QString FilmstripTimeline::kindOf(Target target) {
    switch (target) {
    case Target::TrimStart: return QStringLiteral("trim-start");
    case Target::TrimEnd: return QStringLiteral("trim-end");
    case Target::SelectionStart: return QStringLiteral("selection-start");
    case Target::SelectionEnd: return QStringLiteral("selection-end");
    case Target::CutStart: return QStringLiteral("cut-start");
    case Target::CutEnd: return QStringLiteral("cut-end");
    case Target::CoverStart: return QStringLiteral("cover-start");
    case Target::CoverEnd: return QStringLiteral("cover-end");
    case Target::Playhead: return QStringLiteral("playhead");
    case Target::More: return QStringLiteral("more");
    case Target::Cut: return QStringLiteral("cut");
    case Target::Cover: return QStringLiteral("cover");
    case Target::Overview: return QStringLiteral("overview");
    case Target::None: case Target::Ruler: case Target::Strip: case Target::Lane: break;
    }
    return {};
}

// ---- inspect, parts ----
QJsonObject FilmstripTimeline::inspect() const {
    const auto span = visibleSpan();
    int shown = 0, placeholders = 0;
    for (const qint64 t : tileTimes()) (tiles_ && !tiles_(t).isNull() ? shown : placeholders)++;
    QJsonValue selection = QJsonValue::Null, skim = QJsonValue::Null, hover = QJsonValue::Null;
    if (selection_) selection = QJsonObject{{"start", double(selection_->start)}, {"end", double(selection_->end)}};
    if (skim_) skim = QJsonObject{{"time", double(*skim_)}, {"exact", skimExact_}};
    if (const auto kind = kindOf(hover_.target); !kind.isEmpty()) hover = QJsonObject{{"kind", kind}, {"id", hover_.id}};
    QJsonArray levels;
    for (const int level : zoomLevels()) levels.append(level);
    return {{"zoom", zoom_},
            {"zoomLevels", levels},
            {"visible", QJsonObject{{"start", double(span.start)}, {"end", double(span.end)}}},
            {"selection", selection},
            {"selectedEdit", QJsonObject{{"kind", selectedKind_}, {"id", selectedId_}}},
            {"skim", skim},
            {"snapping", snapping_},
            {"skimming", skimming_},
            {"hover", hover},
            {"lanes", lanesUsed_},
            {"hiddenCovers", hiddenCovers_},
            {"tiles", QJsonObject{{"shown", shown}, {"placeholders", placeholders}}}};
}
QList<FilmstripTimeline::Part> FilmstripTimeline::parts() const {
    QList<Part> list;
    const qint64 trimEnd = edit_.exportEnd(duration_);
    const auto maskFor = [this](const QString& id) -> const RecordingMask* { for (const auto& m : edit_.masks) if (m.id == id) return &m; return nullptr; };
    const auto cutFor = [this](const QString& id) -> const RecordingCut* { for (const auto& c : edit_.cuts) if (c.id == id) return &c; return nullptr; };
    for (const auto& z : zones()) {
        const auto kind = kindOf(z.target);
        QString name;
        switch (z.target) {
        case Target::TrimStart: name = QStringLiteral("Trim start, %1").arg(clock(edit_.trimStart)); break;
        case Target::TrimEnd: name = QStringLiteral("Trim end, %1").arg(clock(trimEnd)); break;
        case Target::Playhead:
            // The cap and the line are one part.
            if (!list.isEmpty() && list.last().kind == kind) { list.last().rect |= z.rect; continue; }
            name = QStringLiteral("Playhead, %1").arg(clock(playhead_));
            break;
        case Target::SelectionStart: name = QStringLiteral("Selection start, %1").arg(clock(selection_->start)); break;
        case Target::SelectionEnd: name = QStringLiteral("Selection end, %1").arg(clock(selection_->end)); break;
        case Target::Cut: if (const auto* c = cutFor(z.id)) name = QStringLiteral("Cut, %1, %2 left out").arg(spanText(c->start, c->end), lengthText(c->end - c->start)); break;
        case Target::CutStart: if (const auto* c = cutFor(z.id)) name = QStringLiteral("Cut start edge, %1").arg(clock(c->start)); break;
        case Target::CutEnd: if (const auto* c = cutFor(z.id)) name = QStringLiteral("Cut end edge, %1").arg(clock(c->end)); break;
        case Target::Cover: if (const auto* m = maskFor(z.id)) name = QStringLiteral("%1, %2").arg(names_->name(*m), spanText(m->start, m->end)); break;
        case Target::CoverStart: if (const auto* m = maskFor(z.id)) name = QStringLiteral("%1 start edge, %2").arg(names_->name(*m), clock(m->start)); break;
        case Target::CoverEnd: if (const auto* m = maskFor(z.id)) name = QStringLiteral("%1 end edge, %2").arg(names_->name(*m), clock(m->end)); break;
        case Target::More: {
            QStringList hidden;
            for (int i = 0; i < edit_.masks.size(); ++i) if (coverLane_.value(i, -1) < 0) hidden << names_->name(edit_.masks[i]);
            name = QStringLiteral("%1 more covers not shown: %2").arg(hiddenCovers_).arg(hidden.join(QStringLiteral(", ")));
            break;
        }
        case Target::Overview: { const auto span = visibleSpan(); name = QStringLiteral("Visible part of the timeline, %1").arg(spanText(span.start, span.end)); break; }
        case Target::None: case Target::Ruler: case Target::Strip: case Target::Lane: continue;
        }
        list.append({kind, z.id, name, z.rect});
    }
    return list;
}

int FilmstripTimeline::partAt(QPoint at) const {
    const auto hit = hitAt(QPointF(at));
    const auto kind = kindOf(hit.target);
    if (kind.isEmpty()) return -1;
    const auto list = parts();
    for (int i = 0; i < list.size(); ++i) if (list[i].kind == kind && list[i].id == hit.id) return i;
    return -1;
}

// ---- painting ----
bool FilmstripTimeline::hovered(Target target, const QString& id) const { return hover_.target == target && hover_.id == id; }
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
    const auto liveCover = [&](const RecordingMask& mask) { return (drag_ == Drag::CoverStart || drag_ == Drag::CoverEnd || drag_ == Drag::CoverSlide) && dragId_ == mask.id ? recording::Span{dragStart_, dragEnd_} : recording::Span{mask.start, mask.end}; };
    const auto xAt = [&](qint64 t) { return double(xFor(t)); };
    const auto paintFlag = [&](const QRectF& r, const QString& text) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.flag);
        p.drawRoundedRect(r, 4, 4);
        Ui::paintText(p, r, text, c.flagText, L.flag, Qt::AlignCenter, Qt::ElideNone);
    };
    const QFontMetrics flagMetrics(L.flag);

    // Ruler in source time, captioned at its right end. The skim flag draws over
    // it with its own background, so labels never blink under it (B3).
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
        // A hovered cut body lifts (A10).
        if (hovered(Target::Cut, cut.id)) p.fillRect(QRectF(a, strip.top(), b - a, strip.height()), c.lift);
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
    if (drag_ == Drag::Select || drag_ == Drag::SelectionStart || drag_ == Drag::SelectionEnd) shownSelection = recording::Span{dragStart_, dragEnd_};
    if (shownSelection && !shownSelection->empty()) {
        const QRectF box(xAt(shownSelection->start), strip.top() + 0.75, xAt(shownSelection->end) - xAt(shownSelection->start), strip.height() - 1.5);
        p.fillRect(box, withAlpha(c.accent, 30));
        QPen dashed(c.accent, 1.5, Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawRect(box);
        // A hovered or dragged selection edge widens to a solid 3 px line.
        const bool startLit = hovered(Target::SelectionStart) || drag_ == Drag::SelectionStart || (drag_ == Drag::Select && dragStart_ != snap(anchorUs_));
        const bool endLit = hovered(Target::SelectionEnd) || drag_ == Drag::SelectionEnd || (drag_ == Drag::Select && dragStart_ == snap(anchorUs_));
        if (startLit) p.fillRect(QRectF(box.left() - 1.5, strip.top(), 3, strip.height()), c.accent);
        if (endLit) p.fillRect(QRectF(box.right() - 1.5, strip.top(), 3, strip.height()), c.accent);
    }
    p.restore();

    // Cut chips and edges (outside the clip so a chip can overhang a short cut's tiles).
    for (const auto& cut : edit_.cuts) {
        const auto live = liveCut(cut);
        const double a = xAt(live.start), b = xAt(live.end);
        if (b < left || a > right) continue;
        const bool selected = selectedKind_ == "cut" && selectedId_ == cut.id;
        p.setPen(Qt::NoPen);
        // Edges brighten and widen 2 -> 3 px when hovered or dragged.
        for (const bool start : {true, false}) {
            const bool lit = hovered(start ? Target::CutStart : Target::CutEnd, cut.id) || (dragId_ == cut.id && drag_ == (start ? Drag::CutStart : Drag::CutEnd));
            const double w = lit ? 3 : 1.5;
            p.setBrush(lit ? c.play : c.cutEdge);
            p.drawRect(QRectF(start ? a : b - w, strip.top(), w, strip.height()));
        }
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
        const bool lit = hovered(start ? Target::TrimStart : Target::TrimEnd) || drag_ == (start ? Drag::TrimStart : Drag::TrimEnd);
        const double x = xAt(t);
        const double bx = start ? x - kBracketWidth : x;
        const QRectF body(bx, strip.top() - 2, kBracketWidth, strip.height() + 4);
        const QColor fill = lit ? c.accentHover : c.accent;
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(bracketPath(body, 5, start));
        p.fillRect(QRectF(start ? x : x - kBracketArm, strip.top() - 2, kBracketArm, 2), fill);
        p.fillRect(QRectF(start ? x : x - kBracketArm, strip.bottom() + 1, kBracketArm, 2), fill);
        const double grip = lit ? 3 : 2;
        p.fillRect(QRectF(bx + 6.5 - grip / 2, strip.center().y() - 8.5, grip, 18), c.onAccent);
    }

    // Covers track; the "+n" chip keeps its own space at the lower lane's right end.
    {
        const auto lanes = coversRect();
        const auto more = moreRect();
        p.setPen(Qt::NoPen);
        p.setBrush(c.lane);
        p.drawRoundedRect(QRectF(lanes), 4, 4);
        if (edit_.masks.isEmpty()) {
            Ui::paintText(p, QRectF(lanes.left() + 10, lanes.top(), lanes.width() - 20, lanes.height()), QStringLiteral("No covers yet"), c.hint, L.hint, Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            for (int i = 0; i < edit_.masks.size(); ++i) {
                if (coverLane_.value(i, -1) < 0) continue;
                const auto& mask = edit_.masks[i];
                const bool selected = selectedKind_ == "cover" && selectedId_ == mask.id;
                const auto live = liveCover(mask);
                const auto lane = laneRect(coverLane_[i]).adjusted(0, 1, 0, -1);
                const double a = xAt(live.start), b = std::max(xAt(live.end), a + 3);
                if (b < lanes.left() || a > lanes.right()) continue;
                p.save();
                QPainterPath laneClip;
                laneClip.addRoundedRect(QRectF(lanes), 4, 4);
                p.setClipPath(laneClip);
                if (coverLane_[i] == 1 && !more.isNull()) p.setClipRect(QRectF(lanes.left(), lanes.top(), more.left() - 3 - lanes.left(), lanes.height()), Qt::IntersectClip);
                const QRectF bar(a, lane.top(), b - a, lane.height());
                const bool lit = hovered(Target::Cover, mask.id) || (drag_ == Drag::CoverSlide && dragId_ == mask.id);
                p.setPen(Qt::NoPen);
                p.setBrush(selected ? (lit ? c.accentHover : c.accent) : (lit ? c.idleBarHover : c.idleBar));
                p.drawRoundedRect(bar, 3, 3);
                const QColor ink = selected ? c.onAccent : c.idleBarText;
                const QRectF text(std::max(bar.left(), double(lanes.left())) + 8, bar.top(), std::min(bar.right(), double(lanes.right() + 1)) - std::max(bar.left(), double(lanes.left())) - 16, bar.height());
                if (text.width() > 12) Ui::paintText(p, text, names_->name(mask), ink, L.bar, Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideRight);
                // End grips: on the selected bar always, on any bar while its edge is hovered or dragged (2 -> 3 px).
                for (const bool start : {true, false}) {
                    const bool edgeLit = hovered(start ? Target::CoverStart : Target::CoverEnd, mask.id) || (dragId_ == mask.id && drag_ == (start ? Drag::CoverStart : Drag::CoverEnd));
                    if (!selected && !edgeLit) continue;
                    const double w = edgeLit ? 3 : 2;
                    p.setBrush(ink);
                    p.drawRect(QRectF(start ? bar.left() + 2 : bar.right() - 2 - w, bar.top() + 3, w, bar.height() - 6));
                }
                p.restore();
            }
            if (!more.isNull()) {
                const QString label = QStringLiteral("+%1").arg(hiddenCovers_);
                p.setPen(Qt::NoPen);
                p.setBrush(hovered(Target::More) ? c.moreHover : c.more);
                p.drawRoundedRect(QRectF(more), 3, 3);
                Ui::paintText(p, QRectF(more), label, pal.textSecondary, L.bar, Qt::AlignCenter, Qt::ElideNone);
            }
        }
    }

    // The overview track above Fit: where the visible span sits in the recording.
    if (const auto track = overviewRect(); !track.isNull()) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.lane);
        p.drawRoundedRect(QRectF(track), track.height() / 2.0, track.height() / 2.0);
        const auto thumb = overviewThumb();
        p.setBrush(hovered(Target::Overview) || drag_ == Drag::Overview ? c.thumbHover : c.thumb);
        p.drawRoundedRect(QRectF(thumb), thumb.height() / 2.0, thumb.height() / 2.0);
        const double px = track.left() + double(playhead_) * track.width() / double(duration_);
        p.fillRect(QRectF(px - 0.5, track.top(), 1, track.height()), c.play);
    }

    // Grips of a selected cut, then the playhead above them.
    if (selectedKind_ == "cut") for (const auto& cut : edit_.cuts) if (cut.id == selectedId_) {
        const auto live = liveCut(cut);
        for (const bool start : {true, false}) {
            const qint64 t = start ? live.start : live.end;
            if (t < span.start || t > span.end) continue;
            const bool lit = hovered(start ? Target::CutStart : Target::CutEnd, cut.id) || (dragId_ == cut.id && drag_ == (start ? Drag::CutStart : Drag::CutEnd));
            const double x = xAt(t), y = strip.center().y();
            p.setPen(Qt::NoPen);
            p.setBrush(c.playOutline);
            p.drawRoundedRect(QRectF(x - 5, y - 13, 10, 26), 3, 3);
            p.setBrush(lit ? c.accentHover : c.accent);
            p.drawRoundedRect(QRectF(x - 4, y - 12, 8, 24), 3, 3);
        }
    }
    if (playhead_ >= span.start && playhead_ <= span.end) {
        const bool lit = hovered(Target::Playhead) || drag_ == Drag::Playhead;
        const double x = xAt(playhead_), line = lit ? 3 : 2;
        const double end = strip.bottom() + 1 + 4;
        p.setPen(Qt::NoPen);
        p.setBrush(c.playOutline);
        p.drawRect(QRectF(x - line / 2 - 1, kTop + 4, line + 2, end - kTop - 4));
        p.setBrush(c.play);
        p.drawRect(QRectF(x - line / 2, kTop + 4, line, end - kTop - 4));
        const QPolygonF cap{QPointF(x - 7, kTop), QPointF(x + 7, kTop), QPointF(x, kTop + 8)};
        p.setPen(QPen(c.playOutline, 1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawPolygon(cap);
    }

    // The snap guide (A7): 1 px accent from the ruler to the lanes while snapped.
    if (snapGuide_ && armed_ && drag_ != Drag::None && *snapGuide_ >= span.start && *snapGuide_ <= span.end) {
        const auto lanes = coversRect();
        p.fillRect(QRectF(xAt(*snapGuide_), kTop, 1, lanes.bottom() + 1 - kTop), c.accent);
    }

    // The drag readout (A8), in the skim flag's style above the strip, beside
    // the dragged edge (never over it), its width reserved from the widest text.
    if (armed_ && drag_ != Drag::None && drag_ != Drag::Scrub && drag_ != Drag::Playhead && drag_ != Drag::Overview) {
        QString text, widestText;
        double at = 0;
        int side = 0;  // -1: the flag ends left of the edge, 1: it starts right of it, 0: centred
        const QString longest = clock(duration_);
        switch (drag_) {
        case Drag::TrimStart: case Drag::TrimEnd: {
            const bool start = drag_ == Drag::TrimStart;
            RecordingEdit trimmed = edit_;
            trimmed.trimStart = dragStart_;
            trimmed.trimEnd = dragEnd_;
            at = start ? dragStart_ : dragEnd_;
            text = QStringLiteral("%1 · %2 clip").arg(clock(qint64(at)), lengthText(trimmed.exportDuration(duration_)));
            widestText = QStringLiteral("%1 · %2 clip").arg(longest, lengthText(duration_));
            side = start ? 1 : -1;
            break;
        }
        case Drag::CutStart: case Drag::CutEnd: case Drag::CoverStart: case Drag::CoverEnd: case Drag::CoverSlide: {
            const bool start = drag_ == Drag::CutStart || drag_ == Drag::CoverStart || drag_ == Drag::CoverSlide;
            at = start ? dragStart_ : dragEnd_;
            text = QStringLiteral("%1 · %2").arg(clock(qint64(at)), deltaText(qint64(at) - (start ? originStart_ : originEnd_)));
            widestText = QStringLiteral("%1 · −%2").arg(longest, deltaText(duration_).mid(1));
            side = start ? -1 : 1;
            break;
        }
        case Drag::Select: case Drag::SelectionStart: case Drag::SelectionEnd:
            at = (dragStart_ + dragEnd_) / 2.0;
            text = lengthText(dragEnd_ - dragStart_);
            widestText = lengthText(duration_);
            break;
        default: break;
        }
        if (!text.isEmpty()) {
            const double w = std::max(flagMetrics.horizontalAdvance(widest(widestText)), flagMetrics.horizontalAdvance(text)) + 12;
            const double x = drag_ == Drag::Select || drag_ == Drag::SelectionStart || drag_ == Drag::SelectionEnd ? (xAt(dragStart_) + xAt(dragEnd_)) / 2.0 : xAt(qint64(at));
            const double flagLeft = side < 0 ? x - 6 - w : side > 0 ? x + 6 : x - w / 2;
            paintFlag(QRectF(std::clamp<double>(flagLeft, 0, std::max(0.0, width() - w)), kTop - 2, w, L.flagHeight), text);
        }
    }

    // The skim line and its own time flag; before hover intent or with skimming
    // off, the hairline and its time alone.
    if (drag_ == Drag::None && (skim_ || hoverLine_)) {
        const qint64 t = skim_ ? *skim_ : *hoverLine_;
        const QString text = (skim_ && !skimExact_ ? QStringLiteral("≈") : QString()) + clock(t);
        // A fixed "≈" slot, so the flag does not change width when the exact frame arrives.
        const double w = flagMetrics.horizontalAdvance(QStringLiteral("≈") + widest(clock(t))) + 12;
        const double x = xAt(t);
        p.setOpacity(0.7);
        p.fillRect(QRectF(x - 0.5, strip.top() - 6, 1, strip.height() + 12), c.play);
        p.setOpacity(1);
        paintFlag(QRectF(std::clamp<double>(x - w / 2, 0, std::max(0.0, width() - w)), kTop - 2, w, L.flagHeight), text);
    }
    if (Ui::keyboardFocus(this)) {
        p.setPen(QPen(pal.accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    }
}

// ---- hover (A10) and skim intent (B1) ----
void FilmstripTimeline::setHover(const Hit& hit) {
    Hit next = hit;
    if (kindOf(next.target).isEmpty()) next = {};  // empty space is not a hover target
    if (next == hover_) { hover_.rect = next.rect; return; }
    // Only the old and new targets repaint; the margins cover a bracket's arms,
    // and the playhead repaints its whole column (cap and line).
    const auto dirty = [this](const Hit& h) {
        if (h.rect.isNull()) return;
        if (h.target == Target::Playhead) update(QRect(h.rect.center().x() - kPlayheadCap, 0, 2 * kPlayheadCap, height()));
        else update(h.rect.adjusted(-kBracketArm - 4, -4, kBracketArm + 4, 8));
    };
    dirty(hover_);
    dirty(next);
    hover_ = next;
}
void FilmstripTimeline::updateHover(QPointF pos) {
    if (drag_ != Drag::None) return;  // the dragged target stays lit until release
    const auto hit = hitAt(pos);
    setHover(hit);
    switch (hit.target) {
    case Target::TrimStart: case Target::TrimEnd: case Target::SelectionStart: case Target::SelectionEnd:
    case Target::CutStart: case Target::CutEnd: case Target::CoverStart: case Target::CoverEnd: case Target::Playhead:
        setCursor(Qt::SizeHorCursor); break;
    case Target::Cover: setCursor(Qt::OpenHandCursor); break;
    case Target::Cut: case Target::More: setCursor(Qt::PointingHandCursor); break;
    case Target::Overview: setCursor(overviewThumb().contains(pos.toPoint()) ? Qt::OpenHandCursor : Qt::ArrowCursor); break;
    default: setCursor(Qt::ArrowCursor); break;
    }
}
bool FilmstripTimeline::horizontalIntent() const {
    double dx = 0, dy = 0;
    for (const auto& d : recentMoves_) { dx += d.x(); dy += d.y(); }
    return std::abs(dx) >= std::abs(dy);
}
void FilmstripTimeline::hoverSkim(QPointF pos) {
    const auto strip = stripRect();
    const bool area = !playing_ && pos.y() >= 0 && pos.y() <= strip.bottom() + 4 && pos.x() >= strip.left() && pos.x() <= strip.right() + 1;
    if (!area) { leaveSkimArea(); return; }
    if (!inSkimArea_) { inSkimArea_ = true; skimIntent_ = false; skimEntered_.start(); }
    // After a click, nothing skims until the pointer has moved kSkimClickMove.
    if (skimBlockedAt_) {
        if (QLineF(*skimBlockedAt_, pos).length() < kSkimClickMove) return;
        skimBlockedAt_.reset();
    }
    const qint64 at = std::clamp<qint64>(timeAt(pos.x()), 0, std::max<qint64>(0, duration_ - 1));
    if (skimming_ && !skimIntent_) {
        if (skimEntered_.elapsed() >= kSkimIntentMs && horizontalIntent()) skimIntent_ = true;
        else if (!skimIntentTimer_.isActive()) skimIntentTimer_.start(int(std::max<qint64>(1, kSkimIntentMs - skimEntered_.elapsed())));
    }
    if (!skimming_ || !skimIntent_) {
        // No intent yet (or skimming off): the hairline and its time, nothing in the viewer.
        if (hoverLine_ != at) { hoverLine_ = at; update(); }
        return;
    }
    skimAt(at);
}
void FilmstripTimeline::leaveSkimArea() {
    inSkimArea_ = false;
    skimIntent_ = false;
    skimIntentTimer_.stop();
    if (hoverLine_) { hoverLine_.reset(); update(); }
    clearSkim();
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
void FilmstripTimeline::clearSelections() {
    selection_.reset();
    selectedKind_.clear();
    selectedId_.clear();
    layoutCovers();
    update();
}

// ---- drags (A3-A9) ----
void FilmstripTimeline::beginDrag(Drag drag, const QString& id, QPointF pos) {
    drag_ = drag;
    dragId_ = id;
    armed_ = false;
    pressPos_ = dragPos_ = pos;
    snapGuide_.reset();
    const auto strip = stripRect();
    const double clampedX = std::clamp<double>(pos.x(), strip.left(), strip.right() + 1);
    pressTimeUs_ = double(scrollUs_) + (clampedX - strip.left()) * usPerPixel();
    originScroll_ = scrollUs_;
    originStart_ = originEnd_ = 0;
    switch (drag) {
    case Drag::TrimStart: case Drag::TrimEnd: originStart_ = edit_.trimStart; originEnd_ = edit_.exportEnd(duration_); break;
    case Drag::CutStart: case Drag::CutEnd: for (const auto& cut : edit_.cuts) if (cut.id == id) originStart_ = cut.start, originEnd_ = cut.end; break;
    case Drag::CoverStart: case Drag::CoverEnd: case Drag::CoverSlide: for (const auto& mask : edit_.masks) if (mask.id == id) originStart_ = mask.start, originEnd_ = mask.end; break;
    case Drag::SelectionStart: case Drag::SelectionEnd: if (selection_) originStart_ = selection_->start, originEnd_ = selection_->end; break;
    case Drag::Overview: {
        // A press beside the thumb centres the view there; the drag carries on from it.
        const auto track = overviewRect();
        if (!overviewThumb().contains(pos.toPoint()) && track.width() > 0) {
            const qint64 at = qint64(std::llround((pos.x() - track.left()) * double(duration_) / track.width()));
            setScroll(at - visibleLength() / 2);
            originScroll_ = scrollUs_;
        }
        armed_ = true;
        setCursor(Qt::ClosedHandCursor);
        break;
    }
    default: break;
    }
    dragStart_ = originStart_;
    dragEnd_ = originEnd_;
}
// Edges, selection ends and scrubs snap to the playhead, trim ends, cut edges,
// cover edges and selection edges within kSnapReach (never to the dragged item
// itself); frames snap underneath. `second` also tries us + second (a slid
// cover's end) and returns the start that puts that end on the target.
qint64 FilmstripTimeline::snapToTargets(qint64 us, bool bypass, std::optional<qint64> second) {
    snapGuide_.reset();
    if (bypass) return snap(us);
    QList<qint64> targets;
    if (drag_ != Drag::Scrub && drag_ != Drag::Playhead) targets << playhead_;
    if (drag_ != Drag::TrimStart && drag_ != Drag::TrimEnd) targets << edit_.trimStart << edit_.exportEnd(duration_);
    const bool cutDrag = drag_ == Drag::CutStart || drag_ == Drag::CutEnd;
    for (const auto& cut : edit_.cuts) if (!(cutDrag && cut.id == dragId_)) targets << cut.start << cut.end;
    const bool coverDrag = drag_ == Drag::CoverStart || drag_ == Drag::CoverEnd || drag_ == Drag::CoverSlide;
    for (int i = 0; i < edit_.masks.size(); ++i) {
        const auto& mask = edit_.masks[i];
        if (coverLane_.value(i, -1) < 0 || (coverDrag && mask.id == dragId_)) continue;
        targets << mask.start << mask.end;
    }
    const bool selectionDrag = drag_ == Drag::Select || drag_ == Drag::SelectionStart || drag_ == Drag::SelectionEnd;
    if (selection_ && !selectionDrag) targets << selection_->start << selection_->end;
    const double reach = kSnapReach * usPerPixel();
    double nearest = reach + 1;
    std::optional<qint64> result;
    for (const qint64 t : targets) {
        if (const double d = std::abs(double(t - us)); d <= reach && d < nearest) { nearest = d; result = t; snapGuide_ = t; }
        if (second) if (const double d = std::abs(double(t - (us + *second))); d <= reach && d < nearest) { nearest = d; result = t - *second; snapGuide_ = t; }
    }
    return snap(result.value_or(us));
}
void FilmstripTimeline::updateDrag(QPointF pos, Qt::KeyboardModifiers modifiers) {
    dragPos_ = pos;
    dragModifiers_ = modifiers;
    const auto strip = stripRect();
    if (drag_ == Drag::Overview) {
        const auto track = overviewRect();
        if (track.width() > 0) setScroll(originScroll_ + qint64(std::llround((pos.x() - pressPos_.x()) * double(duration_) / track.width())));
        return;
    }
    if (!armed_) {
        // The dead zone: nothing changes until the pointer has moved this far horizontally.
        if (std::abs(pos.x() - pressPos_.x()) < (drag_ == Drag::Strip ? kSelectStart : kDeadZone)) return;
        armed_ = true;
        if (drag_ == Drag::Scrub || drag_ == Drag::Playhead) emit scrubStarted();
        if (drag_ == Drag::Strip) drag_ = Drag::Select;
        if (drag_ == Drag::CoverSlide) setCursor(Qt::ClosedHandCursor);
        else if (drag_ != Drag::Scrub && drag_ != Drag::Select) setCursor(Qt::SizeHorCursor);
    }
    const double clampedX = std::clamp<double>(pos.x(), strip.left(), strip.right() + 1);
    const bool bypass = !snapping_ || (modifiers & Qt::ControlModifier);
    // The grab offset: an edge moves by the pointer's delta from the press, never to its absolute x.
    const qint64 delta = qint64(std::llround(double(scrollUs_) + (clampedX - strip.left()) * usPerPixel() - pressTimeUs_));
    const qint64 minimum = frameUs();
    const auto kindName = [this] { return drag_ == Drag::CutStart || drag_ == Drag::CutEnd ? QStringLiteral("cut") : drag_ == Drag::SelectionStart || drag_ == Drag::SelectionEnd ? QStringLiteral("selection") : QStringLiteral("cover"); };
    const auto moveStart = [&](qint64 wanted) {
        const qint64 t = snapToTargets(wanted, bypass);
        dragStart_ = std::clamp<qint64>(t, 0, std::max<qint64>(0, snap(dragEnd_ - minimum)));
        if (dragStart_ != t) snapGuide_.reset();
    };
    const auto moveEnd = [&](qint64 wanted) {
        const qint64 t = snapToTargets(wanted, bypass);
        dragEnd_ = std::clamp<qint64>(t, std::min(duration_, snap(dragStart_ + minimum)), duration_);
        if (dragEnd_ != t) snapGuide_.reset();
    };
    switch (drag_) {
    case Drag::None: case Drag::Strip: case Drag::Overview: return;
    case Drag::Scrub: emit seekRequested(snapToTargets(timeAt(clampedX), bypass)); break;
    case Drag::Playhead: emit seekRequested(snapToTargets(std::clamp<qint64>(prePlayhead_ + delta, 0, duration_), bypass)); break;
    case Drag::TrimStart: moveStart(originStart_ + delta); emit trimEdgeDragged(true, dragStart_); break;
    case Drag::TrimEnd: moveEnd(originEnd_ + delta); emit trimEdgeDragged(false, dragEnd_); break;
    case Drag::CutStart: case Drag::CoverStart: case Drag::SelectionStart:
        moveStart(originStart_ + delta);
        emit editEdgeDragged(kindName(), dragId_, true, dragStart_);
        break;
    case Drag::CutEnd: case Drag::CoverEnd: case Drag::SelectionEnd:
        moveEnd(originEnd_ + delta);
        emit editEdgeDragged(kindName(), dragId_, false, dragEnd_);
        break;
    case Drag::CoverSlide: {
        // The whole cover slides in time, clamped to the recording.
        const qint64 length = originEnd_ - originStart_;
        const qint64 wanted = snapToTargets(originStart_ + delta, bypass, length);
        dragStart_ = std::clamp<qint64>(wanted, 0, std::max<qint64>(0, duration_ - length));
        if (dragStart_ != wanted) snapGuide_.reset();
        dragEnd_ = std::min(duration_, dragStart_ + length);
        emit editEdgeDragged(QStringLiteral("cover"), dragId_, true, dragStart_);
        break;
    }
    case Drag::Select: {
        // Anchored at the press; the moving end follows the pointer and the viewer shows its frame.
        const qint64 anchor = snap(anchorUs_), moving = snapToTargets(timeAt(clampedX), bypass);
        dragStart_ = std::min(anchor, moving);
        dragEnd_ = std::max(anchor, moving);
        emit editEdgeDragged(QStringLiteral("selection"), {}, moving < anchor, moving);
        break;
    }
    }
    // Autoscroll (A9): above Fit, within kAutoscrollZone of a strip end or past it.
    if (zoom_ > 1 && (pos.x() < strip.left() + kAutoscrollZone || pos.x() > strip.right() + 1 - kAutoscrollZone)) {
        if (!autoscroll_.isActive()) { autoscrollClock_.start(); autoscroll_.start(); }
    } else {
        autoscroll_.stop();
    }
    update();
}
void FilmstripTimeline::autoscrollTick() {
    if (drag_ == Drag::None || drag_ == Drag::Overview || !armed_ || zoom_ <= 1) { autoscroll_.stop(); return; }
    const auto strip = stripRect();
    const double x = dragPos_.x();
    double overshoot = 0;
    int direction = 0;
    if (x < strip.left() + kAutoscrollZone) { overshoot = strip.left() + kAutoscrollZone - x; direction = -1; }
    else if (x > strip.right() + 1 - kAutoscrollZone) { overshoot = x - (strip.right() + 1 - kAutoscrollZone); direction = 1; }
    if (!direction) { autoscroll_.stop(); return; }
    // 0.5 visible spans a second at the zone's inner edge, up to 3 at 48 px past the strip end.
    const double spansPerSecond = 0.5 + 2.5 * std::clamp(overshoot / (3.0 * kAutoscrollZone), 0.0, 1.0);
    const double seconds = std::clamp(autoscrollClock_.restart() / 1000.0, 0.001, 0.05);
    const qint64 step = std::max<qint64>(1, qint64(std::llround(spansPerSecond * double(visibleLength()) * seconds)));
    const qint64 before = scrollUs_;
    setScroll(scrollUs_ + direction * step);
    if (scrollUs_ != before) updateDrag(dragPos_, dragModifiers_);
}
void FilmstripTimeline::cancelDrag() {
    // Esc or a right press (A6): the pre-drag state comes back and nothing is proposed.
    const auto drag = std::exchange(drag_, Drag::None);
    armed_ = false;
    dragId_.clear();
    snapGuide_.reset();
    autoscroll_.stop();
    switch (drag) {
    case Drag::TrimStart: case Drag::TrimEnd: case Drag::CutStart: case Drag::CutEnd: case Drag::CoverStart: case Drag::CoverEnd:
    case Drag::CoverSlide: case Drag::SelectionStart: case Drag::SelectionEnd: case Drag::Select:
        emit edgeDragFinished();
        break;
    case Drag::Overview: setScroll(originScroll_); break;
    default: break;
    }
    if ((drag == Drag::Scrub || drag == Drag::Playhead || drag == Drag::Strip || drag == Drag::Select) && playhead_ != prePlayhead_) emit seekRequested(prePlayhead_);
    update();
    updateHover(dragPos_);
}

// ---- pointer ----
void FilmstripTimeline::mousePressEvent(QMouseEvent* e) {
    if (drag_ != Drag::None) {
        if (e->button() == Qt::RightButton) cancelDrag();
        e->accept();
        return;
    }
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    setFocus(Qt::MouseFocusReason);
    clearSkim();
    hoverLine_.reset();
    const auto pos = e->position();
    skimBlockedAt_ = pos;
    prePlayhead_ = playhead_;
    const auto strip = stripRect();
    const double clampedX = std::clamp<double>(pos.x(), strip.left(), strip.right() + 1);
    const auto hit = hitAt(pos);
    setHover(hit);
    // One gesture: an unselected cut's or cover's edge selects it and drags at once (A5).
    const auto select = [this](const QString& kind, const QString& id) {
        if (selectedKind_ == kind && selectedId_ == id) return;
        selectLocally(kind, id);
        if (kind == "cut") emit cutSelected(id); else emit coverSelected(id);
    };
    switch (hit.target) {
    case Target::More: emit moreCoversRequested(); return;
    case Target::TrimStart: beginDrag(Drag::TrimStart, {}, pos); return;
    case Target::TrimEnd: beginDrag(Drag::TrimEnd, {}, pos); return;
    case Target::SelectionStart: beginDrag(Drag::SelectionStart, {}, pos); return;
    case Target::SelectionEnd: beginDrag(Drag::SelectionEnd, {}, pos); return;
    case Target::CutStart: case Target::CutEnd:
        beginDrag(hit.target == Target::CutStart ? Drag::CutStart : Drag::CutEnd, hit.id, pos);
        select("cut", hit.id);
        return;
    case Target::CoverStart: case Target::CoverEnd:
        beginDrag(hit.target == Target::CoverStart ? Drag::CoverStart : Drag::CoverEnd, hit.id, pos);
        select("cover", hit.id);
        return;
    case Target::Playhead: beginDrag(Drag::Playhead, {}, pos); return;
    case Target::Cut: select("cut", hit.id); return;
    case Target::Cover: beginDrag(Drag::CoverSlide, hit.id, pos); select("cover", hit.id); return;
    case Target::Overview: beginDrag(Drag::Overview, {}, pos); return;
    case Target::Ruler:
        // Seeks on press; a drag scrubs. The selection stays (A14).
        beginDrag(Drag::Scrub, {}, pos);
        emit seekRequested(timeAt(clampedX));
        return;
    case Target::Strip: {
        const qint64 at = timeAt(clampedX);
        if (e->modifiers() & Qt::ShiftModifier) {
            // Shift+click moves the selection's nearer edge here (with none, from the playhead).
            const qint64 t = snap(at), frame = frameUs();
            qint64 a = selection_ ? selection_->start : snap(playhead_), b = selection_ ? selection_->end : snap(playhead_);
            if (!selection_ || std::abs(t - a) <= std::abs(t - b)) a = t; else b = t;
            if (a > b) std::swap(a, b);
            if (b - a < frame - 1) return;
            selection_ = recording::Span{a, b};
            selectedKind_.clear();
            selectedId_.clear();
            layoutCovers();
            update();
            emit timeSelected(a, b);
            return;
        }
        // Seeks on press; a drag past kSelectStart selects time from here.
        beginDrag(Drag::Strip, {}, pos);
        anchorUs_ = at;
        emit seekRequested(at);
        return;
    }
    case Target::Lane:
        // An empty lane seeks and clears the selection at once.
        clearSelections();
        emit seekRequested(timeAt(clampedX));
        emit selectionCleared();
        return;
    case Target::None: return;
    }
}
void FilmstripTimeline::mouseMoveEvent(QMouseEvent* e) {
    const auto pos = e->position();
    if (lastMove_) {
        recentMoves_.append(pos - *lastMove_);
        while (recentMoves_.size() > kIntentMoves) recentMoves_.removeFirst();
    }
    lastMove_ = pos;
    if (drag_ != Drag::None) { updateDrag(pos, e->modifiers()); return; }
    updateHover(pos);
    // A button still held after a cancelled drag: no skimming.
    if (e->buttons() != Qt::NoButton) { leaveSkimArea(); return; }
    hoverSkim(pos);
}
void FilmstripTimeline::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    if (drag_ == Drag::None) { updateHover(e->position()); return; }
    // A release outside the widget uses the clamped release position.
    updateDrag(e->position(), e->modifiers());
    autoscroll_.stop();
    const auto drag = std::exchange(drag_, Drag::None);
    const bool armed = std::exchange(armed_, false);
    const auto id = std::exchange(dragId_, QString());
    snapGuide_.reset();
    skimBlockedAt_ = e->position();
    update();
    switch (drag) {
    case Drag::TrimStart: case Drag::TrimEnd:
        emit edgeDragFinished();
        if (armed && (dragStart_ != originStart_ || dragEnd_ != originEnd_)) emit trimProposed(dragStart_, dragEnd_);
        break;
    case Drag::CutStart: case Drag::CutEnd:
        emit edgeDragFinished();
        if (armed && (dragStart_ != originStart_ || dragEnd_ != originEnd_)) emit cutAdjusted(id, dragStart_, dragEnd_);
        break;
    case Drag::CoverStart: case Drag::CoverEnd:
        emit edgeDragFinished();
        if (armed && (dragStart_ != originStart_ || dragEnd_ != originEnd_)) emit coverRetimed(id, dragStart_, dragEnd_);
        break;
    case Drag::CoverSlide:
        // A click on a bar only selected it; a slide is one retime on release.
        if (!armed) break;
        emit edgeDragFinished();
        if (dragStart_ != originStart_ || dragEnd_ != originEnd_) emit coverRetimed(id, dragStart_, dragEnd_);
        break;
    case Drag::SelectionStart: case Drag::SelectionEnd:
        emit edgeDragFinished();
        if (armed && (dragStart_ != originStart_ || dragEnd_ != originEnd_)) {
            selection_ = recording::Span{dragStart_, dragEnd_};
            emit timeSelected(dragStart_, dragEnd_);
        }
        break;
    case Drag::Strip:
        // A click on the strip: the press sought; it clears whatever is selected.
        clearSelections();
        emit selectionCleared();
        break;
    case Drag::Select:
        emit edgeDragFinished();
        if (dragEnd_ - dragStart_ >= frameUs() - 1) {
            selection_ = recording::Span{dragStart_, dragEnd_};
            selectedKind_.clear();
            selectedId_.clear();
            layoutCovers();
            emit timeSelected(dragStart_, dragEnd_);
        } else {
            clearSelections();
            emit selectionCleared();
        }
        break;
    case Drag::Scrub: case Drag::Playhead: case Drag::Overview: case Drag::None: break;
    }
    updateHover(e->position());
}
void FilmstripTimeline::leaveEvent(QEvent* e) {
    if (drag_ == Drag::None) {
        leaveSkimArea();
        setHover({});
        lastMove_.reset();
        recentMoves_.clear();
        skimBlockedAt_.reset();
    }
    QWidget::leaveEvent(e);
}

// ---- keyboard, wheel, pinch ----
void FilmstripTimeline::keyPressEvent(QKeyEvent* e) {
    // Esc during a drag cancels it (A6); every other key is the window's.
    if (drag_ != Drag::None && e->key() == Qt::Key_Escape) { cancelDrag(); e->accept(); return; }
    QWidget::keyPressEvent(e);
}
void FilmstripTimeline::wheelEvent(QWheelEvent* e) {
    const QPoint pixels = e->pixelDelta(), angle = e->angleDelta();
    const auto dominant = [](QPoint d) { return d.x() != 0 && std::abs(d.x()) >= std::abs(d.y()) ? d.x() : d.y(); };
    const qint64 anchor = timeAt(e->position().x());
    const auto step = [&](int steps) {
        const auto levels = zoomLevels();
        int index = 0;
        for (int i = 0; i < levels.size(); ++i) if (levels[i] <= zoom_) index = i;
        setZoom(levels[std::clamp(index + steps, 0, int(levels.size()) - 1)], anchor);
    };
    if (e->modifiers() & Qt::ControlModifier) {
        // Zoom at the pointer: every wheel notch is a level; a trackpad's pixels accumulate.
        if (!pixels.isNull()) {
            pixelZoom_ += dominant(pixels);
            const int steps = int(pixelZoom_ / kPixelZoomStep);
            if (steps != 0) { pixelZoom_ -= steps * kPixelZoomStep; step(steps); }
        } else {
            wheelRemainder_ += dominant(angle);
            const int notches = wheelRemainder_ / 120;
            if (notches != 0) { wheelRemainder_ -= notches * 120; step(notches); }
        }
        e->accept();
        return;
    }
    if (zoom_ <= 1) { e->ignore(); return; }
    // Above Fit, the wheel and Shift+wheel scroll a tenth of the visible span a notch; a trackpad scrolls by its pixels.
    if (!pixels.isNull()) {
        const int d = dominant(pixels);
        if (d == 0) { e->ignore(); return; }
        setScroll(scrollUs_ - qint64(std::llround(d * usPerPixel())));
    } else {
        const int d = dominant(angle);
        if (d == 0) { e->ignore(); return; }
        setScroll(scrollUs_ - qint64(double(d) / 120.0 * double(visibleLength()) * 0.1));
    }
    e->accept();
}
bool FilmstripTimeline::event(QEvent* e) {
    if (e->type() == QEvent::NativeGesture) {
        // Pinch zooms at the pointer, snapping the running scale to the ladder.
        const auto* gesture = static_cast<QNativeGestureEvent*>(e);
        if (gesture->gestureType() == Qt::BeginNativeGesture) { pinchFrom_ = zoom_; pinchScale_ = 1; return true; }
        if (gesture->gestureType() == Qt::EndNativeGesture) { pinchFrom_ = 0; return true; }
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            if (pinchFrom_ <= 0) { pinchFrom_ = zoom_; pinchScale_ = 1; }
            pinchScale_ = std::clamp(pinchScale_ * (1 + gesture->value()), 1.0 / 64, double(1 << 20));
            const double wanted = pinchFrom_ * pinchScale_;
            int best = 1;
            for (const int level : zoomLevels()) if (std::abs(std::log(level / wanted)) < std::abs(std::log(best / wanted))) best = level;
            setZoom(best, timeAt(gesture->position().x()));
            return true;
        }
    }
    return QWidget::event(e);
}
void FilmstripTimeline::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    setScroll(scrollUs_);
    emit visibleSpanChanged();
}
}
