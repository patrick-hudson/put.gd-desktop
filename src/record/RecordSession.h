#pragma once
#include "ClipExporter.h"
#include "Overlays.h"
#include "PortalScreenCast.h"
#include "MutterScreenCast.h"
#include "Microphones.h"
#include "ClickOverlay.h"
#include "X11Clicks.h"
#ifdef PUTGD_RECORD_KDE_SCREENCAST
#include "KdeScreencast.h"
#endif
#include "RecordingPipeline.h"
#include "ImageCopySource.h"
#include "ReviewWindow.h"
#include "X11Targets.h"
#include "core/RecordingModel.h"
#include <QJsonObject>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

namespace putgd { class LayerShellRegionOverlay; }
namespace putgd::record {
// The helper's view of one recording session: resolves the target (after a
// drag selection when a region has no rectangle), shows the ready bounds,
// dimming and control bar, runs the live pipeline, keeps the private working
// clip, and serves preview/export requests against the authoritative edit the
// resident sends with each request. Replies carry the resident's request ids;
// user actions on the bar become unsolicited events the resident applies.
class RecordSession final : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    explicit RecordSession(QObject* parent = nullptr);
    ~RecordSession() override;
    void command(const QString& command, const QJsonObject& params, Reply);
    bool exporting() const { return exportThread_ != nullptr; }
signals:
    void event(const QJsonObject&);
private:
    friend class RecordingStorageContract;
    void prepare(const QJsonObject& params, Reply);
    // A region without a rectangle: the X11 drag surface or, on KDE Wayland, the
    // layer-shell screenshot selector; the stream starts only after the choice.
    void select(Reply);
    void completeSelection(const QJsonObject& params, Reply);
    // Opens the source for the resolved target (X11 directly, or a Wayland stream).
    void acquire(Reply);
    void finishPrepare(Reply);
    // Wayland: share through the portal picker, then continue like X11 with the stream as the target.
    void preparePortal(Reply);
    void prepareNative(Reply);
    // wlr-capture: ext-image-copy-capture frames into appsrc, no portal and no chooser.
    void prepareImageCopy(Reply);
    void finishWayland(quint32 node, int fd, const QRect& shared, const QRect& streamRect, const QString& source, Reply);
    // A window target: the compositor ended the stream (the window closed).
    void targetLost();
    // A window stream's source failed: reopen it unless the window is gone.
    void sourceLost(const Error&);
    // Finishes a running capture on its own and opens review, like a budget stop.
    void endCapture(const QString& reason);
    QString portalTokenKey() const;
    QString loadPortalToken() const;
    void storePortalToken(const QString&) const;
    void start(Reply);
    // Starts the pipeline; start() calls it once the bar is out of the way.
    void begin(Reply);
    void stop(Reply);
    void cancelLocal();
    // Ends the compositor's share once capture is over, so review stays private.
    void releaseAcquisition();
    void preview(const QJsonObject& params, Reply);
    void exportClip(const QJsonObject& params, Reply);
    bool parseEdit(const QJsonObject&, RecordingEdit&) const;
    void emitProgress();
    void showBar(const QString& state);
    void openReview();
    QString targetLabel() const;
    QString id_;
    RecordingTarget target_;
    RecordingConfig config_;
    ResolvedTarget resolved_;
    QJsonObject capabilities_, reviewOptions_;
    QString staging_, workingClip_;
    qint64 durationUs_ = 0;
    bool audioCaptured_ = false;
    // #1364: what the clip's audio track holds, the input the session resolved
    // at prepare, and the meter's open microphone before Record. The device is
    // open only while the bar's toggle is on and closed when the capture ends.
    bool capturedSystem_ = false, capturedMicrophone_ = false;
    MicrophoneChoice microphone_;
    // Click highlights (#1364): on X11 the helper reads presses from the X
    // server; elsewhere the resident sends what the desktop reported (`click`).
    X11ClickSource x11Clicks_;
    void startClicks();
    // A press in desktop coordinates (and, for a window recording, the window's
    // rectangle at that moment): drawn where it falls in the picture.
    void click(QPointF desktop, int button, const QRectF& window, qint64 ageNs = 0);
    static qint64 clickAge(const QJsonObject& params);
    MicrophoneMonitor monitor_;
    QTimer levelTimer_;
    void updateMicrophoneMonitor();
    void closeMicrophone();
    // While the bar is at Ready the session follows the sound server's inputs (#1364): a microphone
    // plugged in after `prepare` can be used without starting over, and one that is pulled is no longer
    // offered. Asked once a second; listing opens no device.
    void followMicrophones();
    std::function<MicrophoneList()> listMicrophones_ = &listMicrophones;
    QTimer microphoneWatch_;
    // An audio command (recording.audio) changes the toggles for this session only: it is not the
    // user's choice to remember.
    bool commandedAudio_ = false;
    // Reads what the finished capture recorded.
    void noteCaptured();
    QJsonObject audioSources() const { return {{"system", capturedSystem_}, {"microphone", capturedMicrophone_}}; }
    // Where the session is (#1434): the one lifecycle state the command table
    // (recording::checkHelperCommand) and every guard here read.
    HelperPhase phase_ = HelperPhase::Idle;
    // Cancelled is final, and Failed gives way only to Cancelled: a late callback
    // never brings a session back.
    void enterPhase(HelperPhase);
    // The clip is in review, with or without an export running.
    bool reviewing() const { return phase_ == HelperPhase::Recorded || phase_ == HelperPhase::Exporting; }
    bool overlayExcluded_ = true;
    // recording.overlay {visible: false} keeps the bar hidden across state changes until raise.
    bool barHidden_ = false;
    QString overlayPlacement_;
    RecordingPipeline pipeline_;
    PortalScreenCast portal_;
    MutterScreenCast mutter_;
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    KdeScreencast kde_;
#endif
    int portalFd_ = -1;
    quint32 portalNode_ = 0;
    QRect portalCrop_;
    bool portalTarget_ = false;
    std::unique_ptr<ImageCopySource> imageCopy_;
    QSize imageCopySize_;
    bool imageCopyPointer_ = false;  // the source draws the cursor and knows where it is (#1364)
    // A window target resolved by the resident: {rect: logical frame [x,y,w,h], title}.
    QJsonObject window_;
    bool targetClosed_ = false;
    int sourceReopens_ = 0;
    QTimer progress_, barTimer_;
    SelectionOverlay* selection_ = nullptr;
    LayerShellRegionOverlay* kdeSelection_ = nullptr;
    BoundsOverlay bounds_;
    ControlBar* bar_ = nullptr;
    ReviewWindow* review_ = nullptr;
    QThread* exportThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> exportCancel_;
};
}
