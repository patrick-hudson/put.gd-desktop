#pragma once
#include "core/KeptSpans.h"
#include "core/RecordingModel.h"
#include "MaskNames.h"
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
// Nothing is written over the picture: guidance lives in the window's
// banners above it.
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
    bool skimming() const { return !skim_.isNull(); }
    // A failed refresh: the message over the frame with Retry preview, and a
    // frame still shown dimmed and marked as an earlier one. Empty clears it.
    void setPreviewError(const QString& message);
    QString previewError() const { return error_; }
    bool stale() const { return !error_.isEmpty() && !frame_.isNull(); }
    bool hasFrame() const { return !frame_.isNull(); }
    qint64 frameTime() const { return timeUs_; }
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
    // #1613: the width an overlay covers at the right; the picture fits the
    // rest, left-aligned, so nothing plays under it.
    void setRightInset(int px);
    int rightInset() const { return rightInset_; }
    // The cover box Enter places: centred, half the frame's width and height.
    QRect defaultMaskRect() const;
    // Proposes a keyboard nudge that is still waiting for the keys to stop.
    void commitNudge();
    bool nudging() const { return nudging_; }
    QSize sizeHint() const override { return {640, 360}; }
signals:
    void maskCreated(const QString& kind, const QRect& rect);
    void maskMoved(const QString& id, const QRect& rect);
    void maskSelected(const QString& id);
    void retryRequested();
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
    void placeErrorCard();
    void announce();
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
    QPointF anchor_, current_, panFrom_;
    QRect original_;
    // Keyboard nudges show at once and are proposed as one edit once the keys
    // stop, so a held arrow never races the resident's revisions.
    QTimer nudge_;
    QRect nudgeFrom_;
    bool nudging_ = false;
    QWidget* errorCard_ = nullptr;
    QLabel* errorText_ = nullptr;
    QPushButton* retry_ = nullptr;
};

// #1613: the private review (design: put.gd-evidence record
// 2026-11-17-recording-review-redesign, design.md). Title; tool row (Select V,
// Cut C, Redact X, Blur B, Undo, Redo, Fit · n%, and the Edits toggle when the
// panel folds); the viewer with its banners along the top; the transport; the
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
    struct Options { QString workingClip; QString destination, sharingMode, uploadSummary; qint64 durationUs = 0; QSize canvas; QStringList formats; QJsonObject codecs; bool audioRecorded = false; QSize availableSize;
        // #1364: what the recorded track holds; with neither set, a recorded track is system audio.
        bool systemRecorded = false, microphoneRecorded = false; };
    ReviewWindow(const Options&, const RecordingConfig&, QWidget* parent = nullptr);
    ~ReviewWindow() override;
    void setEdit(const RecordingEdit&);
    void setConfig(const RecordingConfig&);
    void setExporting(bool exporting, const QString& label);
    void setSavedCopy(const QString& path, quint64 revision, const RecordingConfig& config);
    QSize minimumSizeHint() const override;
    // An operational error above the viewer, kept until dismissed or a new edit.
    void showError(const QString& message, bool retryExport = true);
    // A standing banner above the viewer (why the recording stopped); empty hides it.
    void setNotice(const QString& text);
    // Upload closes the window; a refused export brings it back with the reason.
    void reopen(const QString& error);
    // The resident's animation limits, for a warning before an export is refused.
    void setLimits(qint64 animationMaxUs, int animationMaxEdge);
    // #283: the review session method (recording.review) drives the window as a
    // user does: {action: inspect|seek|play|pause|speed|setIn|setOut|resetTrim|
    // addMask|selectMask|placeMask|setMaskIn|setMaskOut|strength|removeMask|
    // useSolidCover|format|trimToLimit|dismissStatus|showAdvice|showInFolder|
    // copyPath|dismissSavedCopy|retryExport|retryPreview|saveCopyDialog|
    // closeFileDialog|discard|keepReviewing|confirmDiscard, ...}.
    // Edits are proposed exactly as the controls propose them; the answer is the
    // window's state (inspect) after the action, or an error.
    struct DriveResult { QString code, message; QJsonObject state; };
    DriveResult drive(const QJsonObject& params);
    QJsonObject inspect() const;
    // 1440×900 (#1613: the 1366×768 rule is lifted for this window), bounded by
    // the work area when the window first shows.
    static QSize defaultSize();
    // Below this the right panel folds into the Edits toggle.
    static QSize foldBelow() { return {1180, 720}; }
signals:
    void editProposed(quint64 revision, const QJsonArray& commands);
    void configChanged(const RecordingConfig&);
    void uploadRequested(quint64 revision);
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
    bool chooseCopy();
    void upload();
    void refreshFrame();
    // Silent playback of the kept footage only: each kept span is opened at
    // its start (never decoded through a cut) and frames are shown at their
    // output time; the end of the trim stops it. Any seek or edit pauses it.
    void startPlayback(std::optional<qint64> fromUs = std::nullopt);
    void stopPlayback();
    void propose(const QJsonArray& commands);
    // Time and selection.
    qint64 playhead() const { return playhead_; }
    void seek(qint64 us, bool user = true);
    void stepFrames(int frames);
    void stepTime(qint64 us);
    qint64 frameUs() const;
    qint64 frameAt(qint64 index) const;
    std::optional<qint64> firstFrameIn(const recording::Span&) const;
    std::optional<qint64> lastFrameIn(const recording::Span&) const;
    std::optional<qint64> firstKeptFrame(qint64 start, qint64 end) const;
    std::optional<qint64> lastKeptFrame(qint64 start, qint64 end) const;
    void setTimeSelection(std::optional<recording::Span>);
    void selectEdit(const QString& kind, const QString& id);
    void selectCover(const QString& id, bool seekToIt = true);
    void selectCut(const QString& id, bool seekToIt);
    void clearSelection();
    void stepEdit(int direction);
    QList<QPair<QString, QString>> editsInOrder() const;
    // Edits.
    void cutSelection();
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
    bool proposalPending_ = false, uploadAfterAnswer_ = false;
    std::optional<qint64> edgePreview_;  // a dragged bracket or edge: the frame shown
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
    QTimer tileTimer_;
    QLabel *notice_ = nullptr, *time_ = nullptr, *clipTime_ = nullptr, *silent_ = nullptr;
    QWidget* noticeBand_ = nullptr;
    ViewerBanner *placeBanner_ = nullptr, *blurBanner_ = nullptr;
    QList<StatusEntry> shownStatus_;
    StatusEntry message_;
    QPointer<QMessageBox> discard_;
    QToolButton *select_ = nullptr, *cut_ = nullptr, *redact_ = nullptr, *blur_ = nullptr, *undo_ = nullptr, *redo_ = nullptr, *pictureZoom_ = nullptr, *editsToggle_ = nullptr;
    QToolButton *play_ = nullptr, *previousFrame_ = nullptr, *nextFrame_ = nullptr, *timelineZoom_ = nullptr;
    QToolButton *format_ = nullptr, *destinationMenu_ = nullptr;
    QMenu *speedMenu_ = nullptr, *formatMenu_ = nullptr;
    QActionGroup *speeds_ = nullptr, *formats_ = nullptr;
    QAction* includeAudio_ = nullptr;
    QLabel* destinationFull_ = nullptr;
    QPushButton *errorDetails_ = nullptr, *upload_ = nullptr, *save_ = nullptr, *cancel_ = nullptr;
    FileDialog* chooser_ = nullptr;
    QTimer frameTimer_;
    // The worker deletes itself when it finishes; the guard forgets it then, so
    // closing the window after a preview ended never waits on a freed thread.
    QPointer<QThread> playThread_;
    std::shared_ptr<std::atomic_bool> playStop_;
    bool playing_ = false;
    double speed_ = 1.0;
    bool exporting_ = false, closing_ = false;
    qint64 animationMaxUs_ = 0;
    int animationMaxEdge_ = 0;
};
}
