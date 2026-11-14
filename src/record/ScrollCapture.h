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
#include <QRect>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace putgd { class LayerShellRegionOverlay; }
namespace putgd::record {
// The helper's scroll-capture session (#1376), the second session kind of
// putgd-record. It reuses the recording session's region selectors, ready ring
// and bar placement and the same live sources, with the cursor hidden; the
// frames go to a stitcher instead of an encoder. After the rectangle is known
// it puts the pointer in the region, turns the wheel through the desktop's
// injector and stitches until the content stops moving, on a worker thread.
// Rows are spooled to a private file in the staging directory and written as
// one PNG at the end; only its descriptor crosses the control pipe.
//
// Commands: prepare {id, staging, rect?, maxHeight, maxPixels?, fromTop,
// autoStart, selection}, select {rect}, start, stop, cancel, overlay {visible}.
// Events: selected, progress, finished, cancelled, failed (ScrollBackend.h).
// Only wheel steps and pointer motion are injected, only while capturing, and
// nothing after stop, cancel or the end of the control pipe. Stop is the bar's
// button, the `stop` command and, on X11, Escape.
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
    bool fromTop_ = true, autoStart_ = true, barHidden_ = false, barOutside_ = false;
    bool stopRequested_ = false, cancelRequested_ = false;
    Reply selectReply_, stopReply_, cancelReply_;
    QJsonObject finished_;
    SelectionOverlay* selection_ = nullptr;
    LayerShellRegionOverlay* layerSelection_ = nullptr;
    BoundsOverlay bounds_;
    // KWin: damage on the region's output every heartbeatMs while frames are read (BoundsOverlay::showHeartbeat).
    QTimer heartbeat_;
    ScrollBar* bar_ = nullptr;
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
