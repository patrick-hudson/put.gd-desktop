#pragma once
#include "core/KeptSpans.h"
#include "core/RecordingModel.h"
#include "MaskNames.h"
#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

namespace putgd::record {

// #1613: the review window's timeline (replaces ReviewTimeline). From the top:
// a ruler in source time captioned "recording time" at its right end; a
// filmstrip of ~72 px tiles with the trim brackets (accent; outside dimmed),
// cuts hatched with a "Cut · n s" chip, the selected cover's span as a 20 %
// accent tint with a 3 px accent base line, and a time selection as a dashed
// accent range; the playhead (cap on the ruler, a two-tone stroke that ends
// above the Covers lanes); the skim line and its own time flag while paused
// and hovering; and the Covers track: two lanes of named bars, "+n" past two,
// "No covers yet" when empty. Spec: put.gd-evidence record
// 2026-11-17-recording-review-redesign, design.md "Timeline"; picture and
// geometry: mock/review4.html (drawTimeline). Every colour is a Theme token;
// lane and text heights come from QFontMetrics, so 150 % text grows the widget.
//
// It shows and proposes only. Each committed gesture is one signal on release
// (one undo step in the resident); the window turns signals into edits, and
// setEdit() is what it then paints.
//
// Pointer model (feel spec A1-A13, B1-B4): one hit test (hitAt) drives hover,
// cursor, press and parts(), in priority order: the nearest edge within its
// zone (trim brackets, every cut edge, every cover-bar edge, the time
// selection's edges), the playhead, a cut or cover-bar body, then empty ruler,
// strip or lane. Edge, bar and selection-edge drags change nothing for the
// first 3 px, then move by the pointer's delta from the press; they snap to the
// other edges and the playhead within 6 px (Ctrl bypasses, N toggles), show a
// readout flag, autoscroll near the strip ends above Fit, and Esc or a right
// press cancels them.
class FilmstripTimeline final : public QWidget {
    Q_OBJECT
public:
    explicit FilmstripTimeline(std::shared_ptr<MaskNames> names, QWidget* parent = nullptr);
    // Where tile images come from: the tile covering a source time, or a null
    // image (painted as a surface placeholder). The window passes
    // Filmstrip::tile and calls tilesChanged() on Filmstrip::tileReady.
    using TileSource = std::function<QImage(qint64 sourceUs)>;
    void setTileSource(TileSource);
    void tilesChanged();
    void setDuration(qint64 durationUs, int fps);
    // A new revision ends a trim, cut or cover edge drag without proposing it.
    void setEdit(const RecordingEdit&);
    // A pointer drag (scrub, select or an edge) is in progress.
    bool dragging() const { return drag_ != Drag::None; }
    void setPlayhead(qint64 sourceUs);
    qint64 duration() const { return duration_; }
    qint64 playhead() const { return playhead_; }
    void setPlaying(bool playing);  // no skimming while playing
    // At most one of these is shown: a time selection, or a selected cut or
    // cover. Setting one clears the other.
    void setTimeSelection(std::optional<recording::Span>);
    std::optional<recording::Span> timeSelection() const { return selection_; }
    // kind "cut" | "cover" | "trim" | "" (none).
    void setSelectedEdit(const QString& kind, const QString& id);
    // Zoom ladder (A13): integer levels {1,2,3,4,6,8,12,16,24,32,...} up to
    // about 8 px per frame for the current duration, fps and width; 1 is Fit.
    // setZoom snaps to the nearest level. Zooming keeps `anchorUs` under the
    // same x (Ctrl+wheel and pinch pass the pointer's time; without one, the
    // view's centre stays put). Above Fit the strip scrolls horizontally
    // (wheel / Shift+wheel / trackpad), follows the playhead while playing
    // (a manual scroll suspends that until the playhead is back in view or
    // playback restarts), and an overview track under the lanes shows and
    // drags the visible span.
    void setZoom(int level, std::optional<qint64> anchorUs = std::nullopt);
    int zoom() const { return zoom_; }
    QList<int> zoomLevels() const;
    void zoomIn(std::optional<qint64> anchorUs = std::nullopt);
    void zoomOut(std::optional<qint64> anchorUs = std::nullopt);
    void zoomFit();
    // Above Fit, scrolls the least amount that shows `span` (centred when it is
    // wider than the view or wholly off screen); nothing at Fit. Undo and redo
    // call it for the item they brought back.
    void reveal(recording::Span span);
    // Snapping (A7) and skimming (B4): on by default; the window's N / S keys
    // and the transport's skim toggle call the setters. Each emits its
    // *Changed signal only on a change. Skimming off: hover shows only the
    // hairline and its time; no skim signals.
    void setSnapping(bool on);
    bool snapping() const { return snapping_; }
    void setSkimming(bool on);
    bool skimming() const { return skimming_; }
    // [ and ] (E4, the window calls it from any focus): the time selection's
    // start or end at the playhead; with none, it runs to the kept footage's
    // other end. Emits timeSelected.
    // With a cut selected, its start or end moves to the playhead instead (one
    // cutAdjusted); with a cover selected it does nothing (the window sets the
    // cover's time as Shift+I / Shift+O do).
    void markSelectionEdge(bool start);
    // Paused hover at a source time, as the pointer over the ruler or strip
    // makes it after hover intent (skimMoved, then skimRested 40 ms after it
    // rests), or nullopt for the pointer leaving (skimLeft). recording.review
    // skim / skimEnd call it.
    void skimAt(std::optional<qint64> sourceUs);
    std::optional<qint64> skimTime() const { return skim_; }
    // The skim flag reads "≈m:ss.s" until the exact frame for the skim time has
    // arrived; the window calls this when it shows that frame.
    void setSkimExact(bool exact);
    // The visible source span and the tile times to request for it (12 at Fit
    // across the default width, up to 24 per zoom batch; the two ends first).
    recording::Span visibleSpan() const;
    QList<qint64> tileTimes() const;
    // Geometry for tests, the window and recording.review inspect.
    QRect rulerRect() const;
    QRect stripRect() const;
    QRect coversRect() const;
    int xFor(qint64 sourceUs) const;
    qint64 timeAt(qreal x) const;
    QRect trimHandleRect(bool start) const;
    QRect cutRect(const QString& id) const;
    QRect coverBarRect(const QString& id) const;  // empty when the bar is in a hidden lane
    QRect overviewRect() const;  // the overview track; empty at Fit
    // {zoom, zoomLevels, visible: {start, end}, selection, selectedEdit: {kind, id},
    //  skim: {time, exact} | null, snapping, skimming, hover: {kind, id} | null,
    //  pending: {kind trim|cut|cover, id, start, end} | null (a released drag
    //  painted where it was dropped until the edit answers it),
    //  lanes, hiddenCovers, tiles: {shown, placeholders}}
    QJsonObject inspect() const;
    // The things a pointer or a screen reader can name, with the hit zones the
    // pointer uses: both trim handles, the playhead, the time selection's
    // edges, every cut and its edges, every shown cover bar and its edges, the
    // "+n" chip and the overview track. The accessible children come from it.
    // kind: trim-start | trim-end | playhead | selection-start | selection-end |
    // cut | cut-start | cut-end | cover | cover-start | cover-end | more | overview.
    struct Part { QString kind, id, name; QRect rect; };
    QList<Part> parts() const;
    // The index in parts() of what a widget-local point hits (the pointer's
    // priority), or -1 over empty space; the accessible childAt uses it.
    int partAt(QPoint) const;
    QSize sizeHint() const override;         // height 168 at normal text
    QSize minimumSizeHint() const override;
signals:
    // A press on empty ruler, strip or covers lane seeks at once. A click there
    // clears any selection (the lane on press, the ruler and strip on release).
    // Dragging from the ruler or the playhead past 3 px scrubs: scrubStarted,
    // then seekRequested as it moves.
    void seekRequested(qint64 sourceUs);
    void scrubStarted();
    // Dragging a trim bracket: the edge time while dragging (the viewer shows
    // the frame the clip will start or end on), then trimProposed on release.
    void trimEdgeDragged(bool start, qint64 sourceUs);
    void trimProposed(qint64 startUs, qint64 endUs);
    // A drag (4 px or more) on empty strip, a selection edge drag, Shift+click
    // or markSelectionEdge makes or changes a time selection.
    void timeSelected(qint64 startUs, qint64 endUs);
    void selectionCleared();  // a click that seeks
    // A press on a cut or a cover bar (its body or an edge) selects it.
    void cutSelected(const QString& id);
    void coverSelected(const QString& id);
    // Dragging a cut's edge, or a cover bar's end or body, on release.
    void cutAdjusted(const QString& id, qint64 startUs, qint64 endUs);
    void coverRetimed(const QString& id, qint64 startUs, qint64 endUs);
    // While dragging: kind "cut" | "cover" | "selection" (id empty), the
    // moving edge's time (a cover slide reports its start); the viewer shows
    // that frame, as trimEdgeDragged does for the brackets.
    void editEdgeDragged(const QString& kind, const QString& id, bool start, qint64 sourceUs);
    // Any edge, slide or selection drag was released or cancelled, changed or not.
    void edgeDragFinished();
    // Paused hover over the ruler or strip, after hover intent (80 ms over it
    // with mostly horizontal motion). skimMoved with the time under the
    // pointer; skimRested 40 ms after it rests (the window asks FrameSkimmer
    // for the exact frame); skimLeft when the pointer leaves (the window
    // restores the playhead frame).
    void skimMoved(qint64 sourceUs);
    void skimRested(qint64 sourceUs);
    void skimLeft();
    void snappingChanged(bool on);
    void skimmingChanged(bool on);
    // The "+n" hidden-covers chip was clicked (the window opens the Edits list).
    void moreCoversRequested();
    // T15: a press on the strip while playing holds playback (the window
    // pauses); the release lets it go: resume true after a click (which seeks
    // on release), false after a drag (it stays paused).
    void playbackHeld();
    void playbackReleased(bool resume);
    void zoomChanged(int level);
    // The visible span changed (zoom, scroll, resize): request tileTimes().
    void visibleSpanChanged();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent*) override;  // pinch zoom
private:
    // What a point hits, in priority order (A1). Empty areas last.
    enum class Target { None, TrimStart, TrimEnd, SelectionStart, SelectionEnd, CutStart, CutEnd, CoverStart, CoverEnd, Playhead, More, Cut, Cover, Overview, Ruler, Strip, Lane };
    struct Zone { Target target = Target::None; QString id; QRect rect; double edgeX = 0, edgeX2 = 0; };
    struct Hit { Target target = Target::None; QString id; QRect rect; bool operator==(const Hit& o) const { return target == o.target && id == o.id; } };
    enum class Drag { None, Scrub, Playhead, TrimStart, TrimEnd, CutStart, CutEnd, CoverStart, CoverEnd, CoverSlide, SelectionStart, SelectionEnd, Strip, Select, Overview };
    QList<Zone> zones() const;
    Hit hitAt(QPointF) const;
    static QString kindOf(Target);
    qint64 frameUs() const;
    qint64 snap(qint64 us) const;
    qint64 visibleLength() const;
    int stripLeft() const;
    int stripWidth() const;
    int maxZoom() const;
    double usPerPixel() const;
    // What the timeline paints and hit-tests for an item: the drag's live span,
    // a released drag's pending span (T4), or the committed one.
    recording::Span shownTrim() const;
    recording::Span shownCut(const RecordingCut&) const;
    recording::Span shownCover(const RecordingMask&) const;
    void setPending(Drag, const QString& id, qint64 start, qint64 end);
    void easePendingBack();
    void clearPending();
    void setScroll(qint64 startUs);
    void viewChanged();
    void revealPlayhead();
    void layoutCovers();
    void selectLocally(const QString& kind, const QString& id);
    void clearSkim();
    void clearSelections();
    void restartSkimTimer();
    QRect laneRect(int lane) const;
    QRect moreRect() const;
    QRect overviewThumb() const;
    // Drags.
    void beginDrag(Drag, const QString& id, QPointF pos);
    void updateDrag(QPointF pos, Qt::KeyboardModifiers);
    void cancelDrag();
    void autoscrollTick();
    bool edgeDrag() const;
    qint64 snapToTargets(qint64 us, bool bypass, std::optional<qint64> second = std::nullopt);
    // Hover.
    void updateHover(QPointF);
    void setHover(const Hit&);
    bool hovered(Target, const QString& id = {}) const;
    void hoverSkim(QPointF);
    void leaveSkimArea();
    void engageSkim();
    void emitRested();
    bool onGrip(Target) const;
    TileSource tiles_;
    std::shared_ptr<MaskNames> names_;
    qint64 duration_ = 1;
    int fps_ = 30;
    RecordingEdit edit_;
    qint64 playhead_ = 0;
    bool playing_ = false;
    std::optional<recording::Span> selection_;
    QString selectedKind_, selectedId_;
    int zoom_ = 1;
    qint64 scrollUs_ = 0;  // the visible span's start above Fit
    std::optional<qint64> skim_;
    bool skimExact_ = false;
    QTimer skimRest_;
    bool snapping_ = true, skimming_ = true;
    // Hover and skim intent (B1).
    Hit hover_;
    std::optional<qint64> hoverLine_;      // the hairline with skimming off (after intent)
    bool inSkimArea_ = false, skimIntent_ = false;
    QTimer dwellTimer_;                     // 150 ms without moving is intent
    QPointF intentAnchor_, dwellAt_;        // where intent is measured from; where the dwell started
    double engagedY_ = 0;                   // the y at which skimming engaged (8 px vertical departure ends it)
    std::optional<QPointF> lastMove_, skimBlockedAt_, recentDelta_;
    QTimer restThrottle_;                   // skimRested at most every 50 ms while moving
    QElapsedTimer restedClock_;
    std::optional<qint64> lastRested_;
    // The drag in progress.
    Drag drag_ = Drag::None;
    bool armed_ = false;                    // past the dead zone
    bool pressSelected_ = false;            // the press selected the item (a slide needs 8 px)
    bool pressHeldPlayback_ = false;        // T15: the press paused playback
    QString dragId_;
    qint64 dragStart_ = 0, dragEnd_ = 0, anchorUs_ = 0;
    qint64 originStart_ = 0, originEnd_ = 0, prePlayhead_ = 0, originScroll_ = 0;
    double pressTimeUs_ = 0;                // content time under the press (unclamped)
    QPointF pressPos_, dragPos_;
    Qt::KeyboardModifiers dragModifiers_;
    std::optional<qint64> snapGuide_;
    std::optional<qint64> snapLock_;        // the target held while within its release reach (T10)
    QTimer autoscroll_;
    QElapsedTimer autoscrollClock_, autoscrollEntered_;
    bool followSuspended_ = false;          // T18: a manual scroll while playing
    // T4: a released edge or slide, painted where it was dropped until answered.
    struct Pending { Drag drag = Drag::None; QString id; qint64 start = 0, end = 0, fromStart = 0, fromEnd = 0; quint64 revision = 0; bool easing = false; };
    Pending pending_;
    QTimer pendingTimer_, easeTimer_;
    QElapsedTimer easeClock_;
    int wheelRemainder_ = 0;
    // Trackpad gestures (T12): the latched axis (0 none, 1 x, 2 y) and its 6 px decision.
    int latchedAxis_ = 0;
    QPointF latchTravel_;
    QElapsedTimer wheelClock_;
    double pixelZoom_ = 0, pinchScale_ = 1;
    int pinchFrom_ = 0;  // the level a pinch started from; 0 between pinches
    QList<int> coverLane_;  // per edit_.masks entry: 0, 1, or -1 when hidden
    int lanesUsed_ = 0, hiddenCovers_ = 0;
};
}
