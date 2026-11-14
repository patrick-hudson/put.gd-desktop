#pragma once
#include "FrameTap.h"
#include "ImageCopySource.h"
#include "MutterScreenCast.h"
#include "Overlays.h"
#include "ScrollBar.h"
#include "ScrollInjector.h"
#include "X11EscapeWatch.h"
#ifdef PUTGD_RECORD_KDE_SCREENCAST
#include "KdeScreencast.h"
#endif
#include "core/Error.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QWindow>
#include <QRect>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace putgd { class LayerShellRegionOverlay; }
namespace putgd::record {
// The helper's scroll-capture session (#1376, rebuilt on the tracker for
// #1515), the second session kind of putgd-record. It reuses the recording
// session's region selectors, ready ring and bar placement and the same live
// sources, with the cursor hidden; every frame goes to the tracker
// (ScrollSession) on a worker thread, whoever scrolls. The image is spooled to
// a private file in the staging directory with room at both ends and written
// as one PNG at the end; only its descriptor crosses the control pipe.
//
// Commands: prepare {id, staging, rect?, maxHeight, maxPixels?, fromTop,
// autoStart, selection, driver: "user"|"putgd", finishKey?}, select {rect},
// start, autoScroll {on}, pause and resume (auto-scroll off and on, the names
// of #1514), stop (Done), cancel, overlay {visible, rect?, inside?}. Events:
// selected, progress, finished, cancelled, failed (ScrollBackend.h).
//
// Drivers. The user's: nothing is injected and no surface of the helper's
// lies over the page but the ring outside it (and the bar where it has to sit
// inside, #1519). Auto-scroll: wheel steps and pointer motion only, only while
// it is on, and nothing after stop, cancel or the end of the control pipe.
// Done is the bar's button, the `stop` command, the resident's `scroll`
// hotkey and, on X11, Escape.
//
// While auto-scroll is on the pointer is the user's the moment they touch it
// (#1514). Between wheel steps it rests on a surface of the helper's own,
// outside the rectangle: the bar where it is beside the rectangle, else a
// small clear pad at the rest spot (not on GNOME, where only the Shell places
// windows). The helper knows where it last put the pointer; that surface
// reporting it anywhere else, or leaving when no visit into the rectangle was
// sent, is the user, and so is (X11) a pointer that is not where XTest left it,
// and (layer shell) the pointer anywhere on a clear surface over the output
// that leaves only the wheel points to the page. Then nothing more is
// injected: auto-scroll is paused and the capture follows the user, until the
// bar's Auto-scroll (after the mouse has been still for a moment) or the
// `autoScroll` command turns it on again.
class ScrollCapture final : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    explicit ScrollCapture(QObject* parent = nullptr);
    ~ScrollCapture() override;
    void command(const QString& command, const QJsonObject& params, Reply);
    // The control pipe closed or the helper is leaving: from here not even the
    // pointer's way back is injected, and the files of a capture that had not
    // finished are removed. A finished PNG stays for the resident.
    void abandon();
signals:
    void event(const QJsonObject&);
private:
    enum class State { Idle, Selecting, Ready, Starting, Capturing, Done, Cancelled, Failed };
    struct Outcome;
    struct Job;
    struct Gate;
    void prepare(const QJsonObject& params, Reply);
    Error openSelector();
    void closeSelector();
    // The selector (or the resident's `select`) produced a rectangle.
    void selected(const QRect& rect);
    Error setRect(const QRect& rect);
    void showBar();
    void start(Reply);
    void begin(const FrameTap::Source&, Reply);
    void stop(Reply);
    void cancel(Reply);
    // The user moved the mouse while auto-scroll was on (#1514).
    void pause(const QString& by);
    // Auto-scroll on (quiet: from the bar, once the mouse has been still for
    // resumeQuietMs) or off.
    void setAutoScroll(bool on, bool quiet);
    void resume(bool quiet);
    void resumeNow();
    void autoOff();
    // On the main thread, before auto-scroll's first wheel step of a run: the
    // rest and (layer shell) the watch with holes at the wheel points.
    void prepareAuto();
    // Auto-scroll is off: no rest pad, no watch.
    void dropAutoSurfaces();
    // The session's progress, on the main thread.
    void sessionProgress(const QJsonObject&);
    QJsonObject pauseView() const;
    void emitProgress();
    // Where the pointer rests between wheel steps, and the surface that sees it.
    void placeRest();
    void pointerSeen(bool watch, std::optional<QPointF> local, quint64 stamp);
    void userMoved(bool pulledBack, std::optional<QPointF> at, bool atIsBase = false);
    // Layer-shell desktops: a clear surface over the rectangle's output that
    // takes the pointer everywhere but at the wheel points (desktop logical
    // coordinates), the bar and the pad, so that the user's motion is seen
    // while the pointer is in the rectangle for a wheel step too.
    void showWatch(const QList<QPointF>& holes);
    bool eventFilter(QObject*, QEvent*) override;
    void finish(std::shared_ptr<Outcome>);
    void fail(const Error&);
    void release();
    void removeFiles();
    static void work(std::shared_ptr<Job>);

    State state_ = State::Idle;
    QString id_, staging_, frames_, selectionMode_;
    QRect rect_, screen_;
    QString screenName_;
    int maxHeight_ = 0;
    qint64 maxPixels_ = 0;
    bool fromTop_ = false, autoStart_ = true, barHidden_ = false, barOutside_ = false;
    // The driver at the start (auto-scroll on), auto-scroll as the session has
    // it, and the bar inside the rectangle (#1519; its desktop rectangle).
    bool autoAtStart_ = false, autoOn_ = false, barInside_ = false;
    QRect insideBar_;
    QString finishKey_;
    bool stopRequested_ = false, cancelRequested_ = false;
    Reply selectReply_, stopReply_, cancelReply_;
    QJsonObject finished_;
    SelectionOverlay* selection_ = nullptr;
    LayerShellRegionOverlay* layerSelection_ = nullptr;
    BoundsOverlay bounds_;
    // KWin: damage on the region's output every heartbeatMs while frames are read (BoundsOverlay::showHeartbeat).
    QTimer heartbeat_;
    ScrollBar* bar_ = nullptr;
    // The rest pad where the bar is not beside the rectangle, and the window
    // whose pointer events tell the user's motion from the capture's.
    QWidget* pad_ = nullptr;
    QWidget* watch_ = nullptr;
    QPointer<QWindow> restWindow_, watchWindow_;
    QPointF restLocal_, restOrigin_;
    // The last pointer event seen on a surface of the helper's (desktop
    // logical), the motion count then, and the pointer's last arrival at rest.
    QPointF seen_;
    quint64 seenSeq_ = 0, watchSeqBase_ = 0, arrivalStamp_ = 0;
    qint64 arrivedMs_ = -1;
    bool haveSeen_ = false, lastWasArrival_ = false;
    // GNOME: where the Shell put the bar (the resident's `overlay` rect).
    QRect shellBar_;
    // Why auto-scroll handed the wheel back: pointer, scrolled, lost, no_movement.
    QString pausedBy_, progressState_;
    QJsonObject lastProgress_;
    QTimer resumeQuiet_;
    bool resumePending_ = false;
    MutterScreenCast mutter_;
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    KdeScreencast kde_;
#endif
    std::unique_ptr<ImageCopySource> imageCopy_;
    std::unique_ptr<ScrollInjector> injector_;
    FrameTap tap_;
    // X11: Escape stops the capture and keeps the image, whatever has the focus.
    X11EscapeWatch escape_;
    std::shared_ptr<Job> job_;
    std::thread worker_;
};
}
