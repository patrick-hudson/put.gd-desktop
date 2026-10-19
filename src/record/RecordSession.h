#pragma once
#include "ClipExporter.h"
#include "Overlays.h"
#include "PortalScreenCast.h"
#include "MutterScreenCast.h"
#ifdef PUTGD_RECORD_KDE_SCREENCAST
#include "KdeScreencast.h"
#endif
#include "RecordingPipeline.h"
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

namespace putgd { class KdeRegionOverlay; }
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
    void finishWayland(quint32 node, int fd, const QRect& shared, const QRect& streamRect, const QString& source, Reply);
    QString portalTokenKey() const;
    QString loadPortalToken() const;
    void storePortalToken(const QString&) const;
    void start(Reply);
    // Starts the pipeline; start() calls it once the bar is out of the way.
    void begin(Reply);
    void stop(Reply);
    void cancelLocal();
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
    QJsonObject capabilities_;
    QString staging_, workingClip_;
    qint64 durationUs_ = 0;
    bool prepared_ = false, recorded_ = false, overlayExcluded_ = true, cancelled_ = false, starting_ = false;
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
    QTimer progress_, barTimer_;
    SelectionOverlay* selection_ = nullptr;
    KdeRegionOverlay* kdeSelection_ = nullptr;
    BoundsOverlay bounds_;
    ControlBar* bar_ = nullptr;
    ReviewWindow* review_ = nullptr;
    QThread* exportThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> exportCancel_;
};
}
