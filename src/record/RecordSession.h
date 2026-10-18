#pragma once
#include "ClipExporter.h"
#include "Overlays.h"
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
    void finishPrepare(Reply);
    void start(Reply);
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
    bool prepared_ = false, recorded_ = false, overlayExcluded_ = true;
    RecordingPipeline pipeline_;
    QTimer progress_, barTimer_;
    SelectionOverlay* selection_ = nullptr;
    BoundsOverlay bounds_;
    ControlBar* bar_ = nullptr;
    ReviewWindow* review_ = nullptr;
    QThread* exportThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> exportCancel_;
};
}
