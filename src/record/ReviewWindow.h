#pragma once
#include "core/KeptSpans.h"
#include "core/RecordingModel.h"
#include "MaskNames.h"
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <limits>
#include <tuple>

class QAction;
class QActionGroup;
class QHBoxLayout;
class QLabel;
class QMenu;
class QMessageBox;
class QPushButton;
class QVBoxLayout;
class QThread;
class QToolButton;
namespace putgd { class FileDialog; }
namespace putgd::Ui { class WindowHeader; }

namespace putgd::record {
class EditsOverlay;
class Filmstrip;
class FilmstripTimeline;
class FrameSkimmer;
class ReviewPanel;
class ViewerBanner;

// Paints the frame at the playhead (or the skim position) scaled to fit, or at
// 100 % (one source pixel per device pixel) with drag and arrow panning, and
// lets the user draw a cover's box, select a cover and move or resize it, in
// canvas pixels, with the pointer or, with focus, the keyboard: arrows move
// the selected cover, Shift+arrows resize it (1 px steps, 10 px with Ctrl).
// The selected cover's corners (16 × 16 px targets) and a band along each side
// (6 px outside; inside, 6 px on a box at least 40 px that way, else 4) resize
// it from the opposite side, flipping past
// it; its body moves it. A press on an unselected cover selects it (a move
// then needs 8 px); moves and resizes of the selected one start 4 px from the
// press and apply the release point; Esc, a right press, Ctrl+Z or a lost
// window reverts the gesture (pan included). While playing a click only pauses.
// A click while placing places a default box at it; the arrows belong to the
// viewer only after a click on a cover or Tab into it.
// While a cover moves, its effect follows the box (the "live" cover): the
// window supplies the frame without that cover, and until it arrives the
// shown frame keeps the old place covered under a pending outline.
// Nothing is written over the picture: guidance lives in the window's tool
// row (placement, blur warning, a frame error with Retry preview).
class FrameView final : public QWidget {
    Q_OBJECT
public:
    explicit FrameView(QWidget* parent = nullptr, std::shared_ptr<MaskNames> names = {});
    // A decoded frame; it also clears a preview error and any skim image.
    void setFrame(const QImage&, qint64 timeUs);
    // #1613: the skimmer's picture (a tile, then the exact frame) shown in
    // place of the playhead frame until clearSkim(); the frame stays.
    void setSkim(const QImage&, qint64 timeUs);
    void clearSkim();
    void promoteSkim();
    void fadeOutSkim(int ms);
    // The cover a placing drag would make now (canvas px), or empty (F5).
    QRect placementPreview() const;
    void setLinkedHover(const QString& coverId);
    bool skimming() const { return !skim_.isNull(); }
    qint64 skimTime() const { return skimUs_; }
    // A failed refresh: a frame still shown is dimmed and marked as an earlier
    // one; the window shows the message with Retry preview. Empty clears it.
    void setPreviewError(const QString& message);
    QString previewError() const { return error_; }
    // The error with which frame is still shown, as the window's message says it.
    QString previewErrorText() const;
    bool stale() const { return !error_.isEmpty() && !frame_.isNull(); }
    bool hasFrame() const { return !frame_.isNull(); }
    qint64 frameTime() const { return timeUs_; }
    QSize frameSize() const { return frame_.size(); }
    QSize skimSize() const { return skim_.size(); }
    void setCanvas(const QSize& canvas) { canvas_ = canvas; update(); }
    void setMasks(const QList<RecordingMask>& masks, const QString& selected);
    void armCreate(const QString& kind);
    void disarm();
    const QString& armed() const { return armed_; }
    // Where the frame is drawn, in this widget's coordinates.
    QRect imageRect() const;
    void setPlaying(bool playing) { playing_ = playing; update(); }
    bool playing() const { return playing_; }
    // #1613: the picture's zoom: Fit, or 100 % of the source's pixels.
    void setActualSize(bool actual);
    bool actualSize() const { return actual_; }
    int zoomPercent() const;
    // #1613: pans a picture at 100 % by source pixels (the arrow keys and
    // recording.review viewerPan); the pan clamps so the picture stays in view.
    void panBy(const QPointF& sourcePixels);
    QPointF pan() const { return pan_; }
    // #1613: the width an overlay covers at the right; the picture fits the
    // rest, left-aligned, so nothing plays under it.
    void setRightInset(int px);
    int rightInset() const { return rightInset_; }
    // The cover box Enter places: centred, half the frame's width and height.
    QRect defaultMaskRect() const;
    // Proposes a keyboard nudge that is still waiting for the keys to stop.
    void commitNudge();
    // Drops a nudge not yet proposed: the cover goes back to where it was.
    void cancelNudge();
    bool nudging() const { return nudging_; }
    // A pointer drag (draw, move, resize or pan) is in progress.
    bool dragging() const { return drag_ != Drag::None; }
    // The live cover (D4): the frame without cover `id` at timeUs (asked for on
    // liveEditStarted), and the end of the live state once the frame for the
    // committed edit is shown (or the gesture changed nothing).
    void setUncovered(const QString& id, const QImage& image, qint64 timeUs);
    // Another cover changed under the live one: its base is pending again (W3).
    void dropUncovered();
    void endLive();
    // R29: the selected blur's strength while the panel coalesces it, shown at once.
    void previewStrength(const QString& id, int strength);
    QString liveId() const { return liveId_; }
    // {box: [x, y, w, h], grips: [[x, y] × 8, handle order]} for the selected cover, or {}.
    QJsonObject selectedGeometry() const;
    // Esc, a right press, Ctrl+Z or a lost window: the gesture in progress
    // goes back to its start (W5, W20). False with none.
    bool cancelGesture();
    // The viewer has the arrow keys (a click on a cover or Tab into it, W8).
    bool arrowsClaimed() const { return arrowsClaimed_; }
    // Placing waits for the frame it is drawn on (W6).
    void setPlaceWaiting(bool waiting);
    bool placeWaiting() const { return placeWaiting_; }
    // A click while placing: the box it places, centred on a canvas point.
    QRect defaultMaskRectAt(const QPointF& canvasPoint) const;
    bool livePending() const;
    // Geometry shared by hover, cursor and press (tests).
    static int handleAt(const QRectF& viewRect, const QPointF& p);
    static QRect resizedRect(const QRect& r, int handle, const QPoint& delta, const QRect& bounds);
    QSize sizeHint() const override { return {640, 360}; }
signals:
    void maskCreated(const QString& kind, const QRect& rect);
    void maskMoved(const QString& id, const QRect& rect);
    void maskSelected(const QString& id);
    // A cover shown at timeUs started to move: the window asks for the frame without it.
    void liveEditStarted(const QString& id, qint64 timeUs);
    // A click on the picture while playing (W3).
    void pauseRequested();
    // The cover under the pointer on the picture ("" when none) (Q22).
    void hoveredCoverChanged(const QString& id);
    void arrowsClaimedChanged(bool claimed);
    // Esc gave the arrows back: the window returns focus to the timeline.
    void arrowsReleased();
    void previewErrorChanged();
    void zoomChanged();
protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    QPointF toCanvas(const QPointF&) const;
    QRectF toView(const QRect&) const;
    QRect bounds() const;
    QSize source() const;
    QRect area() const;
    void clampPan();
    void announce();
    struct Target { enum class Kind { None, Handle, Body } kind = Kind::None; QString id; int handle = -1; };
    Target targetAt(const QPointF&) const;
    void updateHover(const QPointF&);
    void beginLive(const QString& id);
    const QImage& livePicture();
    void applyDrag(const QPointF&);
    bool pastDeadZone(const QPointF&) const;
    QRect placementAt(const QPointF& p) const;
    void claimArrows(bool);
    QImage frame_, skim_;
    qint64 timeUs_ = 0, skimUs_ = 0;
    QSize canvas_;
    QList<RecordingMask> masks_;
    std::shared_ptr<MaskNames> maskNames_;
    QString selected_, hovered_, armed_, error_;
    bool playing_ = false, actual_ = false;
    int rightInset_ = 0;
    QPointF pan_;  // at 100 %: the source point at the area's centre, minus the source centre
    enum class Drag { None, Create, Move, Resize, Pan } drag_ = Drag::None;
    QPointF anchor_, current_, panFrom_, pressAt_;
    QRect original_;
    QString dragId_;
    int handle_ = -1, hoveredHandle_ = -1;
    bool moved_ = false;  // the pointer passed the dead zone
    bool pressedSelected_ = false, arrowsClaimed_ = false, placeWaiting_ = false;
    QString liveId_, uncoveredId_;
    QImage uncovered_, composite_;
    qint64 uncoveredUs_ = -1;
    std::tuple<qint64, QRect, QString, int> compositeKey_;
    std::optional<int> liveStrength_;
    QString linkedHover_;
    QImage fading_;
    QElapsedTimer fadeClock_;
    int fadeMs_ = 100;
    // The last point the pointer was on a grip, and when it left it (Q23, Q24),
    // as the timeline keeps them (Q10t).
    std::optional<QPointF> gripAt_;
    QElapsedTimer gripLeft_;
    // Keyboard nudges show at once and are proposed as one edit once the keys
    // stop, so a held arrow never races the resident's revisions.
    QTimer nudge_;
    QRect nudgeFrom_;
    bool nudging_ = false;
};

// #1613: the private review (design: put.gd-evidence record
// 2026-11-17-recording-review-redesign, design.md). Title; tool row (Select V,
// Cut C, Redact X, Blur B, Undo, Redo, then the placement prompt, blur warning
// or frame error in its free middle, Fit · n%, and the Edits toggle when the
// panel folds); the viewer, with the notice and status lines over its top
// edge (they never refit the picture); the transport; the
// filmstrip timeline; the footer (format, Discard…, Save a copy…, destination,
// Upload). The right panel (ReviewPanel) runs from the tool row to the footer
// and folds into the Edits overlay below ~1180×720.
// Edits are proposed to the resident as events and the resident's answer
// (setEdit) is what this window displays; nothing uploads from here. Upload
// closes the window and the work continues in the background; Save a copy
// keeps it open so the user can still upload afterwards.
class ReviewWindow final : public QWidget {
    Q_OBJECT
public:
    // availableSize optionally supplies the embedding host's workspace; otherwise use QScreen.
    struct Options { QString workingClip; QString destination, sharingMode, uploadSummary; bool links = false;
        // #2079: the Seal toggle beside Upload, {available, on, reason?}; empty shows none.
        QJsonObject seal; qint64 durationUs = 0; QSize canvas; QStringList formats; QJsonObject codecs; bool audioRecorded = false; QSize availableSize;
        // #1364: what the recorded track holds; with neither set, a recorded track is system audio.
        bool systemRecorded = false, microphoneRecorded = false; };
    ReviewWindow(const Options&, const RecordingConfig&, QWidget* parent = nullptr);
    ~ReviewWindow() override;
    // The resident's edit. answer, when this push answers one of the window's
    // proposals: {proposal: n} once proposal n was applied, plus refused:
    // {code, message, revision} when it was not (the edit is then unchanged).
    void setEdit(const RecordingEdit&, const QJsonObject& answer = {});
    // seq: the window's latest output choice (configChanged's seq) the resident had
    // applied; an older config is ignored. False when ignored.
    bool setConfig(const RecordingConfig&, quint64 seq = 0);
    void setExporting(bool exporting, const QString& label);
    void setSavedCopy(const QString& path, quint64 revision, const RecordingConfig& config);
    QSize minimumSizeHint() const override;
    // An operational error above the viewer, kept until dismissed or a new edit.
    void showError(const QString& message, bool retryExport = true);
    // A standing banner above the viewer (why the recording stopped); empty hides it.
    void setNotice(const QString& text);
    // Upload closes the window; a refused export brings it back with the reason.
    void reopen(const QString& error, bool retry = true);
    // The resident's animation limits, for a warning before an export is refused.
    void setLimits(qint64 animationMaxUs, int animationMaxEdge);
    // #283: the review session method (recording.review) drives the window as a
    // user does: {action: inspect|seek|play|pause|speed|setIn|setOut|setTrim|resetTrim|
    // addMask|selectMask|placeMask|setMaskIn|setMaskOut|strength|removeMask|
    // useSolidCover|format|trimToLimit|dismissStatus|showAdvice|showInFolder|
    // copyPath|dismissSavedCopy|retryExport|retryPreview|saveCopyDialog|
    // closeFileDialog|discard|keepReviewing|confirmDiscard, and (#1613) selectRange|
    // clearSelection|cut|selectCut|adjustCut|restoreCut|playAcross|undo|redo|armRedact|
    // armBlur|cancelPlacement|selectEdit|nextEdit|previousEdit|seekCoverStart|
    // seekCoverEnd|setCoverEdgeToPlayhead|setCoverEdge|coverKind|timelineZoom|
    // timelineFit|viewerZoom|viewerPan|frameStep|skim|skimEnd|panel|resize, and the
    // feel controls: setSkimming|setSnapping {on}|zoomIn|zoomOut|retimeCover|
    // moreCovers|markSelection {edge}|editPoint {delta}|setCutEdge {cutId, edge}|
    // scrollTimeline {start}; placeMask {at} places the default box a click
    // places (exactly one of rect and at), ...}.
    // Edits are proposed exactly as the controls propose them; the answer is the
    // window's state (inspect) after the action, or an error. A state that
    // proposed edits carries their numbers in "proposals"; inspect's lastRefusal
    // {code, message, revision, proposal} names the last one the resident refused.
    struct DriveResult { QString code, message; QJsonObject state; };
    DriveResult drive(const QJsonObject& params);
    QJsonObject inspect() const;
    // 1440×900 (#1613: the 1366×768 rule is lifted for this window), bounded by
    // the work area when the window first shows.
    static QSize defaultSize();
    // Below this the right panel folds into the Edits toggle.
    static QSize foldBelow() { return {1180, 720}; }
signals:
    // proposal numbers the window's proposals 1, 2, …; the resident's answer names it.
    void editProposed(quint64 revision, const QJsonArray& commands, quint64 proposal);
    // seq numbers the window's output choices 1, 2, …; the resident echoes the last it applied.
    void configChanged(const RecordingConfig&, quint64 seq);
    // The revision and the output the window shows; the resident refuses any other.
    void uploadRequested(quint64 revision, const RecordingConfig& config);
    // #2079: the Seal toggle changed; the resident applies it when the upload starts.
    void sealToggled(bool on);
    void retryExportRequested();
    void saveCopyRequested(const QString& path, quint64 revision);
    void cancelRequested();
protected:
    void closeEvent(QCloseEvent*) override;
    void hideEvent(QHideEvent*) override;
    void showEvent(QShowEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
private:
    // One status line above the viewer: an error, warning or notice with the
    // actions that resolve it; each action is the drive() call the button makes.
    struct StatusEntry {
        QString text, tone;
        QList<QPair<QString, QJsonObject>> actions;
        bool operator==(const StatusEntry&) const = default;
    };
    // A cover being placed: armed by X/B (or the panel), drawn on the viewer.
    struct Placement { QString kind; recording::Span span; bool whole = false; };
    QList<StatusEntry> statusEntries() const;
    void refreshStatus();
    void refreshAll();
    void refreshTransport();
    void refreshTools();
    void refreshFooter();
    void refreshPanel();
    void refreshBanners();
    void refreshFold();
    void placeOverlayInset();
    // Discard, the close button and the drive's discard ask before anything is
    // lost; recording.cancel (an agent's explicit cancel) never asks.
    void askDiscard();
    void answerDiscard(bool discard);
    // Save a copy…: the desktop's chooser (#259); false while one is open or exporting.
    bool chooseCopy(std::optional<quint64> refusalsAtPress = std::nullopt);
    // Upload, Ctrl+Enter and Enter: never during a pointer drag or while placing;
    // with proposals unanswered, only once the resident accepted the last of
    // them (a refusal or 2 s without an answer cancels it, with a status line).
    void upload(std::optional<quint64> refusalsAtPress = std::nullopt);
    bool tryUpload();  // sends once nothing is unanswered; true when it did
    // The one way a waiting upload stops, with its own status line (why: what happened).
    void cancelUpload(const QString& why = {});
    // Save a copy waits, as Upload does, for the changes made before it (W5).
    void trySaveCopy();
    void cancelSaveCopy(const QString& why);
    void emitConfig();
    int viewEdge() const;
    // The frame at the playhead (or a dragged edge) from the frame worker: asked
    // at once (leading edge), one decode in flight, the newest time next; the
    // viewer keeps the last frame meanwhile (#1613 C1, C2).
    void requestFrame();
    void askFrame();
    void showFrameError(qint64 at, const QString& message);
    // The same frame decoded on this thread, for Set Out / Set to playhead when
    // the frame at the playhead isn't shown yet (they need its own time).
    void decodeFrameNow();
    void placeBanners();
    void beginEdgePreview();
    void endEdgePreview();
    bool cancelDrags();
    void stopPlaceWait();
    void showNote(const QString& text, int ms);
    void refreshNote();
    void noteUndo();
    bool blurApplies() const;
    recording::Span placementSpan(const QString& kind, bool& whole) const;
    void stepEditPoint(int direction);
    bool adoptSkim();
    qint64 shownFrameStart();
    void placePendingEnter();
    void revealHiddenCovers();
    DriveResult driveAction(const QJsonObject& params);
    // Silent playback of the kept footage only: each kept span is opened at
    // its start (never decoded through a cut) and frames are shown at their
    // output time; the end of the trim stops it. Any seek or edit pauses it.
    // resume (a zoom, speed or edit restart): with nothing kept ahead it stays paused, never wrapping.
    void startPlayback(std::optional<qint64> fromUs = std::nullopt, bool resume = false);
    void stopPlayback();
    void propose(const QJsonArray& commands);
    // Time and selection.
    qint64 playhead() const { return playhead_; }
    void seek(qint64 us, bool user = true);
    void stepFrames(int frames);
    void stepTime(qint64 us);
    qint64 frameUs() const;
    qint64 frameIndex(qint64 us) const;
    qint64 nextFrameEdge(qint64 at) const;   // the edge after the frame at `at`
    qint64 previousFrame(qint64 edge) const; // the frame before an end edge
    std::optional<qint64> shownFrameEnd();   // the edge after the frame shown at the playhead
    QString refuseChange(const QString& message);
    qint64 frameAt(qint64 index) const;
    std::optional<qint64> firstFrameIn(const recording::Span&) const;
    std::optional<qint64> lastFrameIn(const recording::Span&) const;
    std::optional<qint64> firstKeptFrame(qint64 start, qint64 end) const;
    std::optional<qint64> lastKeptFrame(qint64 start, qint64 end) const;
    void setTimeSelection(std::optional<recording::Span>);
    void selectTime(qint64 start, qint64 end);
    void selectEdit(const QString& kind, const QString& id);
    void selectCover(const QString& id, bool seekToIt = true);
    // The frame without live cover `id`, every other cover as the edit has it (D4, W3).
    void askUncovered(const QString& id, qint64 at);
    QString cutEdgeToPlayhead(const QString& id, bool start);
    void takeSkimForKey();
    void selectCut(const QString& id, bool seekToIt);
    void clearSelection();
    void stepEdit(int direction);
    QList<QPair<QString, QString>> editsInOrder() const;
    // Edits.
    void cutSelection();
    void proposeTrim(qint64 start, qint64 end);
    void adjustCut(const QString& id, qint64 start, qint64 end);
    void retimeCover(const QString& id, qint64 start, qint64 end);
    void setCoverKind(const QString& id, const QString& kind);
    void setPanelOpen(bool open);
    void arm(const QString& kind);
    void cancelPlacement();
    void placeCover(const QString& kind, const QRect& rect);
    void updateCover(const QString& id, const std::function<void(QJsonObject&)>& change);
    void restoreCut(const QString& id);
    void playAcross(const QString& id);
    void seekCoverEdge(const QString& id, bool start);
    QString setTrimAt(bool start);
    QString setMaskAt(bool start, const QString& id = {});
    void removeSelectedCover();
    void useSolidCover();
    void undoRedo(bool undo);
    RecordingMask* selectedMask();
    const RecordingMask* selectedMask() const;
    const RecordingMask* maskById(const QString& id) const;
    // Thumbnails and skim.
    void requestTiles();
    Options options_;
    RecordingConfig config_;
    bool audioIntent_ = false;
    // Keeps or drops the clip's one audio track, naming the sources it holds.
    void applyAudio();
    // "System audio", "Microphone" or "System audio and microphone".
    QString audioName() const;
    QString audioSummary() const;
    RecordingEdit edit_;
    recording::KeptSpans kept_;
    std::shared_ptr<MaskNames> maskNames_ = std::make_shared<MaskNames>();
    qint64 playhead_ = 0;
    std::optional<recording::Span> timeSelection_;
    QString selectedKind_, selectedId_;  // "cut" | "cover" | "trim" | ""
    std::optional<Placement> placing_;
    // After making a cut, the playhead lands 1 s before its seam once the
    // resident answers (unless something else moved the playhead first).
    std::optional<recording::Span> pendingCut_;
    quint64 pendingCutProposal_ = 0;  // the cut's proposal; only its answer moves the playhead
    // Proposals sent and answered (numbered from 1); the one an upload waits for.
    quint64 proposalSeq_ = 0, answeredSeq_ = 0;
    bool uploadWaiting_ = false;  // Upload pressed, its revision not yet fully answered
    StatusEntry uploadNotice_;    // "Upload cancelled: …" until dismissed or Upload again
    quint64 configSeq_ = 0;
    QString saveWaitingPath_;  // a chosen copy waiting for earlier changes to be answered
    quint64 saveAwaits_ = 0;
    quint64 refusals_ = 0, saveRefusalMark_ = 0;  // changes not applied, counted
    // refusals_ at a press on Save or Upload, and which (W5).
    std::optional<std::pair<const QWidget*, quint64>> pressRefusals_;
    std::optional<std::pair<const QWidget*, quint64>> lastPressRefusals_;  // kept for a double-click's second press (U1)
    QWidget* releasing_ = nullptr;  // Save or Upload while it handles a mouse release (U2, U3)
    std::optional<quint64> takePressRefusals(const QWidget* button);
    QTimer saveWait_;
    quint64 uploadAwaits_ = 0;  // the last proposal made before Upload: answers up to it are awaited
    qint64 shownFor_ = -1, shownActual_ = -1;  // the playhead a frame was decoded for, and its own time
    bool proposalPending() const { return answeredSeq_ < proposalSeq_; }
    QTimer uploadWait_;
    int shownEdge_ = 1280;  // the long edge frames are decoded at (viewEdge())
    QJsonObject lastRefusal_;
    // The exact skim frame asked for: its time and the edit revision it was asked at.
    std::optional<std::pair<qint64, quint64>> skimAsked_;
    void askSkimFrame(qint64 at);
    qint64 skimReach() const;
    // The last exact skim frames (B2): one answers any time from its request to
    // its frame's own time when no cover starts or ends in between.
    struct SkimFrame { qint64 requested, frameUs; quint64 revision; int edge; QImage image; };
    QList<SkimFrame> skimCache_;
    const SkimFrame* cachedSkim(qint64 at) const;
    void cacheSkim(SkimFrame frame);
    // The playhead frame asked for last (its time, revision and size).
    struct FrameAsk { qint64 at; quint64 revision; int edge; QString variant; };
    std::optional<FrameAsk> frameAsked_;
    quint64 shownRevision_ = 0;
    // The live cover (D4): the frame without it asked for, the proposal its
    // drag made, and the revision whose frame ends the live state.
    std::optional<std::pair<QString, qint64>> uncoveredAsked_;
    // Request tags (W3, W4): each live-base request and each cover edge
    // preview gets its own; answers under any other tag are dropped.
    quint64 requestTags_ = 0, uncoveredTag_ = 0, edgeTag_ = 0;
    quint64 nextRequestTag() { return std::numeric_limits<quint64>::max() - ++requestTags_; }
    quint64 cutFieldProposal_ = 0;  // the proposal a typed cut time made (W8)
    quint64 liveProposal_ = 0;
    std::optional<quint64> liveWaitRevision_;
    // C4: a trim proposal and whether it moved the start; its answer may bring the playhead in.
    quint64 pendingTrimProposal_ = 0;
    bool pendingTrimStart_ = false;
    std::optional<qint64> edgePreview_;  // a dragged bracket or edge: the frame shown
    std::optional<RecordingEdit> edgeEdit_;  // a cover edge drag: the edit with its proposed span (W4)
    quint64 edgeDragSeq_ = 0, edgeHoldProposal_ = 0;  // proposals before the drag; the one its release made
    QTimer edgeHoldTimer_, placeWaitTimer_, noteTimer_;
    QString transientNote_, placeHint_;
    QTimer placeHintTimer_, enterPlaceTimer_;
    bool enterPlacePending_ = false;
    QElapsedTimer skimMovedClock_;  // P11
    QTimer skimReturn_;  // F2
    std::optional<std::pair<QString, QString>> revealRow_;  // Q20
    int noteMs_ = 1500;
    bool loadingFrame_ = false;
    bool heldPlaying_ = false;  // T15: playing when the timeline held playback for a press
    quint64 undoProposal_ = 0;
    bool undoWasUndo_ = true;
    RecordingEdit undoBefore_;
    std::optional<qint64> skimUs_;
    QString savedPath_;
    quint64 savedRevision_ = 0;
    RecordingConfig savedConfig_;
    bool savedDismissed_ = false;
    QJsonObject lastShowInFolder_;
    bool savedCurrent() const;
    QSize availableReviewSize() const;
    void showAdvice();
    QPointer<QWidget> adviceDialog_;
    bool initialFitDone_ = false, folded_ = false;
    Ui::WindowHeader* header_ = nullptr;
    QWidget *toolRow_ = nullptr, *transport_ = nullptr, *footer_ = nullptr, *statusRow_ = nullptr;
    QVBoxLayout* statusLines_ = nullptr;
    FrameView* view_ = nullptr;
    FilmstripTimeline* timeline_ = nullptr;
    ReviewPanel* panel_ = nullptr;
    EditsOverlay* overlay_ = nullptr;
    Filmstrip* filmstrip_ = nullptr;
    FrameSkimmer* skimmer_ = nullptr;
    FrameSkimmer* frames_ = nullptr;  // the playhead's frames (its own decoder)
    QTimer tileTimer_;
    QLabel *notice_ = nullptr, *time_ = nullptr, *clipTime_ = nullptr, *silent_ = nullptr;
    QWidget* noticeBand_ = nullptr;
    ViewerBanner *placeBanner_ = nullptr, *blurBanner_ = nullptr, *frameError_ = nullptr, *toolNote_ = nullptr;
    QWidget* toolMessages_ = nullptr;
    QList<StatusEntry> shownStatus_;
    StatusEntry message_;
    QPointer<QMessageBox> discard_;
    QToolButton *select_ = nullptr, *cut_ = nullptr, *redact_ = nullptr, *blur_ = nullptr, *undo_ = nullptr, *redo_ = nullptr, *pictureZoom_ = nullptr, *editsToggle_ = nullptr;
    QToolButton *play_ = nullptr, *previousFrame_ = nullptr, *nextFrame_ = nullptr, *timelineZoom_ = nullptr, *skimToggle_ = nullptr, *snapToggle_ = nullptr, *playSpeed_ = nullptr;
    QMenu* timelineZoomMenu_ = nullptr;
    QWidget* bannerArea_ = nullptr;
    QToolButton *format_ = nullptr, *destinationMenu_ = nullptr;
    QMenu *speedMenu_ = nullptr, *formatMenu_ = nullptr;
    QActionGroup *speeds_ = nullptr, *formats_ = nullptr;
    QAction* includeAudio_ = nullptr;
    QLabel* destinationFull_ = nullptr;
    QPushButton* seal_ = nullptr; bool sealAvailable_ = false; QString sealReason_;
    QPushButton *errorDetails_ = nullptr, *upload_ = nullptr, *save_ = nullptr, *cancel_ = nullptr;
    FileDialog* chooser_ = nullptr;
    QTimer frameTimer_;  // only with PUTGD_RECORD_REVIEW_DELAY_MS (tests, the catalog): frames asked that long after a seek
    // The worker deletes itself when it finishes; the guard forgets it then, so
    // closing the window after a preview ended never waits on a freed thread.
    QPointer<QThread> playThread_;
    std::shared_ptr<std::atomic_bool> playStop_, playEnded_;
    bool playing_ = false;
    double speed_ = 1.0;
    bool exporting_ = false, closing_ = false;
    qint64 animationMaxUs_ = 0;
    int animationMaxEdge_ = 0;
};
}
