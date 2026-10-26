#pragma once
#include "core/RecordingModel.h"
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

class QComboBox;
class QLabel;
class QMessageBox;
class QPushButton;
class QVBoxLayout;
class QShortcut;
class QSpinBox;
class QThread;
class QToolButton;
namespace putgd { class FileDialog; }
namespace putgd::Graphite { class WindowHeader; }

namespace putgd::record {
// Paints the preview frame scaled to fit and lets the user create, select,
// move and resize mask rectangles in canvas pixels, with the pointer or, with
// focus, the keyboard: arrows move the selected mask, Shift+arrows resize it
// (10 px steps, 1 px with Ctrl), Escape clears the selection.
class FrameView final : public QWidget {
    Q_OBJECT
public:
    explicit FrameView(QWidget* parent = nullptr);
    // A decoded frame; it also clears a preview error.
    void setFrame(const QImage&, qint64 timeUs);
    // A failed refresh: the message over the frame with Retry preview, and a
    // frame still shown dimmed and marked as an earlier one. Empty clears it.
    void setPreviewError(const QString& message);
    QString previewError() const { return error_; }
    bool stale() const { return !error_.isEmpty() && !frame_.isNull(); }
    bool hasFrame() const { return !frame_.isNull(); }
    qint64 frameTime() const { return timeUs_; }
    void setCanvas(const QSize& canvas) { canvas_ = canvas; update(); }
    void setMasks(const QList<RecordingMask>& masks, const QString& selected);
    void armCreate(const QString& kind) { armed_ = kind; setCursor(Qt::CrossCursor); update(); }
    void disarm() { armed_.clear(); unsetCursor(); update(); }
    const QString& armed() const { return armed_; }
    // Where the frame is drawn, in this widget's coordinates.
    QRect imageRect() const;
    // The mask Enter places: centred, half the frame's width and height.
    QRect defaultMaskRect() const;
    // Proposes a keyboard nudge that is still waiting for the keys to stop.
    void commitNudge();
    QSize sizeHint() const override { return {640, 360}; }
signals:
    void maskCreated(const QString& kind, const QRect& rect);
    void maskMoved(const QString& id, const QRect& rect);
    void maskSelected(const QString& id);
    void retryRequested();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    QPointF toCanvas(const QPointF&) const;
    QRectF toView(const QRect&) const;
    QRect bounds() const;
    void placeErrorCard();
public:
    // A widget pinned inside the frame's bottom edge (the review's status
    // row): it covers the frame only while shown and costs no layout height.
    void setFooterOverlay(QWidget* overlay) { footer_ = overlay; placeFooter(); }
    void placeFooter();
private:
    QPointer<QWidget> footer_;
    QImage frame_;
    qint64 timeUs_ = 0;
    QSize canvas_;
    QList<RecordingMask> masks_;
    QString selected_, armed_, error_;
    enum class Drag { None, Create, Move, Resize } drag_ = Drag::None;
    QPointF anchor_, current_;
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

// #283: the one timeline under the frame. The track is the whole clip with the
// retained interval between two trim handles; each mask's time span is a bar
// in a lane below it. Dragging the track scrubs, dragging a handle or a
// selected mask's edge retimes it, and a click on a bar selects that mask.
// It only shows and proposes: the window turns its signals into edits.
class ReviewTimeline final : public QWidget {
    Q_OBJECT
public:
    explicit ReviewTimeline(QWidget* parent = nullptr);
    void setDuration(qint64 us);
    void setTrim(qint64 start, qint64 end);
    void setMasks(const QList<RecordingMask>& masks, const QString& selected);
    void setPlayhead(qint64 us);
    qint64 playhead() const { return playhead_; }
    qint64 duration() const { return duration_; }
    // Lanes in use (masks that overlap in time take separate lanes, up to four).
    int lanes() const { return lanes_; }
    // Geometry for tests and the review session method: the track, a time's x
    // and the bar of a mask.
    QRect trackRect() const;
    int xFor(qint64 us) const;
    QRect maskRect(const QString& id) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
signals:
    void scrubbed(qint64 us);
    void scrubStarted();
    void trimChanged(qint64 start, qint64 end);
    void maskRetimed(const QString& id, qint64 start, qint64 end);
    void maskSelected(const QString& id);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    enum class Drag { None, Scrub, TrimStart, TrimEnd, MaskStart, MaskEnd, MaskMovePending, MaskMove };
    qint64 timeAt(qreal x) const;
    void layoutLanes();
    QRect laneRect(int lane) const;
    void seek(qint64 us);
    qint64 duration_ = 1, playhead_ = 0, trimStart_ = 0, trimEnd_ = 1;
    qint64 moveAnchor_ = 0;  // a moved mask: the press point's offset from its start
    qreal pressX_ = 0;       // where a mask-bar press began, for the drag distance
    QList<RecordingMask> masks_;
    QList<int> lane_;
    int lanes_ = 0;
    QString selected_;
    Drag drag_ = Drag::None;
    qint64 dragStart_ = 0, dragEnd_ = 0;
};

// The private review: scrub, trim, timed masks, then Upload or Save copy.
// Edits are proposed to the resident as events and the resident's answer
// (setEdit) is what this window displays; nothing uploads from here. Upload
// closes the window and the work continues in the background; Save copy
// keeps it open so the user can still upload afterwards.
class ReviewWindow final : public QWidget {
    Q_OBJECT
public:
    struct Options { QString workingClip; qint64 durationUs = 0; QSize canvas; QStringList formats; QJsonObject codecs; bool audioRecorded = false; };
    ReviewWindow(const Options&, const RecordingConfig&, QWidget* parent = nullptr);
    void setEdit(const RecordingEdit&);
    void setConfig(const RecordingConfig&);
    void setExporting(bool exporting, const QString& label);
    // An operational error in the status row, kept until dismissed or a new edit.
    void showError(const QString& message);
    // A standing banner above the frame (why the recording stopped); empty hides it.
    void setNotice(const QString& text);
    // Upload closes the window; a refused export brings it back with the reason.
    void reopen(const QString& error);
    // The resident's animation limits, for a warning before an export is refused.
    void setLimits(qint64 animationMaxUs, int animationMaxEdge);
    // #283: the review session method (recording.review) drives the window as a
    // user does: {action: inspect|seek|play|pause|speed|setIn|setOut|resetTrim|
    // addMask|selectMask|placeMask|setMaskIn|setMaskOut|strength|removeMask|
    // useSolidCover|format|trimToLimit|dismissStatus|retryPreview|
    // saveCopyDialog|closeFileDialog|discard|keepReviewing|confirmDiscard, ...}.
    // Edits are proposed exactly as the controls propose them; the answer is the
    // window's state (inspect) after the action, or an error.
    struct DriveResult { QString code, message; QJsonObject state; };
    DriveResult drive(const QJsonObject& params);
    QJsonObject inspect() const;
    // The default size that fits a 1366×768 desktop with its panel and title bar.
    // 960x600 under the desktop's title bar; with put.gd's frame the window also
    // takes the title bar's height, for its compact header (still within the
    // 1366x768 fit: a panel plus 72 px to spare).
    static QSize defaultSize();
signals:
    void editProposed(quint64 revision, const QJsonArray& commands);
    void configChanged(const RecordingConfig&);
    void uploadRequested(quint64 revision);
    void saveCopyRequested(const QString& path, quint64 revision);
    void cancelRequested();
protected:
    void closeEvent(QCloseEvent*) override;
    void hideEvent(QHideEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    // One line of the status row: an error, warning or notice with the actions
    // that resolve it; each action is the drive() call the button makes.
    struct StatusEntry {
        QString text, tone;
        QList<QPair<QString, QJsonObject>> actions;
        bool operator==(const StatusEntry&) const = default;
    };
    QList<StatusEntry> statusEntries() const;
    void refreshStatus();
    // Discard, the close button and the drive's discard ask before anything is
    // lost; recording.cancel (an agent's explicit cancel) never asks.
    void askDiscard();
    void answerDiscard(bool discard);
    void useSolidCover();
    // Save copy…: the desktop's chooser (#259); false while one is open or exporting.
    bool chooseCopy();
    void refreshFrame();
    // Silent playback of the retained interval from the scrubber: frames are
    // decoded in order on a worker and shown at their clip time; any scrub or
    // edit control pauses it. The export preview has no audio path.
    void startPlayback();
    void stopPlayback();
    void refreshMaskList();
    void refreshLabels();
    void refreshTimeline();
    void propose(const QJsonArray& commands);
    void selectMask(const QString& id);
    void armMask(const QString& kind);
    void disarm();
    void addMaskAt(const QString& kind, const QRect& rect);
    // Trim and mask in/out at the playhead (the toolbar, I/O and Shift+I/O);
    // the refusal shown in the status, or empty when proposed.
    QString setTrimAt(bool start);
    QString setMaskAt(bool start);
    void setStrength(int value);
    void removeSelected();
    qint64 playhead() const;
    void seek(qint64 us);
    RecordingMask* selectedMask();
    const RecordingMask* selectedMask() const;
    QString timeText(qint64 us) const;
    Options options_;
    RecordingConfig config_;
    RecordingEdit edit_;
    Graphite::WindowHeader* header_ = nullptr;
    FrameView* view_ = nullptr;
    ReviewTimeline* timeline_ = nullptr;
    QLabel *notice_ = nullptr, *time_ = nullptr, *retained_ = nullptr, *strengthLabel_ = nullptr, *audio_ = nullptr;
    // Errors, warnings and privacy guidance with their actions; shown only when there are any.
    QWidget* statusRow_ = nullptr;
    QVBoxLayout* statusLines_ = nullptr;
    QList<StatusEntry> shownStatus_;
    bool blurTipDismissed_ = false;  // the blur privacy guidance, acknowledged once
    StatusEntry message_;
    QPointer<QMessageBox> discard_;
    QToolButton* play_ = nullptr;
    QComboBox* speed_ = nullptr;
    // The worker deletes itself when it finishes; the guard forgets it then, so
    // closing the window after a preview ended never waits on a freed thread.
    QPointer<QThread> playThread_;
    std::shared_ptr<std::atomic_bool> playStop_;
    bool playing_ = false;
    QToolButton *trimStart_ = nullptr, *trimEnd_ = nullptr, *resetTrim_ = nullptr, *addBlur_ = nullptr, *addSolid_ = nullptr, *removeMask_ = nullptr, *maskStart_ = nullptr, *maskEnd_ = nullptr;
    QPushButton *upload_ = nullptr, *save_ = nullptr, *cancel_ = nullptr;
    FileDialog* chooser_ = nullptr;
    QWidget *maskTools_ = nullptr;
    QShortcut *escape_ = nullptr, *place_ = nullptr, *placeEnter_ = nullptr;
    QSpinBox* strength_ = nullptr;
    QComboBox* format_ = nullptr;
    QComboBox* masks_ = nullptr;
    QString selectedMask_;
    QTimer frameTimer_;
    bool exporting_ = false, closing_ = false;
    qint64 animationMaxUs_ = 0;
    int animationMaxEdge_ = 0;
public:
    ~ReviewWindow() override;
};
}
