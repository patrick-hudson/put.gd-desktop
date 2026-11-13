#pragma once
#include "RecordingBackend.h"
#include "HelperSupervisor.h"
#include <QObject>

namespace putgd {
// Supervises one transient `putgd-record` process per recording session
// (HelperSupervisor): build-identity handshake, request/result commands,
// unsolicited events, bounded deadlines and a required normal exit. The
// capability inventory comes from a separate `putgd-record --capabilities`
// run, cached until the resident exits. Media never crosses these pipes.
// A command that misses its deadline is answered recording_helper_timeout and
// the helper is kept: it may hold the only copy of the clip (#1443).
class RecordingHelperBackend final : public QObject, public RecordingBackend {
    Q_OBJECT
public:
    explicit RecordingHelperBackend(QString helperPath = {}, QObject* parent = nullptr);
    // Tests shorten these; production keeps the defaults.
    void setTimeouts(int startupMs, int commandMs, int prepareMs, int exportMs);
    QJsonObject capabilities() const override { return helper_.capabilities(); }
    void refreshCapabilities(std::function<void()> done) override { helper_.refreshCapabilities(std::move(done)); }
    // A transient `putgd-record --microphones [device]` run, never cached: inputs come and go.
    void microphones(const QString& device, Reply reply) override;
    void setEventSink(Event sink) override { helper_.setEventSink(std::move(sink)); }
    void prepare(const QString& id, const RecordingTarget&, const RecordingConfig&, const QString& stagingDirectory, Reply, const QJsonObject& reviewOptions = {}) override;
    void control(const QString& id, const QString& command, const QJsonObject& params, Reply) override;
    void preview(const QString& id, const RecordingEdit&, const RecordingConfig&, qint64 timeUs, int maxEdge, Reply) override;
    void exportClip(const QString& id, const RecordingEdit&, const RecordingConfig&, const QString& outputPath, Reply) override;
    void setEdit(const QString& id, const RecordingEdit&, const RecordingConfig&, const QJsonObject& limits) override;
    void discard(const QString& id) override { helper_.discard(id); }
    qint64 processId(const QString& id) const override { return helper_.processId(id); }
    QJsonObject diagnostics() const { return helper_.diagnostics(); }
    // The resolved interface scheme, "dark" or "light" (#198): the helper's
    // overlays, control bar and review window use its tokens. It goes in the
    // bootstrap frame, and a running helper gets an `appearance` frame.
    void setAppearance(const QString& scheme) { helper_.setAppearance(scheme); }
private:
    HelperSupervisor helper_;
    // prepare covers a hands-on drag selection and OS consent; the overlay stays visible and Escape cancels.
    int commandMs_ = recording::helperCommandMs, prepareMs_ = recording::helperPrepareMs, exportMs_ = recording::helperExportMs;
};
}
