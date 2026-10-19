#pragma once
#include "RecordingBackend.h"
#include "UiControl.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <memory>

namespace putgd {
// Supervises one transient `putgd-record` process per recording session over
// private UiControl pipes: build-identity handshake, request/result commands,
// unsolicited events, bounded deadlines and a required normal exit. The
// capability inventory comes from a separate `putgd-record --capabilities`
// run, cached until the resident exits. Media never crosses these pipes.
class RecordingHelperBackend final : public QObject, public RecordingBackend {
    Q_OBJECT
public:
    explicit RecordingHelperBackend(QString helperPath = {}, QObject* parent = nullptr);
    ~RecordingHelperBackend() override;
    // Tests shorten these; production keeps the defaults.
    void setTimeouts(int startupMs, int commandMs, int prepareMs, int exportMs);
    QJsonObject capabilities() const override;
    void refreshCapabilities(std::function<void()> done) override;
    void setEventSink(Event sink) override { sink_ = std::move(sink); }
    void prepare(const QString& id, const RecordingTarget&, const RecordingConfig&, const QString& stagingDirectory, Reply) override;
    void control(const QString& id, const QString& command, const QJsonObject& params, Reply) override;
    void preview(const QString& id, const RecordingEdit&, const RecordingConfig&, qint64 timeUs, int maxEdge, Reply) override;
    void exportClip(const QString& id, const RecordingEdit&, const RecordingConfig&, const QString& outputPath, Reply) override;
    void setEdit(const QString& id, const RecordingEdit&, const RecordingConfig&, const QJsonObject& limits) override;
    void discard(const QString& id) override;
    qint64 processId(const QString& id) const override;
    QJsonObject diagnostics() const;
private:
    struct Outstanding { Reply reply; };
    struct Child {
        QProcess* process = nullptr;
        UiControl* control = nullptr;
        QString id, nonce;
        quint64 generation = 0;
        bool ready = false, shuttingDown = false, exited = false;
        QHash<QString, Outstanding> outstanding;
        qint64 stderrBytes = 0;
    };
    void send(const QString& command, const QJsonObject& params, int timeoutMs, Reply);
    void receive(quint64 generation, const QString& type, const QJsonObject& payload);
    void failChild(quint64 generation, const Error&);
    void exited(quint64 generation, int code, QProcess::ExitStatus);
    void finishProbe();
    QJsonObject envelope() const;
    QString helperPath_;
    Event sink_;
    std::unique_ptr<Child> child_;
    quint64 generation_ = 0;
    QTimer startup_, stopDeadline_;
    // prepare covers a hands-on drag selection and OS consent; the overlay stays visible and Escape cancels.
    int startupMs_ = 10000, commandMs_ = 15000, prepareMs_ = 600000, exportMs_ = 600000;
    QJsonObject cached_, lastExit_;
    qint64 cachedAt_ = 0;
    QProcess* probe_ = nullptr;
    QTimer probeDeadline_;
    QList<std::function<void()>> probeWaiters_;
};
}
