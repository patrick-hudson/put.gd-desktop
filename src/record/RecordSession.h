#pragma once
#include "ClipExporter.h"
#include "RecordingPipeline.h"
#include "X11Targets.h"
#include "core/RecordingModel.h"
#include <QJsonObject>
#include <QObject>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

namespace putgd::record {
// The helper's view of one recording session: resolves the target, runs the
// live pipeline, keeps the private working clip, and serves preview/export
// requests against the authoritative edit the resident sends with each
// request. Replies carry the resident's request ids; events are unsolicited.
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
    void start(Reply);
    void stop(Reply);
    void preview(const QJsonObject& params, Reply);
    void exportClip(const QJsonObject& params, Reply);
    bool parseEdit(const QJsonObject&, RecordingEdit&) const;
    void emitProgress();
    QString id_;
    RecordingTarget target_;
    RecordingConfig config_;
    ResolvedTarget resolved_;
    QString staging_, workingClip_;
    qint64 durationUs_ = 0;
    bool prepared_ = false, recorded_ = false;
    RecordingPipeline pipeline_;
    QTimer progress_;
    QThread* exportThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> exportCancel_;
};
}
