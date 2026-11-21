#include "FilmstripTimeline.h"
#include "Duration.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QAccessible>
#include <QAccessibleWidget>
#include <QFontMetrics>
#include <QJsonArray>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QResizeEvent>
#include <QScopeGuard>
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
constexpr int kOverviewHeight = 8; // the overview track, shown above Fit (T9)
constexpr int kOverviewHit = 16;   // its hit height
constexpr int kOverviewThumb = 24; // its thumb's least width
constexpr int kBottom = 4;
constexpr int kBracketWidth = 13;  // a trim bracket's body, outside the kept range
constexpr int kBracketArm = 16;    // its arms along the strip's top and bottom
constexpr int kTilesAtFit = 12;    // across the default strip width
constexpr int kDefaultStrip = 1120;
constexpr int kMaxTiles = 24;
// Pointer model (feel spec A2-A4, A7, A9, A12, B1; round 2 T1-T17).
constexpr int kTrimPad = 8;        // a trim bracket's zone past its body, both sides
constexpr int kEdgeZone = 8;       // a cut or cover edge's zone, each side
constexpr int kBodyKeep = 8;       // a narrow item's body keeps this much between its edge zones
constexpr int kMinItemHit = 18;    // items narrower than this get this hit width
constexpr int kNarrowItem = 20;    // narrower items: a press selects; edges only once selected (T11)
constexpr int kNarrowEdgeGap = 8;  // a selected narrow item's edge targets sit at least this far apart
constexpr int kPlayheadCap = 24;   // the playhead's zone on the ruler (T1)
constexpr int kSelectionZone = 6;  // a time selection edge's zone each side
constexpr int kDeadZone = 4;       // edge, bracket, selected-bar and selection-edge drags start past it (T5)
constexpr int kScrubDeadZone = 3;  // a ruler or cap press turns into a scrub past it
constexpr int kFirstPressSlide = 8;// a bar selected by this press slides only past it (T5)
constexpr int kSelectStart = 6;    // a press on empty strip turns into a time selection past it (T6)
constexpr int kSkimClickMove = 2;  // no skim after a click until the pointer moves this far
constexpr int kSnapReach = 6;      // snapping reach in screen pixels at every zoom (R16t)
constexpr int kSnapRelease = 3;    // a held snap lets go this far past its reach
constexpr int kAutoscrollZone = 12;      // T9: the inner zone
constexpr int kAutoscrollPast = 60;      // full speed this far past the strip end
constexpr int kAutoscrollDelayMs = 120;  // entry waits this long
constexpr int kAutoscrollMs = 16;
constexpr int kSkimRestMs = 40;
constexpr int kSkimRestEveryMs = 50;     // skimRested at most this often while moving (T8)
constexpr int kSkimIntentPx = 12;        // Q3: net horizontal travel that is intent
constexpr int kSkimDwellMs = 150;        // or this long without moving
constexpr int kSkimDwellSlop = 3;        // "without moving": within this many px
constexpr int kSkimVertical = 8;         // vertical departure that ends skimming
constexpr int kPendingMs = 1000;         // T4: a released drag waits this long for its answer
constexpr int kEaseBackMs = 120;         // then eases back over this
constexpr int kAxisLatch = 6;            // T12: trackpad pixels before the scroll axis latches
constexpr int kGestureIdleMs = 150;      // a trackpad gesture ends after this idle
constexpr int kZoomAnimationMs = 120;    // R5: a zoom step eases over this
constexpr int kCutEdgeOutside = 4;       // R7: an unselected cut's edge zone outside the cut
constexpr int kNarrowOutside = 10;       // R6: a selected narrow item's edge zone outside it...
constexpr int kNarrowInside = 2;         // ...and inside it
constexpr int kSelectedGrip = 5;         // R11: a selected item's grip wins within this of its edge
constexpr int kPlayheadClickSlop = 2;    // R3: a cap click farther than this from the line seeks
constexpr int kLateGripMs = 60;          // Q10t, P1: a press this soon after leaving a grip...
constexpr int kLateGripPx = 8;           // ...and this close to where it left takes that grip
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
// A change in whole frames: "+3 f", "−2 f".
QString frameDeltaText(qint64 us, qint64 frame) {
    const qint64 n = (std::abs(us) + frame / 2) / frame;
    return QStringLiteral("%1%2 f").arg(us < 0 && n ? QStringLiteral("−") : QStringLiteral("+")).arg(n);
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
    connect(&skimRest_, &QTimer::timeout, this, &FilmstripTimeline::emitRested);
    restThrottle_.setSingleShot(true);
    connect(&restThrottle_, &QTimer::timeout, this, &FilmstripTimeline::emitRested);
    dwellTimer_.setSingleShot(true);
    dwellTimer_.setInterval(kSkimDwellMs);
    connect(&dwellTimer_, &QTimer::timeout, this, [this] {
        if (drag_ != Drag::None || !inSkimArea_ || playing_ || !lastMove_ || skimBlockedAt_) return;
        // Resting on a grip ends a skim (R9); dwelling elsewhere is intent (T8).
        if (skimIntent_) { if (onGrip(hover_.target)) disengageSkim(*lastMove_); return; }
        // After a press or release in the timeline only travel is intent (P4).
        if (!onGrip(hover_.target) && !travelOnly_) engageSkim();
    });
    pendingTimer_.setSingleShot(true);
    pendingTimer_.setInterval(kPendingMs);
    connect(&pendingTimer_, &QTimer::timeout, this, &FilmstripTimeline::easePendingBack);
    easeTimer_.setInterval(16);
    connect(&easeTimer_, &QTimer::timeout, this, [this] {
        if (!pending_.easing || easeClock_.elapsed() >= kEaseBackMs) { clearPending(); return; }
        update();
    });
    zoomAnimation_.setInterval(16);
    connect(&zoomAnimation_, &QTimer::timeout, this, [this] {
        const double t = std::clamp(zoomClock_.elapsed() / double(kZoomAnimationMs), 0.0, 1.0), e = 1 - std::pow(1 - t, 3);
        if (t >= 1) { finishZoom(); return; }
        // One geometric progress for both: the anchor keeps its screen fraction (Q2).
        liveLength_ = fromLength_ * std::pow(double(visibleLength()) / fromLength_, e);
        liveStart_ = std::clamp(double(animAnchor_) - animFraction_ * liveLength_, 0.0, std::max(0.0, double(duration_) - liveLength_));
        update();
    });
    zoomSettle_.setSingleShot(true);
    zoomSettle_.setInterval(kGestureIdleMs);
    connect(&zoomSettle_, &QTimer::timeout, this, &FilmstripTimeline::settleContinuousZoom);
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
    // A bracket, cut edge or cover whose item changed under the drag would
    // write over the newer edit on release: that drag ends here, proposing
    // nothing. An edit that leaves the dragged item alone keeps the drag (T7).
    bool cancelDrag = false;
    if (edgeDrag() && edit.revision != edit_.revision) {
        if (drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd) {
            cancelDrag = edit.trimStart != originStart_ || edit.exportEnd(duration_) != originEnd_;
        } else if (drag_ == Drag::CutStart || drag_ == Drag::CutEnd) {
            const auto it = std::find_if(edit.cuts.cbegin(), edit.cuts.cend(), [this](const RecordingCut& c) { return c.id == dragId_; });
            cancelDrag = it == edit.cuts.cend() || it->start != originStart_ || it->end != originEnd_;
        } else {
            const auto it = std::find_if(edit.masks.cbegin(), edit.masks.cend(), [this](const RecordingMask& m) { return m.id == dragId_; });
            cancelDrag = it == edit.masks.cend() || it->start != originStart_ || it->end != originEnd_;
        }
    }
    // A released drag's pending span (T4): a new revision answers it; the same
    // revision again is a refusal, which eases back.
    if (pending_.drag != Drag::None && !pending_.easing) {
        if (edit.revision != pending_.revision) clearPending();
        else { edit_ = edit; easePendingBack(); }
    }
    edit_ = edit;
    if (cancelDrag) {
        drag_ = Drag::None;
        armed_ = false;
        dragId_.clear();
        snapGuide_.reset();
        snapLock_.reset();
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
    // Starting playback again resumes following the playhead (T18).
    if (playing_) { followSuspended_ = false; leaveSkimArea(); }
    if (drag_ == Drag::None && lastMove_) updateHover(*lastMove_);
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
    // The lanes stay as they are; only a hidden cover being selected is promoted (R13).
    if (kind == "cover") for (int i = 0; i < edit_.masks.size(); ++i) if (edit_.masks[i].id == id && coverLane_.value(i, -1) < 0) layoutCovers();
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
    if (!on) {
        const auto at = skim_;
        clearSkim();
        if (at && inSkimArea_) hoverLine_ = at;  // the hairline and its time stay
    } else if (hoverLine_) {
        const auto at = *hoverLine_;
        hoverLine_.reset();
        skimAt(at);
    }
    update();
    emit skimmingChanged(on);
}
void FilmstripTimeline::skimAt(std::optional<qint64> sourceUs) {
    if (!sourceUs) {
        // An explicit end (every seek, a key step included): the skim needs
        // fresh intent; a 1-2 px jiggle does not bring it back (Q4).
        hoverLine_.reset();
        clearSkim();
        skimIntent_ = false;
        dwellTimer_.stop();
        if (lastMove_) intentAnchor_ = dwellAt_ = *lastMove_;
        update();
        return;
    }
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
    // The exact frame is asked for while moving too: at most every 50 ms,
    // the latest time winning, and again when the pointer rests (T8).
    if (!restedClock_.isValid() || restedClock_.elapsed() >= kSkimRestEveryMs) emitRested();
    else if (!restThrottle_.isActive()) restThrottle_.start(int(kSkimRestEveryMs - restedClock_.elapsed()));
    restartSkimTimer();
}
void FilmstripTimeline::emitRested() {
    if (!skim_ || lastRested_ == skim_) return;
    lastRested_ = skim_;
    restedClock_.restart();
    emit skimRested(*skim_);
}
void FilmstripTimeline::setLinkedHover(const QString& kind, const QString& id) {
    const QString k = kind == "cut" || kind == "cover" ? kind : QString(), i = k.isEmpty() ? QString() : id;
    if (k == linkedKind_ && i == linkedId_) return;
    linkedKind_ = k;
    linkedId_ = i;
    update();
}
void FilmstripTimeline::setSkimExact(bool exact) { if (skimExact_ != exact) { skimExact_ = exact; update(); } }
void FilmstripTimeline::markSelectionEdge(bool start) {
    const qint64 at = snap(playhead_), frame = frameUs();
    // Always the time selection, whatever is selected (Q7): covers use
    // Shift+I / Shift+O and cut edges are dragged.
    // The selection's start or end at the playhead; with none, it runs to the kept footage's other end.
    qint64 a = selection_ ? selection_->start : edit_.trimStart, b = selection_ ? selection_->end : edit_.exportEnd(duration_);
    if (start) { a = at; if (b - a < frame - 1) b = std::min(duration_, snap(a + frame)); }
    else { b = at; if (b - a < frame - 1) a = std::max<qint64>(0, snap(b - frame)); }
    if (b - a < frame - 1) return;
    selection_ = recording::Span{a, b};
    selectedKind_.clear();
    selectedId_.clear();
    update();
    emit timeSelected(a, b);
}

// ---- what is shown: live drag, pending answer, or the edit (T4) ----
recording::Span FilmstripTimeline::shownTrim() const {
    if (drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd) return {dragStart_, dragEnd_};
    const recording::Span committed{edit_.trimStart, edit_.exportEnd(duration_)};
    if (pending_.drag == Drag::TrimStart || pending_.drag == Drag::TrimEnd) {
        if (!pending_.easing) return {pending_.start, pending_.end};
        const double t = std::clamp(easeClock_.elapsed() / double(kEaseBackMs), 0.0, 1.0), k = 1 - (1 - t) * (1 - t);
        return {pending_.start + qint64(std::llround((committed.start - pending_.start) * k)), pending_.end + qint64(std::llround((committed.end - pending_.end) * k))};
    }
    return committed;
}
recording::Span FilmstripTimeline::shownCut(const RecordingCut& cut) const {
    if ((drag_ == Drag::CutStart || drag_ == Drag::CutEnd) && dragId_ == cut.id) return {dragStart_, dragEnd_};
    if ((pending_.drag == Drag::CutStart || pending_.drag == Drag::CutEnd) && pending_.id == cut.id) {
        if (!pending_.easing) return {pending_.start, pending_.end};
        const double t = std::clamp(easeClock_.elapsed() / double(kEaseBackMs), 0.0, 1.0), k = 1 - (1 - t) * (1 - t);
        return {pending_.start + qint64(std::llround((cut.start - pending_.start) * k)), pending_.end + qint64(std::llround((cut.end - pending_.end) * k))};
    }
    return {cut.start, cut.end};
}
recording::Span FilmstripTimeline::shownCover(const RecordingMask& mask) const {
    if ((drag_ == Drag::CoverStart || drag_ == Drag::CoverEnd || drag_ == Drag::CoverSlide) && dragId_ == mask.id) return {dragStart_, dragEnd_};
    if ((pending_.drag == Drag::CoverStart || pending_.drag == Drag::CoverEnd || pending_.drag == Drag::CoverSlide) && pending_.id == mask.id) {
        if (!pending_.easing) return {pending_.start, pending_.end};
        const double t = std::clamp(easeClock_.elapsed() / double(kEaseBackMs), 0.0, 1.0), k = 1 - (1 - t) * (1 - t);
        return {pending_.start + qint64(std::llround((mask.start - pending_.start) * k)), pending_.end + qint64(std::llround((mask.end - pending_.end) * k))};
    }
    return {mask.start, mask.end};
}
// A proposal is out: paint the item where it was dropped until the edit
// answers (a new revision), a refusal arrives (the same revision again) or
// kPendingMs passes; the last two ease back.
void FilmstripTimeline::setPending(Drag drag, const QString& id, qint64 start, qint64 end) {
    easeTimer_.stop();
    pending_ = {drag, id, start, end, 0, 0, edit_.revision, false};
    pendingTimer_.start();
    update();
}
void FilmstripTimeline::easePendingBack() {
    if (pending_.drag == Drag::None || pending_.easing) return;
    pendingTimer_.stop();
    pending_.easing = true;
    easeClock_.start();
    easeTimer_.start();
    update();
}
void FilmstripTimeline::clearPending() {
    if (pending_.drag == Drag::None) return;
    pendingTimer_.stop();
    easeTimer_.stop();
    pending_ = {};
    update();
}

// ---- time and frames ----
qint64 FilmstripTimeline::frameUs() const { return std::max<qint64>(1, qRound64(1e6 / fps_)); }
qint64 FilmstripTimeline::snap(qint64 us) const {
    const qint64 n = qRound64(double(std::clamp<qint64>(us, 0, duration_)) * fps_ / 1e6);
    return std::min(duration_, qRound64(double(n) * 1e6 / fps_));
}
qint64 FilmstripTimeline::visibleLength() const { return std::max<qint64>(1, duration_ / zoom_); }
double FilmstripTimeline::usPerPixel() const { return double(visibleLength()) / stripWidth(); }
recording::Span FilmstripTimeline::visibleSpan() const {
    if (paintLive_ && zoomLive()) { const qint64 a = qint64(std::floor(liveStart_)); return {a, std::min(duration_, qint64(std::ceil(liveStart_ + liveLength_)))}; }
    return {scrollUs_, std::min(duration_, scrollUs_ + visibleLength())};
}
// What paints: the live view during a zoom animation or a pinch, else the model's.
double FilmstripTimeline::viewStart() const { return paintLive_ && zoomLive() ? liveStart_ : double(scrollUs_); }
double FilmstripTimeline::viewLength() const { return paintLive_ && zoomLive() ? liveLength_ : double(visibleLength()); }
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
    if (right - left < kOverviewThumb) {
        const int mid = (left + right) / 2;
        left = std::max(track.left(), mid - kOverviewThumb / 2);
        right = std::min(track.right() + 1, left + kOverviewThumb);
        left = right - kOverviewThumb;
    }
    return QRect(left, track.top(), right - left, track.height());
}
int FilmstripTimeline::xFor(qint64 sourceUs) const {
    const double x = stripLeft() + (double(sourceUs) - viewStart()) * stripWidth() / viewLength();
    return int(std::lround(std::clamp(x, -1e6, 1e6)));
}
qint64 FilmstripTimeline::timeAt(qreal x) const {
    const double t = viewStart() + (x - stripLeft()) * viewLength() / stripWidth();
    return std::clamp<qint64>(qint64(std::llround(t)), 0, duration_);
}
void FilmstripTimeline::setScroll(qint64 startUs) {
    if (zoomLive() && !continuous_) finishZoom();
    const auto clamped = std::clamp<qint64>(startUs, 0, std::max<qint64>(0, duration_ - visibleLength()));
    if (clamped == scrollUs_) return;
    scrollUs_ = clamped;
    viewChanged();
    update();
    emit visibleSpanChanged();
}
// After a scroll or zoom: the lanes (the "+n" chip's space depends on the
// view), the hover under a still pointer, and the skim time under it (T8).
void FilmstripTimeline::viewChanged() {
    layoutCovers();
    if (drag_ != Drag::None || !lastMove_) return;
    updateHover(*lastMove_);
    if (skim_) skimAt(timeAt(lastMove_->x()));
    else if (hoverLine_) { hoverLine_ = std::clamp<qint64>(timeAt(lastMove_->x()), 0, std::max<qint64>(0, duration_ - 1)); update(); }
}
void FilmstripTimeline::revealPlayhead() {
    if (zoom_ <= 1) return;
    const auto span = visibleSpan();
    const qint64 len = visibleLength();
    // A manual scroll while playing suspends following until the playhead is back in view (T18).
    if (playing_ && followSuspended_) {
        if (playhead_ < span.start || playhead_ > span.end) return;
        followSuspended_ = false;
    }
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
    if (next == zoom_ && !zoomLive()) return;
    const auto before = visibleSpan();
    // Without an anchor (keys, menu): the playhead when it is on screen, else the view's centre (R4).
    const bool playheadShown = playhead_ >= before.start && playhead_ <= before.end;
    const qint64 anchor = std::clamp<qint64>(anchorUs.value_or(playheadShown ? playhead_ : before.start + visibleLength() / 2), 0, duration_);
    // From what is painted (a running animation or pinch included); the anchor
    // keeps its x, and one off screen is held at the nearest edge.
    paintLive_ = true;
    const double fromStart = viewStart(), fromLength = viewLength();
    paintLive_ = false;
    const double fraction = std::clamp((double(anchor) - fromStart) / fromLength, 0.0, 1.0);
    const bool changed = next != zoom_;
    zoom_ = next;
    scrollUs_ = std::clamp<qint64>(anchor - qint64(std::llround(fraction * double(visibleLength()))), 0, std::max<qint64>(0, duration_ - visibleLength()));
    startZoomAnimation(fromStart, fromLength, anchor, fraction);
    viewChanged();
    update();
    if (changed) emit zoomChanged(zoom_);
    emit visibleSpanChanged();
}
// R5: the painted view eases from where it was to the model's over 120 ms
// (length geometrically, start linearly); tiles are asked for the model's span
// once, not per frame. A press, a scroll or a hidden widget ends it at once.
void FilmstripTimeline::startZoomAnimation(double fromStart, double fromLength, qint64 anchor, double fraction) {
    continuous_ = false;
    zoomSettle_.stop();
    if (!isVisible() || (std::abs(fromLength - double(visibleLength())) < 1 && std::abs(fromStart - double(scrollUs_)) < 1)) { finishZoom(); return; }
    fromStart_ = liveStart_ = fromStart;
    fromLength_ = liveLength_ = fromLength;
    animAnchor_ = anchor;
    animFraction_ = fraction;
    zoomClock_.start();
    zoomAnimation_.start();
}
void FilmstripTimeline::finishZoom() {
    zoomAnimation_.stop();
    zoomSettle_.stop();
    continuous_ = false;
    if (liveLength_ <= 0) return;
    liveLength_ = 0;
    update();
}
// R5: pinch and Ctrl+trackpad zoom continuously about the anchor, then settle
// to the nearest ladder level when the gesture ends.
void FilmstripTimeline::continuousZoom(double factor, qint64 anchorUs) {
    if (!continuous_) {
        paintLive_ = true;
        liveStart_ = viewStart();
        liveLength_ = viewLength();
        paintLive_ = false;
        zoomAnimation_.stop();
        continuous_ = true;
        continuousZoom_ = double(duration_) / liveLength_;
    }
    continuousAnchor_ = anchorUs;
    continuousZoom_ = std::clamp(continuousZoom_ * factor, 1.0, double(maxZoom()));
    const double length = double(duration_) / continuousZoom_;
    const double fraction = std::clamp((double(anchorUs) - liveStart_) / liveLength_, 0.0, 1.0);
    liveLength_ = length;
    liveStart_ = std::clamp(double(anchorUs) - fraction * length, 0.0, std::max(0.0, double(duration_) - length));
    zoomSettle_.start();
    update();
}
void FilmstripTimeline::settleContinuousZoom() {
    if (!continuous_) return;
    continuous_ = false;
    zoomSettle_.stop();
    int best = 1;
    for (const int level : zoomLevels()) if (std::abs(std::log(level / continuousZoom_)) < std::abs(std::log(best / continuousZoom_))) best = level;
    setZoom(best, continuousAnchor_);
}
void FilmstripTimeline::zoomIn(std::optional<qint64> anchorUs) {
    for (const int level : zoomLevels()) if (level > zoom_) { setZoom(level, anchorUs); return; }
}
void FilmstripTimeline::zoomOut(std::optional<qint64> anchorUs) {
    const auto levels = zoomLevels();
    for (int i = int(levels.size()) - 1; i >= 0; --i) if (levels[i] < zoom_) { setZoom(levels[i], anchorUs); return; }
}
void FilmstripTimeline::zoomFit() { setZoom(1); }
void FilmstripTimeline::reveal(recording::Span span) {
    if (zoom_ <= 1) return;
    const auto view = visibleSpan();
    const qint64 len = visibleLength();
    const qint64 a = std::clamp<qint64>(std::min(span.start, span.end), 0, duration_), b = std::clamp<qint64>(std::max(span.start, span.end), 0, duration_);
    if (a >= view.start && b <= view.end) return;
    if (b - a > len || b < view.start || a > view.end) setScroll((a + b) / 2 - len / 2);  // setScroll emits visibleSpanChanged on a change
    else if (a < view.start) setScroll(a);
    else setScroll(b - len);
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
// Two lanes, first fit by start; the selected cover always gets a lane. With
// covers hidden, the "+n" chip takes the lower lane's right end: a bar there
// (or its edge zone) under the chip would be covered, so that lane stops short
// of it and such a cover counts as hidden too (T17).
void FilmstripTimeline::layoutCovers() {
    const auto& masks = edit_.masks;
    coverLane_ = QList<int>(masks.size(), -1);
    QList<int> order(masks.size());
    for (int i = 0; i < masks.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return masks[a].start < masks[b].start; });
    std::optional<qint64> blockedFrom;  // the lower lane is closed from here to the view's end
    const auto place = [&](int selectedFirst) {
        QList<QPair<qint64, qint64>> used[2];
        coverLane_.fill(-1);
        const qint64 viewEnd = visibleSpan().end;
        const auto tryPlace = [&](int i) {
            const qint64 a = masks[i].start, b = std::max(masks[i].end, a + 1);
            for (int lane = 0; lane < 2; ++lane) {
                if (lane == 1 && blockedFrom && a <= viewEnd && b > *blockedFrom) continue;
                const bool free = std::none_of(used[lane].cbegin(), used[lane].cend(), [&](const auto& u) { return a < u.second && u.first < b; });
                if (free) { used[lane].append({a, b}); coverLane_[i] = lane; return; }
            }
        };
        if (selectedFirst >= 0) tryPlace(selectedFirst);
        for (const int i : order) if (i != selectedFirst) tryPlace(i);
    };
    int selected = -1;
    if (selectedKind_ == "cover") for (int i = 0; i < masks.size(); ++i) if (masks[i].id == selectedId_) selected = i;
    const auto run = [&] {
        place(-1);
        if (selected >= 0 && coverLane_[selected] < 0) place(selected);
        hiddenCovers_ = int(std::count(coverLane_.cbegin(), coverLane_.cend(), -1));
    };
    run();
    // The chip's width follows its count; two passes settle it.
    for (int pass = 0; pass < 3 && hiddenCovers_ > 0; ++pass) {
        const auto chip = moreRect();
        const qint64 from = std::max<qint64>(0, timeAt(chip.left() - kEdgeZone - 2));
        if (blockedFrom == from) break;
        blockedFrom = from;
        run();
    }
    lanesUsed_ = 0;
    for (const int lane : coverLane_) if (lane >= 0) lanesUsed_ = std::max(lanesUsed_, lane + 1);
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
    // The gap between strip and lanes belongs to the nearer of the two (T17).
    const int gapMid = (strip.bottom() + 1 + metrics().lanesTop) / 2;
    const auto shown = [&](qint64 t) { return t >= span.start && t <= span.end; };
    const auto box = [](double l, double r, int top, int bottom) {
        const int a = int(std::floor(l)), b = int(std::ceil(r));
        return QRect(a, top, std::max(1, b - a), bottom - top);
    };
    // An item's body and edge zones. At kNarrowItem or wider: kEdgeZone each
    // side of each edge (only kCutEdgeOutside outside an unselected cut, R7),
    // shrunk toward the outside so the body keeps kBodyKeep. Narrower:
    // unselected, only its body (at least kMinItemHit wide), so a press selects
    // it (T11); selected, edges just outside the bar ([a−10, a+2], [b−2, b+10])
    // and the body between them, so it can still be slid (R6).
    const auto item = [&](double a, double b, int top, int bottom, Target startT, Target endT, Target bodyT, const QString& id, bool selected, bool startShown, bool endShown) {
        const double c = (a + b) / 2;
        if (b - a < kNarrowItem) {
            if (!selected) {
                const double half = std::max(double(kMinItemHit), b - a) / 2;
                list.append({bodyT, id, box(std::max(c - half, stripL), std::min(c + half, stripR), top, bottom), c, c});
                return;
            }
            if (b - a > 2 * kNarrowInside) list.append({bodyT, id, box(a + kNarrowInside, b - kNarrowInside, top, bottom), c, c});
            if (startShown) list.append({startT, id, box(a - kNarrowOutside, std::min(a + kNarrowInside, c), top, bottom), a, a});
            if (endShown) list.append({endT, id, box(std::max(b - kNarrowInside, c), b + kNarrowOutside, top, bottom), b, b});
            return;
        }
        const double outside = bodyT == Target::Cut && !selected ? kCutEdgeOutside : kEdgeZone;
        list.append({bodyT, id, box(std::max(a, stripL), std::min(b, stripR), top, bottom), c, c});
        if (startShown) list.append({startT, id, box(a - outside, std::min(a + kEdgeZone, c - kBodyKeep / 2.0), top, bottom), a, a});
        if (endShown) list.append({endT, id, box(std::max(b - kEdgeZone, c + kBodyKeep / 2.0), b + outside, top, bottom), b, b});
    };
    // Trim brackets (T1): the 13 px body plus kTrimPad both sides, from 3 px
    // above the strip to 4 px below it, and both painted arms. Not the ruler.
    const int trimTop = strip.top() - 3, trimBottom = strip.bottom() + 1 + 4;
    const auto trim = shownTrim();
    if (shown(trim.start)) {
        const double x = xFor(trim.start);
        list.append({Target::TrimStart, {}, box(x - kBracketWidth - kTrimPad, x + kTrimPad, trimTop, trimBottom), x - kBracketWidth, x});
        list.append({Target::TrimStart, {}, box(x, x + kBracketArm, trimTop, strip.top() + 1), x - kBracketWidth, x});
        list.append({Target::TrimStart, {}, box(x, x + kBracketArm, strip.bottom(), trimBottom), x - kBracketWidth, x});
    }
    if (shown(trim.end)) {
        const double x = xFor(trim.end);
        list.append({Target::TrimEnd, {}, box(x - kTrimPad, x + kBracketWidth + kTrimPad, trimTop, trimBottom), x, x + kBracketWidth});
        list.append({Target::TrimEnd, {}, box(x - kBracketArm, x, trimTop, strip.top() + 1), x, x + kBracketWidth});
        list.append({Target::TrimEnd, {}, box(x - kBracketArm, x, strip.bottom(), trimBottom), x, x + kBracketWidth});
    }
    // The playhead is grabbed on the ruler only: its cap's 24 px over the whole
    // ruler (T1, T2). On the strip a press seeks or selects, even on its line.
    if (shown(playhead_)) {
        const double x = xFor(playhead_);
        list.append({Target::Playhead, {}, box(x - kPlayheadCap / 2.0, x + kPlayheadCap / 2.0, 0, strip.top()), x, x});
    }
    if (selection_ && !selection_->empty()) {
        for (const bool start : {true, false}) {
            const qint64 t = start ? selection_->start : selection_->end;
            if (!shown(t)) continue;
            const double x = xFor(t);
            list.append({start ? Target::SelectionStart : Target::SelectionEnd, {}, box(x - kSelectionZone, x + kSelectionZone, strip.top() - 3, gapMid), x, x});
        }
    }
    for (const auto& cut : edit_.cuts) {
        const auto live = shownCut(cut);
        if (live.end < span.start || live.start > span.end) continue;
        const bool selected = selectedKind_ == "cut" && selectedId_ == cut.id;
        item(xFor(live.start), xFor(live.end), strip.top(), gapMid, Target::CutStart, Target::CutEnd, Target::Cut, cut.id, selected, shown(live.start), shown(live.end));
    }
    // A bar's hit area is its whole lane row (the upper lane's reaches up to the gap's middle).
    const auto lanes = coversRect();
    for (int i = 0; i < edit_.masks.size(); ++i) {
        const int lane = coverLane_.value(i, -1);
        if (lane < 0) continue;
        const auto& mask = edit_.masks[i];
        const auto live = shownCover(mask);
        if (live.end < span.start || live.start > span.end) continue;
        const auto row = laneRect(lane);
        const int top = lane == 0 ? gapMid : row.top(), bottom = lane == 1 ? lanes.bottom() + 1 : row.bottom() + 1;
        const double a = xFor(live.start), b = std::max<double>(xFor(live.end), a + 3);
        const bool selected = selectedKind_ == "cover" && selectedId_ == mask.id;
        item(a, b, top, bottom, Target::CoverStart, Target::CoverEnd, Target::Cover, mask.id, selected, shown(live.start), shown(live.end));
    }
    if (const auto more = moreRect(); !more.isNull()) list.append({Target::More, {}, more, 0, 0});
    if (const auto track = overviewRect(); !track.isNull()) {
        const int grow = (kOverviewHit - track.height()) / 2;
        list.append({Target::Overview, {}, track.adjusted(0, -grow, 0, grow), 0, 0});
    }
    return list;
}
FilmstripTimeline::Hit FilmstripTimeline::hitAt(QPointF pos) const {
    const auto list = zones();
    const auto inside = [&](const Zone& z) { return QRectF(z.rect).contains(pos); };
    const auto hit = [](const Zone& z) { return Hit{z.target, z.id, z.rect}; };
    // The "+n" chip owns its reserved space.
    for (const auto& z : list) if (z.target == Target::More && inside(z)) return hit(z);
    // (1) The nearest edge within its zone; overlapping zones split at the
    // midpoint. A selected cut's grip wins over a bracket body under it (R11).
    // Grips tied within 1 px (coincident edges, R17t): the selected item's
    // wins, else the one whose item lies on the pointer's side of the seam.
    const auto selectedGrip = [this](const Zone& z) {
        const bool cut = z.target == Target::CutStart || z.target == Target::CutEnd, cover = z.target == Target::CoverStart || z.target == Target::CoverEnd;
        const bool trim = z.target == Target::TrimStart || z.target == Target::TrimEnd;
        return (cut && selectedKind_ == "cut" && selectedId_ == z.id) || (cover && selectedKind_ == "cover" && selectedId_ == z.id) || (trim && selectedKind_ == "trim");
    };
    // The edge itself and the side its item lies on (-1 left, 1 right).
    const auto seam = [](const Zone& z) { return z.target == Target::TrimStart ? z.edgeX2 : z.edgeX; };
    const auto itemSide = [](Target t) { return t == Target::TrimStart || t == Target::CutEnd || t == Target::CoverEnd || t == Target::SelectionEnd ? -1 : 1; };
    QList<std::pair<const Zone*, double>> grips;
    for (const auto& z : list) {
        if (!onGrip(z.target) || !inside(z)) continue;
        double d = pos.x() < z.edgeX ? z.edgeX - pos.x() : pos.x() > z.edgeX2 ? pos.x() - z.edgeX2 : 0.0;
        if (selectedGrip(z) && (z.target == Target::CutStart || z.target == Target::CutEnd) && std::abs(pos.x() - seam(z)) <= kSelectedGrip) d = -1;
        grips.append({&z, d});
    }
    if (!grips.isEmpty()) {
        const double nearest = std::min_element(grips.cbegin(), grips.cend(), [](const auto& l, const auto& r) { return l.second < r.second; })->second;
        const Zone* best = nullptr;
        for (const auto& [z, d] : grips) {
            if (d > nearest + 1) continue;
            if (!best) { best = z; continue; }
            if (selectedGrip(*z) != selectedGrip(*best)) { if (selectedGrip(*z)) best = z; continue; }
            const int side = pos.x() < seam(*z) ? -1 : 1;
            if (itemSide(z->target) == side && itemSide(best->target) != side) best = z;
        }
        return hit(*best);
    }
    // (2) The playhead's cap, (3) a cut or cover body, then the overview track.
    for (const auto& z : list) if (z.target == Target::Playhead && inside(z)) return hit(z);
    for (const auto& z : list) if ((z.target == Target::Cut || z.target == Target::Cover) && inside(z)) return hit(z);
    for (const auto& z : list) if (z.target == Target::Overview && inside(z)) return hit(z);
    // (4) Empty ruler, strip or lane; the gap between strip and lanes splits at its middle.
    const auto strip = stripRect();
    const int gapMid = (strip.bottom() + 1 + metrics().lanesTop) / 2;
    const auto lanes = coversRect();
    if (pos.y() >= 0 && pos.y() < strip.top()) return {Target::Ruler, {}, {}};
    if (pos.y() >= strip.top() && pos.y() < gapMid) return {Target::Strip, {}, {}};
    if (pos.y() >= gapMid && pos.y() <= lanes.bottom() + 1 && pos.x() >= 0 && pos.x() < width()) return {Target::Lane, {}, {}};
    return {};
}
// Edges and brackets: the targets that are grips (they win by nearness, and skimming stops over them).
bool FilmstripTimeline::onGrip(Target target) const {
    switch (target) {
    case Target::TrimStart: case Target::TrimEnd: case Target::SelectionStart: case Target::SelectionEnd:
    case Target::CutStart: case Target::CutEnd: case Target::CoverStart: case Target::CoverEnd: return true;
    default: return false;
    }
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
    QJsonValue pending = QJsonValue::Null;
    if (pending_.drag != Drag::None && !pending_.easing) {
        const bool trim = pending_.drag == Drag::TrimStart || pending_.drag == Drag::TrimEnd, cut = pending_.drag == Drag::CutStart || pending_.drag == Drag::CutEnd;
        pending = QJsonObject{{"kind", trim ? "trim" : cut ? "cut" : "cover"}, {"id", pending_.id}, {"start", double(pending_.start)}, {"end", double(pending_.end)}};
    }
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
            {"pending", pending},
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
        // A target made of several rectangles (a bracket and its arms) is one part.
        if (!kind.isEmpty() && !list.isEmpty() && list.last().kind == kind && list.last().id == z.id) { list.last().rect |= z.rect; continue; }
        QString name;
        switch (z.target) {
        case Target::TrimStart: name = QStringLiteral("Trim start, %1").arg(clock(edit_.trimStart)); break;
        case Target::TrimEnd: name = QStringLiteral("Trim end, %1").arg(clock(trimEnd)); break;
        case Target::Playhead: name = QStringLiteral("Playhead, %1").arg(clock(playhead_)); break;
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
    // Paint the live view while a zoom animates or a pinch is under way (R5).
    paintLive_ = true;
    const auto liveGuard = qScopeGuard([this] { paintLive_ = false; });
    const auto strip = stripRect();
    const auto span = visibleSpan();
    // Live drags and released ones awaiting their answer paint where they are (T4).
    const auto trimShown = shownTrim();
    const qint64 trimA = trimShown.start, trimB = trimShown.end;
    const auto liveCut = [&](const RecordingCut& cut) { return shownCut(cut); };
    const auto liveCover = [&](const RecordingMask& mask) { return shownCover(mask); };
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
        const double pxPerSecond = double(strip.width()) / (viewLength() / 1e6);
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
        if (hovered(Target::Cut, cut.id) || (linkedKind_ == "cut" && linkedId_ == cut.id)) p.fillRect(QRectF(a, strip.top(), b - a, strip.height()), c.lift);
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
                const bool lit = hovered(Target::Cover, mask.id) || (drag_ == Drag::CoverSlide && dragId_ == mask.id) || (linkedKind_ == "cover" && linkedId_ == mask.id);
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

    // The playhead, then a selected cut's grips above it.
    if (playhead_ >= span.start && playhead_ <= span.end) {
        const bool lit = hovered(Target::Playhead) || drag_ == Drag::Playhead;
        const double x = xAt(playhead_), line = lit ? 3 : 2;
        // Down through the Covers track (T19).
        const double end = coversRect().bottom() + 1;
        p.setPen(Qt::NoPen);
        p.setBrush(c.playOutline);
        p.drawRect(QRectF(x - line / 2 - 1, kTop + 4, line + 2, end - kTop - 4));
        p.setBrush(c.play);
        p.drawRect(QRectF(x - line / 2, kTop + 4, line, end - kTop - 4));
        const QPolygonF cap{QPointF(x - 7, kTop), QPointF(x + 7, kTop), QPointF(x, kTop + 8)};
        p.setPen(QPen(c.playOutline, 1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawPolygon(cap);
    }
    // A selected cut's grips, above the playhead line (R1).
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

    // The snap guide (A7): 1 px accent from the ruler to the lanes while snapped.
    if (snapGuide_ && armed_ && drag_ != Drag::None && *snapGuide_ >= span.start && *snapGuide_ <= span.end) {
        const auto lanes = coversRect();
        p.fillRect(QRectF(xAt(*snapGuide_), kTop, 1, lanes.bottom() + 1 - kTop), c.accent);
    }

    // The drag readout (A8, T16), in the skim flag's style, its width reserved
    // from the widest text so it never jitters. Above the strip beside the
    // dragged edge (never over it); over the playhead while scrubbing; just
    // above the Covers track near the pointer for a cover's edge or slide.
    if (armed_ && drag_ != Drag::None && drag_ != Drag::Overview) {
        QString text, widestText;
        double x = 0, y = kTop - 2;
        int side = 0;  // -1: the flag ends left of x, 1: it starts right of it, 0: centred
        const QString longest = clock(duration_);
        // Frames once a frame spans 2 px or more, tenths of a second otherwise (T10).
        const bool frames = double(frameUs()) / usPerPixel() >= 2;
        const auto change = [&](qint64 us) { return frames ? frameDeltaText(us, frameUs()) : deltaText(us); };
        const QString widestChange = frames ? QStringLiteral("−%1 f").arg(duration_ / frameUs()) : QStringLiteral("−") + deltaText(duration_).mid(1);
        switch (drag_) {
        case Drag::Scrub: case Drag::Playhead:
            text = clock(playhead_);
            widestText = longest;
            x = xAt(playhead_);
            break;
        case Drag::TrimStart: case Drag::TrimEnd: {
            const bool start = drag_ == Drag::TrimStart;
            RecordingEdit trimmed = edit_;
            trimmed.trimStart = dragStart_;
            trimmed.trimEnd = dragEnd_;
            const qint64 at = start ? dragStart_ : dragEnd_;
            text = QStringLiteral("%1 · %2 clip").arg(clock(at), lengthText(trimmed.exportDuration(duration_)));
            widestText = QStringLiteral("%1 · %2 clip").arg(longest, lengthText(duration_));
            x = xAt(at);
            side = start ? 1 : -1;
            break;
        }
        case Drag::CutStart: case Drag::CutEnd: {
            const bool start = drag_ == Drag::CutStart;
            const qint64 at = start ? dragStart_ : dragEnd_;
            text = QStringLiteral("%1 · %2").arg(clock(at), change(at - (start ? originStart_ : originEnd_)));
            widestText = QStringLiteral("%1 · %2").arg(longest, widestChange);
            x = xAt(at);
            side = start ? -1 : 1;
            break;
        }
        case Drag::CoverStart: case Drag::CoverEnd: case Drag::CoverSlide: {
            const bool start = drag_ != Drag::CoverEnd;
            const qint64 at = start ? dragStart_ : dragEnd_;
            text = QStringLiteral("%1 · %2").arg(clock(at), change(at - (start ? originStart_ : originEnd_)));
            widestText = QStringLiteral("%1 · %2").arg(longest, widestChange);
            x = dragPos_.x();
            y = coversRect().top() - L.flagHeight - 2;
            break;
        }
        case Drag::Select: case Drag::SelectionStart: case Drag::SelectionEnd: {
            // The moving edge's time and the selection's length.
            const bool startMoves = drag_ == Drag::SelectionStart || (drag_ == Drag::Select && dragStart_ != snap(anchorUs_));
            const qint64 at = startMoves ? dragStart_ : dragEnd_;
            text = QStringLiteral("%1 · %2").arg(clock(at), lengthText(dragEnd_ - dragStart_));
            widestText = QStringLiteral("%1 · %2").arg(longest, lengthText(duration_));
            x = xAt(at);
            side = startMoves ? -1 : 1;
            break;
        }
        default: break;
        }
        if (!text.isEmpty()) {
            const double w = std::max(flagMetrics.horizontalAdvance(widest(widestText)), flagMetrics.horizontalAdvance(text)) + 12;
            const double flagLeft = side < 0 ? x - 6 - w : side > 0 ? x + 6 : x - w / 2;
            paintFlag(QRectF(std::clamp<double>(flagLeft, 0, std::max(0.0, width() - w)), y, w, L.flagHeight), text);
        }
    }

    // The skim line and its own time flag (after hover intent); with skimming
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
    // The hovered cut or cover (its bar or an edge) for the Edits list (Q9).
    const auto item = [](const Hit& h) -> QString {
        if (h.target == Target::Cut || h.target == Target::CutStart || h.target == Target::CutEnd) return QStringLiteral("cut");
        if (h.target == Target::Cover || h.target == Target::CoverStart || h.target == Target::CoverEnd) return QStringLiteral("cover");
        return {};
    };
    const QString kind = item(next), id = kind.isEmpty() ? QString() : next.id;
    if (kind != hoverItemKind_ || id != hoverItemId_) {
        hoverItemKind_ = kind;
        hoverItemId_ = id;
        emit hoverChanged(kind, id);
    }
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
    // Remember the last grip under the pointer and when it left it (Q10t).
    if (onGrip(hit.target)) { gripAt_ = pos; gripLeft_.invalidate(); }
    else if (onGrip(hover_.target)) gripLeft_.start();
    setHover(hit);
    // Cursors (T3): edges and brackets split, the playhead's ruler scrubs, empty
    // strip selects while paused.
    if (onGrip(hit.target)) { setCursor(Qt::SplitHCursor); return; }
    switch (hit.target) {
    case Target::Playhead: case Target::Ruler: setCursor(Qt::SizeHorCursor); break;
    case Target::Strip: setCursor(Qt::IBeamCursor); break;  // a drag there selects, playing or not (R14)
    case Target::Cover: setCursor(selectedKind_ == "cover" && selectedId_ == hit.id ? Qt::OpenHandCursor : Qt::PointingHandCursor); break;
    case Target::Cut: case Target::More: setCursor(Qt::PointingHandCursor); break;
    case Target::Overview: setCursor(overviewThumb().contains(pos.toPoint()) ? Qt::OpenHandCursor : Qt::ArrowCursor); break;
    default: setCursor(Qt::ArrowCursor); break;
    }
}
// Hover intent (T8): 6 px of net horizontal travel, or 150 ms dwelling, over
// the ruler or strip and off the grips. Before it nothing shows; after it the
// skim follows the pointer until it reaches a grip, leaves vertically by 8 px
// or leaves the ruler and strip.
void FilmstripTimeline::hoverSkim(QPointF pos) {
    const auto strip = stripRect();
    const bool area = !playing_ && pos.y() >= 0 && pos.y() <= strip.bottom() + 4 && pos.x() >= strip.left() && pos.x() <= strip.right() + 1;
    if (!area) { leaveSkimArea(); return; }
    if (!inSkimArea_) {
        inSkimArea_ = true;
        skimIntent_ = false;
        intentAnchor_ = dwellAt_ = pos;
        dwellTimer_.start();
    }
    // After a click, nothing skims until the pointer has moved kSkimClickMove.
    if (skimBlockedAt_) {
        if (QLineF(*skimBlockedAt_, pos).length() < kSkimClickMove) return;
        skimBlockedAt_.reset();
        intentAnchor_ = dwellAt_ = pos;
        dwellTimer_.start();
    }
    const bool grip = onGrip(hover_.target);
    if (skimIntent_) {
        // Leaving vertically ends the skim: 8 px from the reference while the
        // recent motion (the last 40 ms, at least 6 px of it) is mostly vertical;
        // sideways travel carries the reference along (P2). Passing over a grip
        // does not end it, only resting on one does (the dwell timer, R9).
        double sdx = 0, sdy = 0, path = 0;
        const qint64 now = motionClock_.elapsed();
        for (int i = int(recentMotion_.size()) - 1; i >= 0 && path < 6; --i) {
            const auto& [t, d] = recentMotion_[i];
            if (now - t > 40) break;
            sdx += d.x();
            sdy += d.y();
            path += std::hypot(d.x(), d.y());
        }
        if (std::abs(sdy) > 2 * std::abs(sdx)) { if (std::abs(pos.y() - engagedY_) >= kSkimVertical) { disengageSkim(pos); return; } }
        else if (std::abs(sdx) >= 2 * std::abs(sdy)) engagedY_ = pos.y();
        if (QLineF(dwellAt_, pos).length() > kSkimDwellSlop) { dwellAt_ = pos; dwellTimer_.start(); }
        const qint64 at = std::clamp<qint64>(timeAt(pos.x()), 0, std::max<qint64>(0, duration_ - 1));
        if (skimming_) skimAt(at);
        else if (hoverLine_ != at) { hoverLine_ = at; update(); }
        return;
    }
    if (QLineF(dwellAt_, pos).length() > kSkimDwellSlop) { dwellAt_ = pos; dwellTimer_.start(); }
    if (grip) { intentAnchor_ = pos; return; }
    // Intent: mostly horizontal travel since entering, |dx| at least 12 px and three times |dy| (Q3).
    const QPointF d = pos - intentAnchor_;
    if (std::abs(d.x()) >= kSkimIntentPx && std::abs(d.x()) >= 3 * std::abs(d.y())) engageSkim();
}
void FilmstripTimeline::disengageSkim(QPointF pos) {
    skimIntent_ = false;
    hoverLine_.reset();
    clearSkim();
    intentAnchor_ = dwellAt_ = pos;
    dwellTimer_.start();
    update();
}
void FilmstripTimeline::engageSkim() {
    if (!lastMove_) return;
    skimIntent_ = true;
    engagedY_ = lastMove_->y();
    dwellTimer_.stop();
    const qint64 at = std::clamp<qint64>(timeAt(lastMove_->x()), 0, std::max<qint64>(0, duration_ - 1));
    if (skimming_) skimAt(at);
    else { hoverLine_ = at; update(); }
}
void FilmstripTimeline::leaveSkimArea() {
    inSkimArea_ = false;
    travelOnly_ = false;
    skimIntent_ = false;
    dwellTimer_.stop();
    if (hoverLine_) { hoverLine_.reset(); update(); }
    clearSkim();
}
void FilmstripTimeline::clearSkim(bool silent) {
    skimRest_.stop();
    restThrottle_.stop();
    lastRested_.reset();
    if (!skim_) return;
    skim_.reset();
    skimExact_ = false;
    update();
    if (!silent) emit skimLeft();
}
void FilmstripTimeline::restartSkimTimer() { skimRest_.start(); }
void FilmstripTimeline::clearSelections() {
    selection_.reset();
    selectedKind_.clear();
    selectedId_.clear();
    update();
}

// ---- drags (A3-A9, T5-T10, T14-T16) ----
void FilmstripTimeline::beginDrag(Drag drag, const QString& id, QPointF pos) {
    // The origins come from what is shown (a pending span included), read
    // before drag_ is set so they are not the last drag's live span.
    drag_ = Drag::None;
    if (pending_.easing) clearPending();  // a refusal easing back: start from the edit
    dragId_ = id;
    armed_ = false;
    deferredSelect_ = selectFromCut_ = false;
    pressScroll_ = scrollUs_;
    preKind_ = selectedKind_;
    preId_ = selectedId_;
    preSelection_ = selection_;
    pressSelected_ = false;
    pressPos_ = dragPos_ = pos;
    snapGuide_.reset();
    snapLock_.reset();
    pressTimeUs_ = double(scrollUs_) + (pos.x() - stripLeft()) * usPerPixel();
    originScroll_ = scrollUs_;
    originStart_ = originEnd_ = 0;
    switch (drag) {
    case Drag::TrimStart: case Drag::TrimEnd: { const auto trim = shownTrim(); originStart_ = trim.start; originEnd_ = trim.end; break; }
    case Drag::CutStart: case Drag::CutEnd: for (const auto& cut : edit_.cuts) if (cut.id == id) { const auto live = shownCut(cut); originStart_ = live.start; originEnd_ = live.end; } break;
    case Drag::CoverStart: case Drag::CoverEnd: case Drag::CoverSlide: for (const auto& mask : edit_.masks) if (mask.id == id) { const auto live = shownCover(mask); originStart_ = live.start; originEnd_ = live.end; } break;
    case Drag::SelectionStart: case Drag::SelectionEnd: if (selection_) originStart_ = selection_->start, originEnd_ = selection_->end; break;
    case Drag::Overview: {
        // A press beside the thumb centres the view there; the drag carries on from it.
        const auto track = overviewRect();
        if (!overviewThumb().contains(pos.toPoint()) && track.width() > 0) {
            const qint64 at = qint64(std::llround((pos.x() - track.left()) * double(duration_) / track.width()));
            setScroll(at - visibleLength() / 2);
            originScroll_ = scrollUs_;
        }
        if (playing_) followSuspended_ = true;
        armed_ = true;
        setCursor(Qt::ClosedHandCursor);
        break;
    }
    default: break;
    }
    drag_ = drag;
    // A dragged item answered by a pending proposal starts from where it shows.
    if (pending_.drag != Drag::None && ((edgeDrag() && pending_.id == id) || ((drag == Drag::TrimStart || drag == Drag::TrimEnd) && (pending_.drag == Drag::TrimStart || pending_.drag == Drag::TrimEnd)))) clearPending();
    dragStart_ = originStart_;
    dragEnd_ = originEnd_;
}
// Edges, selection ends and scrubs snap to the playhead, trim ends, cut edges,
// cover edges and selection edges (never to the dragged item itself; trim
// drags also to the recording's ends). A target is taken within min(6 px,
// 150 ms) and held until 3 px past that (T10); frames snap underneath and the
// guide sits at the frame-snapped target. `second` also tries us + second (a
// slid cover's end) and returns the start that puts that end on the target.
qint64 FilmstripTimeline::snapToTargets(qint64 us, bool bypass, std::optional<qint64> second) {
    snapGuide_.reset();
    // The clip's ends are reachable even when the duration is not frame-aligned (P5).
    if (us >= duration_) { snapLock_.reset(); return duration_; }
    if (bypass) { snapLock_.reset(); return snap(us); }
    QList<qint64> targets;
    if (drag_ != Drag::Scrub && drag_ != Drag::Playhead) targets << playhead_;
    if (drag_ != Drag::TrimStart && drag_ != Drag::TrimEnd) targets << edit_.trimStart << edit_.exportEnd(duration_);
    else targets << 0 << duration_;
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
    // Never the dragged item's own starting point (R1): a drag that starts on a
    // target moves from its first armed pixel.
    QList<qint64> origins;
    switch (drag_) {
    case Drag::TrimStart: case Drag::CutStart: case Drag::CoverStart: case Drag::SelectionStart: origins << originStart_; break;
    case Drag::TrimEnd: case Drag::CutEnd: case Drag::CoverEnd: case Drag::SelectionEnd: origins << originEnd_; break;
    case Drag::CoverSlide: origins << originStart_ << originEnd_; break;
    case Drag::Select: origins << snap(anchorUs_); break;
    case Drag::Playhead: origins << prePlayhead_; break;
    default: break;
    }
    const qint64 frame = frameUs();
    targets.erase(std::remove_if(targets.begin(), targets.end(), [&](qint64 t) { return std::any_of(origins.cbegin(), origins.cend(), [&](qint64 o) { return std::abs(t - o) <= frame; }); }), targets.end());
    // Reach in screen pixels at every zoom (R16t).
    const double reach = kSnapReach * usPerPixel(), hold = reach + kSnapRelease * usPerPixel();
    std::optional<qint64> result;
    // A held target stays until the pointer is past its release reach.
    if (snapLock_ && targets.contains(*snapLock_)) {
        if (std::abs(double(*snapLock_ - us)) <= hold) result = *snapLock_;
        else if (second && std::abs(double(*snapLock_ - (us + *second))) <= hold) result = *snapLock_ - *second;
    }
    if (!result) {
        snapLock_.reset();
        double nearest = reach + 1;
        for (const qint64 t : targets) {
            if (const double d = std::abs(double(t - us)); d <= reach && d < nearest) { nearest = d; result = t; snapLock_ = t; }
            if (second) if (const double d = std::abs(double(t - (us + *second))); d <= reach && d < nearest) { nearest = d; result = t - *second; snapLock_ = t; }
        }
    }
    if (snapLock_) snapGuide_ = snap(*snapLock_);
    if (result && (*result == duration_ || *result == 0)) return *result;
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
        // The dead zone: nothing changes until the pointer has moved this far
        // horizontally. A time selection also needs mostly horizontal motion (T15);
        // a bar this press selected slides only after 8 px (T5).
        const double dx = std::abs(pos.x() - armFrom_.x()), dy = std::abs(pos.y() - armFrom_.y());
        const double threshold = drag_ == Drag::Strip ? kSelectStart : drag_ == Drag::Scrub || drag_ == Drag::Playhead ? kScrubDeadZone : drag_ == Drag::CoverSlide && pressSelected_ ? kFirstPressSlide : kDeadZone;
        if (dx < threshold || (drag_ == Drag::Strip && dx <= dy)) return;
        armed_ = true;
        // An unselected cut's edge selects it only now that the drag is real (R7).
        if (std::exchange(deferredSelect_, false)) { selectLocally("cut", dragId_); emit cutSelected(dragId_); pressSelected_ = true; }
        if (drag_ == Drag::Scrub || drag_ == Drag::Playhead) emit scrubStarted();
        if (drag_ == Drag::Strip) drag_ = Drag::Select;
        if (drag_ == Drag::CoverSlide) setCursor(Qt::ClosedHandCursor);
        else if (drag_ == Drag::Scrub || drag_ == Drag::Playhead) setCursor(Qt::SizeHorCursor);
        else if (drag_ != Drag::Select) setCursor(Qt::SplitHCursor);
    }
    const double clampedX = std::clamp<double>(pos.x(), strip.left(), strip.right() + 1);
    const bool bypass = !snapping_ || (modifiers & Qt::ControlModifier);
    // The grab offset: an edge moves by the pointer's travel from the press
    // (unclamped, plus any autoscroll), never to its absolute x; only the
    // resulting time is clamped (T14).
    const qint64 delta = qint64(std::llround((pos.x() - pressPos_.x()) * usPerPixel())) + (scrollUs_ - originScroll_);
    const qint64 minimum = frameUs();
    const auto kindName = [this] { return drag_ == Drag::CutStart || drag_ == Drag::CutEnd ? QStringLiteral("cut") : drag_ == Drag::SelectionStart || drag_ == Drag::SelectionEnd ? QStringLiteral("selection") : QStringLiteral("cover"); };
    // Above Fit the dragged edge stays in view; autoscroll brings the rest (R20t).
    const auto view = visibleSpan();
    // The whole frames inside the view (snapping rounds to frames, so the clamp does too).
    // A view that reaches the clip's start or end reaches 0 or the duration, frame-aligned or not (P5).
    const qint64 firstIn = view.start <= 0 ? 0 : std::min(duration_, qRound64(std::ceil(double(view.start) * fps_ / 1e6) * 1e6 / fps_));
    const qint64 lastIn = view.end >= duration_ ? duration_ : std::max(firstIn, std::min(duration_, qRound64(std::floor(double(view.end) * fps_ / 1e6) * 1e6 / fps_)));
    const auto inView = [&](qint64 t) { return zoom_ > 1 ? std::clamp<qint64>(t, firstIn, lastIn) : t; };
    // A cut edge stops at a neighbouring cut's edge (Q8).
    qint64 floorUs = 0, ceilUs = duration_;
    if (drag_ == Drag::CutStart || drag_ == Drag::CutEnd)
        for (const auto& cut : std::as_const(edit_.cuts)) {
            if (cut.id == dragId_) continue;
            if (cut.end <= originStart_) floorUs = std::max(floorUs, cut.end);
            if (cut.start >= originEnd_) ceilUs = std::min(ceilUs, cut.start);
        }
    const auto moveStart = [&](qint64 wanted) {
        const qint64 t = inView(snapToTargets(std::clamp<qint64>(wanted, floorUs, ceilUs), bypass));
        dragStart_ = std::clamp<qint64>(t, std::min(floorUs, snap(dragEnd_ - minimum)), std::max<qint64>(0, snap(dragEnd_ - minimum)));
        if (dragStart_ != t) snapGuide_.reset();
    };
    const auto moveEnd = [&](qint64 wanted) {
        const qint64 t = inView(snapToTargets(std::clamp<qint64>(wanted, floorUs, ceilUs), bypass));
        dragEnd_ = std::clamp<qint64>(t, std::min(duration_, snap(dragStart_ + minimum)), std::max(ceilUs, std::min(duration_, snap(dragStart_ + minimum))));
        if (dragEnd_ != t) snapGuide_.reset();
    };
    switch (drag_) {
    case Drag::None: case Drag::Strip: case Drag::Overview: return;
    case Drag::Scrub: emit seekRequested(snapToTargets(timeAt(clampedX), bypass)); break;
    case Drag::Playhead: emit seekRequested(snapToTargets(std::clamp<qint64>(prePlayhead_ + delta, 0, duration_), bypass)); break;
    case Drag::TrimStart: moveStart(inView(originStart_ + delta)); emit trimEdgeDragged(true, dragStart_); break;
    case Drag::TrimEnd: moveEnd(inView(originEnd_ + delta)); emit trimEdgeDragged(false, dragEnd_); break;
    case Drag::CutStart: case Drag::CoverStart: case Drag::SelectionStart:
        moveStart(inView(originStart_ + delta));
        emit editEdgeDragged(kindName(), dragId_, true, dragStart_);
        break;
    case Drag::CutEnd: case Drag::CoverEnd: case Drag::SelectionEnd:
        moveEnd(inView(originEnd_ + delta));
        emit editEdgeDragged(kindName(), dragId_, false, dragEnd_);
        break;
    case Drag::CoverSlide: {
        // The whole cover slides in time, clamped to the recording.
        const qint64 length = originEnd_ - originStart_;
        // By the pointer's travel only: an end scrolled off screen is not pulled into view (Q1).
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
    // Autoscroll (A9, T9): above Fit, within kAutoscrollZone of a strip end or past it.
    if (zoom_ > 1 && (pos.x() < strip.left() + kAutoscrollZone || pos.x() > strip.right() + 1 - kAutoscrollZone)) {
        if (!autoscroll_.isActive()) { autoscrollEntered_.start(); autoscrollClock_.start(); autoscroll_.start(); }
    } else {
        autoscroll_.stop();
    }
    update();
}
void FilmstripTimeline::autoscrollTick() {
    if (drag_ == Drag::None || drag_ == Drag::Overview || !armed_ || zoom_ <= 1) { autoscroll_.stop(); return; }
    // Entry waits kAutoscrollDelayMs, so a drag that only passes the zone does not scroll.
    if (autoscrollEntered_.elapsed() < kAutoscrollDelayMs) { autoscrollClock_.restart(); return; }
    const auto strip = stripRect();
    const double x = dragPos_.x();
    double overshoot = 0;
    int direction = 0;
    if (x < strip.left() + kAutoscrollZone) { overshoot = strip.left() + kAutoscrollZone - x; direction = -1; }
    else if (x > strip.right() + 1 - kAutoscrollZone) { overshoot = x - (strip.right() + 1 - kAutoscrollZone); direction = 1; }
    if (!direction) { autoscroll_.stop(); return; }
    // A quadratic ramp: about 0.05 visible spans a second at the zone's inner
    // edge, 2.5 at kAutoscrollPast beyond the strip end.
    const double reach = std::clamp(overshoot / double(kAutoscrollZone + kAutoscrollPast), 0.0, 1.0);
    const double spansPerSecond = 0.05 + (2.5 - 0.05) * reach * reach;
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
    snapLock_.reset();
    autoscroll_.stop();
    switch (drag) {
    case Drag::TrimStart: case Drag::TrimEnd: case Drag::CutStart: case Drag::CutEnd: case Drag::CoverStart: case Drag::CoverEnd:
    case Drag::CoverSlide: case Drag::SelectionStart: case Drag::SelectionEnd: case Drag::Select:
        emit edgeDragFinished();
        break;
    default: break;
    }
    // The view comes back as it was before the press, autoscroll or overview drag included (R12).
    setScroll(pressScroll_);
    if ((drag == Drag::Scrub || drag == Drag::Playhead || drag == Drag::Strip || drag == Drag::Select) && playhead_ != prePlayhead_) emit seekRequested(prePlayhead_);
    // A cover or cut press that selected its item gives the earlier selection
    // back, then the playhead its selection may have moved (Q1, P7).
    const bool itemPress = drag == Drag::CoverSlide || drag == Drag::CoverStart || drag == Drag::CoverEnd || drag == Drag::CutStart || drag == Drag::CutEnd
        || ((drag == Drag::Strip || drag == Drag::Select) && selectFromCut_);
    if (itemPress && std::exchange(pressSelected_, false)) {
        selectLocally(preKind_, preId_);
        selection_ = preSelection_;
        if (preKind_ == "cut") emit cutSelected(preId_);
        else if (preKind_ == "cover") emit coverSelected(preId_);
        else if (preSelection_) emit timeSelected(preSelection_->start, preSelection_->end);
        else emit selectionCleared();
        if (playhead_ != prePlayhead_) emit seekRequested(prePlayhead_);
    }
    // A press that held playback gives it back as it was: playing.
    if (std::exchange(pressHeldPlayback_, false)) emit playbackReleased(true);
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
    // A zoom still easing or pinching lands first, so the press hits what it sees settle.
    settleContinuousZoom();
    finishZoom();
    hoverLine_.reset();
    skimIntent_ = false;
    travelOnly_ = true;  // P4
    dwellTimer_.stop();
    auto pos = e->position();
    armFrom_ = pos;  // P1: the dead zone counts from the real press
    skimBlockedAt_ = pos;
    prePlayhead_ = playhead_;
    pressHeldPlayback_ = false;
    const auto strip = stripRect();
    auto hit = hitAt(pos);
    // A press that arrives just after the pointer left a grip, near where it
    // left (an input stack holding the button back, such as middle-button
    // emulation, delivers it late at a moved position), takes that grip, from
    // where the pointer was on it (Q10t). It arms only after 4 px of travel
    // past the real press, so a click with no movement never edits (P1).
    if (!onGrip(hit.target) && gripAt_ && gripLeft_.isValid() && gripLeft_.elapsed() <= kLateGripMs
        && std::abs(pos.x() - gripAt_->x()) <= kLateGripPx && std::abs(pos.y() - gripAt_->y()) <= kLateGripPx) {
        if (const auto grip = hitAt(*gripAt_); onGrip(grip.target)) { hit = grip; pos = *gripAt_; }
    }
    gripAt_.reset();
    const double clampedX = std::clamp<double>(pos.x(), strip.left(), strip.right() + 1);
    // A press that seeks ends the skim silently, so the window's seek keeps
    // the skimmed picture; any other press ends it as leaving does (P3).
    const bool seeks = !playing_ && (hit.target == Target::Ruler || hit.target == Target::Lane || (hit.target == Target::Strip && !(e->modifiers() & Qt::ShiftModifier)));
    clearSkim(seeks);
    setHover(hit);
    // One gesture: an unselected cut's or cover's edge selects it and drags at once (A5).
    const auto select = [this](const QString& kind, const QString& id) {
        if (selectedKind_ == kind && selectedId_ == id) return false;
        selectLocally(kind, id);
        if (kind == "cut") emit cutSelected(id); else emit coverSelected(id);
        return true;
    };
    switch (hit.target) {
    case Target::More: emit moreCoversRequested(); return;
    case Target::TrimStart: case Target::TrimEnd:
        beginDrag(hit.target == Target::TrimStart ? Drag::TrimStart : Drag::TrimEnd, {}, pos);
        anchorUs_ = timeAt(std::clamp<double>(armFrom_.x(), strip.left(), strip.right() + 1));  // P6
        return;
    case Target::SelectionStart: beginDrag(Drag::SelectionStart, {}, pos); return;
    case Target::SelectionEnd: beginDrag(Drag::SelectionEnd, {}, pos); return;
    case Target::CutStart: case Target::CutEnd: {
        // An unselected cut's edge selects it only once the drag arms; a click
        // there is a strip click (R7).
        const bool selected = selectedKind_ == "cut" && selectedId_ == hit.id;
        beginDrag(hit.target == Target::CutStart ? Drag::CutStart : Drag::CutEnd, hit.id, pos);
        deferredSelect_ = !selected;
        anchorUs_ = timeAt(clampedX);
        return;
    }
    case Target::CoverStart: case Target::CoverEnd:
        beginDrag(hit.target == Target::CoverStart ? Drag::CoverStart : Drag::CoverEnd, hit.id, pos);
        pressSelected_ = select("cover", hit.id);
        return;
    case Target::Playhead:
        beginDrag(Drag::Playhead, {}, pos);
        if (playing_) { pressHeldPlayback_ = true; emit playbackHeld(); }  // R21t
        return;
    case Target::Cut:
        // A press on a cut selects it; a drag from it selects time from the press (R8).
        beginDrag(Drag::Strip, {}, pos);
        pressSelected_ = select("cut", hit.id);
        selectFromCut_ = true;
        anchorUs_ = timeAt(clampedX);
        return;
    case Target::Cover:
        // An unselected bar: the press selects it, and it slides only past 8 px (T5).
        beginDrag(Drag::CoverSlide, hit.id, pos);
        pressSelected_ = select("cover", hit.id);
        return;
    case Target::Overview: beginDrag(Drag::Overview, {}, pos); return;
    case Target::Ruler:
        // Seeks on press; a drag scrubs. The selection stays (A14). Playing, the
        // press holds playback and a click seeks on release and plays on (R21t).
        beginDrag(Drag::Scrub, {}, pos);
        anchorUs_ = timeAt(clampedX);
        if (playing_) { pressHeldPlayback_ = true; emit playbackHeld(); }
        else emit seekRequested(anchorUs_);
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
            update();
            emit timeSelected(a, b);
            return;
        }
        // Paused, it seeks on press; playing, the press holds playback and a
        // click seeks on release and plays on (T15). A drag past kSelectStart
        // selects time from here.
        beginDrag(Drag::Strip, {}, pos);
        anchorUs_ = at;
        if (playing_) { pressHeldPlayback_ = true; emit playbackHeld(); }
        else emit seekRequested(at);
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
    recentDelta_.reset();
    if (lastMove_) {
        recentDelta_ = pos - *lastMove_;
        if (!motionClock_.isValid()) motionClock_.start();
        recentMotion_.append({motionClock_.elapsed(), *recentDelta_});
        while (recentMotion_.size() > 16 || (!recentMotion_.isEmpty() && motionClock_.elapsed() - recentMotion_.first().first > 200)) recentMotion_.removeFirst();
    }
    lastMove_ = pos;
    // A release we never saw (the button is up): the drag is cancelled (R2).
    if (drag_ != Drag::None && !(e->buttons() & Qt::LeftButton)) { cancelDrag(); return; }
    if (drag_ != Drag::None) { updateDrag(pos, e->modifiers()); return; }
    updateHover(pos);
    // A button still held after a cancelled drag: no skimming.
    if (e->buttons() != Qt::NoButton) { leaveSkimArea(); return; }
    hoverSkim(pos);
}
void FilmstripTimeline::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    if (drag_ == Drag::None) { updateHover(e->position()); return; }
    // A release outside the widget uses the clamped release position, with
    // the last move's Ctrl (letting go of Ctrl just before the drop does not snap, Q6).
    updateDrag(e->position(), dragModifiers_);
    autoscroll_.stop();
    const auto drag = std::exchange(drag_, Drag::None);
    const bool armed = std::exchange(armed_, false);
    const bool held = std::exchange(pressHeldPlayback_, false);
    const auto id = std::exchange(dragId_, QString());
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    snapGuide_.reset();
    snapLock_.reset();
    skimBlockedAt_ = e->position();
    travelOnly_ = true;  // P4
    update();
    const bool changed = dragStart_ != originStart_ || dragEnd_ != originEnd_;
    const bool deferred = std::exchange(deferredSelect_, false), fromCut = std::exchange(selectFromCut_, false);
    // A click on the strip: the seek (on press when paused, here when playing)
    // and, unless Shift is held, clearing the time selection; a selected cut or
    // cover stays selected (R18t).
    const auto stripClick = [&](bool seek) {
        if (seek) emit seekRequested(anchorUs_);
        if (!shift && selection_) { selection_.reset(); update(); emit timeSelectionCleared(); }
        if (held) emit playbackReleased(true);
    };
    // After a selection drag the playhead lands on the frame the viewer showed
    // for the moving edge (R19t): an end shows the frame before it.
    const auto landOn = [&](bool startMoved) {
        emit seekRequested(startMoved ? dragStart_ : std::max(dragStart_, snap(dragEnd_ - frameUs())));
    };
    switch (drag) {
    case Drag::TrimStart: case Drag::TrimEnd: {
        emit edgeDragFinished();
        if (armed && changed) { setPending(drag, {}, dragStart_, dragEnd_); emit trimProposed(dragStart_, dragEnd_); }
        // A click on the kept side of a bracket's zone is a strip click; on its body it does nothing (P6).
        const bool keptSide = drag == Drag::TrimStart ? armFrom_.x() > xFor(originStart_) : armFrom_.x() < xFor(originEnd_);
        if (!armed && keptSide) stripClick(true);
        break;
    }
    case Drag::CutStart: case Drag::CutEnd:
        emit edgeDragFinished();
        if (!armed && deferred) {
            // A click inside the cut selects it; in the 4 px outside it, it seeks (R7, Q5).
            const auto it = std::find_if(edit_.cuts.cbegin(), edit_.cuts.cend(), [&](const RecordingCut& c) { return c.id == id; });
            if (it != edit_.cuts.cend() && pressPos_.x() >= xFor(it->start) && pressPos_.x() <= xFor(it->end)) { selectLocally("cut", id); emit cutSelected(id); }
            else stripClick(true);
            break;
        }
        if (armed && changed) { setPending(drag, id, dragStart_, dragEnd_); emit cutAdjusted(id, dragStart_, dragEnd_); }
        break;
    case Drag::CoverStart: case Drag::CoverEnd:
        emit edgeDragFinished();
        if (armed && changed) { setPending(drag, id, dragStart_, dragEnd_); emit coverRetimed(id, dragStart_, dragEnd_); }
        break;
    case Drag::CoverSlide:
        // A click on a bar only selected it; a slide is one retime on release.
        if (!armed) break;
        emit edgeDragFinished();
        if (changed) { setPending(drag, id, dragStart_, dragEnd_); emit coverRetimed(id, dragStart_, dragEnd_); }
        break;
    case Drag::SelectionStart: case Drag::SelectionEnd:
        emit edgeDragFinished();
        if (armed && changed) {
            selection_ = recording::Span{dragStart_, dragEnd_};
            emit timeSelected(dragStart_, dragEnd_);
            landOn(drag == Drag::SelectionStart);
        }
        break;
    case Drag::Strip:
        // A click on a cut only selected it (R8); elsewhere it is a strip click.
        if (!fromCut) stripClick(held);
        break;
    case Drag::Select: {
        emit edgeDragFinished();
        // Once armed, a selection stays whatever its length, from one frame (R10).
        if (dragEnd_ - dragStart_ < frameUs() - 1) { stripClick(held); break; }
        const bool startMoved = dragStart_ != snap(anchorUs_);
        selection_ = recording::Span{dragStart_, dragEnd_};
        selectedKind_.clear();
        selectedId_.clear();
        emit timeSelected(dragStart_, dragEnd_);
        landOn(startMoved);
        if (held) emit playbackReleased(false);
        break;
    }
    case Drag::Scrub: case Drag::Playhead:
        if (armed) { if (held) emit playbackReleased(false); break; }
        // A plain click: on the ruler it seeks (on release when playing); on the
        // cap, farther than 2 px from the line, it seeks to the click (R3).
        if (drag == Drag::Scrub && held) emit seekRequested(anchorUs_);
        if (drag == Drag::Playhead && std::abs(pressPos_.x() - xFor(prePlayhead_)) > kPlayheadClickSlop) emit seekRequested(timeAt(std::clamp<double>(pressPos_.x(), stripRect().left(), stripRect().right() + 1)));
        if (held) emit playbackReleased(true);
        break;
    case Drag::Overview: case Drag::None: break;
    }
    updateHover(e->position());
}
// Losing the window or the pointer to a popup mid-drag cancels the drag (R2).
void FilmstripTimeline::focusOutEvent(QFocusEvent* e) {
    if (drag_ != Drag::None && (e->reason() == Qt::ActiveWindowFocusReason || e->reason() == Qt::PopupFocusReason)) cancelDrag();
    QWidget::focusOutEvent(e);
}
void FilmstripTimeline::leaveEvent(QEvent* e) {
    if (drag_ == Drag::None) {
        leaveSkimArea();
        setHover({});
        lastMove_.reset();
        recentDelta_.reset();
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
    const bool trackpad = !pixels.isNull();
    // A gesture starts afresh at its begin phase or after kGestureIdleMs idle (T12).
    if (e->phase() == Qt::ScrollBegin || !wheelClock_.isValid() || wheelClock_.elapsed() > kGestureIdleMs) {
        latchedAxis_ = 0;
        latchTravel_ = latchBuffer_ = {};
        wheelRemainder_ = 0;
    }
    wheelClock_.restart();
    const bool ended = e->phase() == Qt::ScrollEnd;
    const auto step = [&](int steps) {
        const auto levels = zoomLevels();
        int index = 0;
        for (int i = 0; i < levels.size(); ++i) if (levels[i] <= zoom_) index = i;
        setZoom(levels[std::clamp(index + steps, 0, int(levels.size()) - 1)], anchor);
    };
    if (e->modifiers() & Qt::ControlModifier) {
        // Zoom at the pointer: every wheel notch is a level; a trackpad zooms
        // continuously and settles on the nearest level when it ends (R5).
        if (trackpad) {
            paintLive_ = true;
            const qint64 at = timeAt(e->position().x());
            paintLive_ = false;
            continuousZoom(std::pow(1.5, dominant(pixels) / double(kPixelZoomStep)), at);
        } else {
            wheelRemainder_ += dominant(angle);
            const int notches = wheelRemainder_ / 120;
            if (notches != 0) { wheelRemainder_ -= notches * 120; step(notches); }
        }
        if (ended) { wheelRemainder_ = 0; settleContinuousZoom(); }  // a gesture's end carries no pixels
        e->accept();
        return;
    }
    if (zoom_ <= 1) { e->ignore(); return; }
    // Above Fit, the wheel and Shift+wheel scroll a tenth of the visible span a
    // notch; a trackpad scrolls by its pixels along the axis it latched after
    // kAxisLatch px. A manual scroll while playing suspends following (T18).
    qint64 by = 0;
    if (trackpad) {
        if (!latchedAxis_) {
            latchTravel_ += QPointF(std::abs(pixels.x()), std::abs(pixels.y()));
            latchBuffer_ += QPointF(pixels);
            if (std::max(latchTravel_.x(), latchTravel_.y()) < kAxisLatch) { e->accept(); return; }
            // Latched: the travel buffered before it counts too (R15).
            latchedAxis_ = latchTravel_.x() >= latchTravel_.y() ? 1 : 2;
            by = -qint64(std::llround((latchedAxis_ == 1 ? latchBuffer_.x() : latchBuffer_.y()) * usPerPixel()));
        } else {
            by = -qint64(std::llround((latchedAxis_ == 1 ? pixels.x() : pixels.y()) * usPerPixel()));
        }
        if (ended) latchedAxis_ = 0;
    } else {
        const int d = dominant(angle);
        if (d == 0) { e->ignore(); return; }
        by = -qint64(double(d) / 120.0 * double(visibleLength()) * 0.1);
    }
    if (by != 0) {
        if (playing_) followSuspended_ = true;
        setScroll(scrollUs_ + by);
    }
    e->accept();
}
bool FilmstripTimeline::event(QEvent* e) {
    if (e->type() == QEvent::WindowDeactivate && drag_ != Drag::None) cancelDrag();  // R2
    if (e->type() == QEvent::NativeGesture) {
        // Pinch zooms continuously at the pointer and settles on the nearest
        // ladder level when it ends (R5).
        const auto* gesture = static_cast<QNativeGestureEvent*>(e);
        if (gesture->gestureType() == Qt::BeginNativeGesture) return true;
        if (gesture->gestureType() == Qt::EndNativeGesture) { settleContinuousZoom(); return true; }
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            paintLive_ = true;
            const qint64 at = timeAt(gesture->position().x());
            paintLive_ = false;
            continuousZoom(1 + gesture->value(), at);
            return true;
        }
    }
    return QWidget::event(e);
}
void FilmstripTimeline::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    setScroll(scrollUs_);
    layoutCovers();
    emit visibleSpanChanged();
}
}
