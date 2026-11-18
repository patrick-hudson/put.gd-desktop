#pragma once
#include "core/KeptSpans.h"
#include "core/RecordingModel.h"
#include "MaskNames.h"
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
    // Zoom levels 1 (Fit), 2, 4, 8, 16. Zooming keeps `anchorUs` under the
    // same x (Ctrl+wheel passes the pointer's time; the zoom button passes the
    // playhead). Above Fit the strip scrolls horizontally (wheel / Shift+wheel)
    // and follows the playhead while playing.
    void setZoom(int level, std::optional<qint64> anchorUs = std::nullopt);
    int zoom() const { return zoom_; }
    // Paused hover at a source time, as the pointer over the ruler or strip
    // makes it (skimMoved, then skimRested after ~120 ms), or nullopt for the
    // pointer leaving (skimLeft). recording.review skim / skimEnd call it.
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
    // {zoom, visible: {start, end}, selection, selectedEdit: {kind, id},
    //  skim: {time, exact} | null, lanes, hiddenCovers, tiles: {shown, placeholders}}
    QJsonObject inspect() const;
    // The things a pointer or a screen reader can name: both trim handles, every
    // cut, every shown cover bar, the "+n" chip, and the edge handles of the
    // selected cut or cover. Tooltips and the accessible children come from it.
    // kind: trim-start | trim-end | cut | cut-start | cut-end | cover |
    // cover-start | cover-end | more.
    struct Part { QString kind, id, name; QRect rect; };
    QList<Part> parts() const;
    QSize sizeHint() const override;         // height 142 at normal text
    QSize minimumSizeHint() const override;
signals:
    // A click on the ruler or the strip (not on a handle or a chip): seek and
    // clear any selection. A drag on the ruler scrubs: scrubStarted, then
    // seekRequested as it moves.
    void seekRequested(qint64 sourceUs);
    void scrubStarted();
    // Dragging a trim bracket: the edge time while dragging (the viewer shows
    // the frame the clip will start or end on), then trimProposed on release.
    void trimEdgeDragged(bool start, qint64 sourceUs);
    void trimProposed(qint64 startUs, qint64 endUs);
    // A drag on the strip, or [ and ] at the playhead, makes a time selection.
    void timeSelected(qint64 startUs, qint64 endUs);
    void selectionCleared();  // Esc, or a click that seeks
    // A click on a cut or a cover bar selects it.
    void cutSelected(const QString& id);
    void coverSelected(const QString& id);
    // Dragging a selected cut's edge, or a selected cover bar's end, on release.
    void cutAdjusted(const QString& id, qint64 startUs, qint64 endUs);
    void coverRetimed(const QString& id, qint64 startUs, qint64 endUs);
    // Dragging a selected cut's edge or a selected cover bar's end: kind "cut" |
    // "cover", the edge time while dragging (the viewer shows that frame, as
    // trimEdgeDragged does for the brackets).
    void editEdgeDragged(const QString& kind, const QString& id, bool start, qint64 sourceUs);
    // An edge drag (trim, cut or cover) began; then it was released, changed or not.
    void edgeDragStarted();
    void edgeDragFinished();
    // Paused hover over the ruler or strip. skimMoved at once with the time
    // under the pointer (the window shows the tile); skimRested after ~120 ms
    // without movement (the window asks FrameSkimmer for the exact frame);
    // skimLeft when the pointer leaves (the window restores the playhead frame).
    void skimMoved(qint64 sourceUs);
    void skimRested(qint64 sourceUs);
    void skimLeft();
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
    bool event(QEvent*) override;  // tooltips
private:
    enum class Drag { None, Scrub, TrimStart, TrimEnd, CutStart, CutEnd, CoverStart, CoverEnd, Strip, Select };
    struct Handle { Drag drag = Drag::None; QString id; };
    Handle handleAt(QPointF) const;
    QString cutAt(QPointF) const;
    QString coverAt(QPointF) const;
    qint64 frameUs() const;
    qint64 snap(qint64 us) const;
    qint64 visibleLength() const;
    int stripLeft() const;
    int stripWidth() const;
    void setScroll(qint64 startUs);
    void revealPlayhead();
    void layoutCovers();
    void selectLocally(const QString& kind, const QString& id);
    void clearSkim();
    void clearSelections();
    void restartSkimTimer();
    QRect laneRect(int lane) const;
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
    Drag drag_ = Drag::None;
    QString dragId_;
    qint64 dragStart_ = 0, dragEnd_ = 0, anchorUs_ = 0;
    qreal pressX_ = 0;
    int wheelRemainder_ = 0;
    QList<int> coverLane_;  // per edit_.masks entry: 0, 1, or -1 when hidden
    int lanesUsed_ = 0, hiddenCovers_ = 0;
};
}
