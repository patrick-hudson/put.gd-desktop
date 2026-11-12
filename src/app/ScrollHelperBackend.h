#pragma once
#include "ScrollBackend.h"
#include "ipc/UiControl.h"
#include "core/Log.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <memory>
#include <optional>

namespace putgd {
// The production scroll-capture backend (#1376): one transient `putgd-record`
// process per capture, started as a scroll session, over private UiControl
// pipes. It is RecordingHelperBackend's supervision for the helper's second
// session kind: build-identity handshake, request/result commands, unsolicited
// events, bounded deadlines and a required normal exit. What the desktop offers
// comes from a separate `putgd-record --scroll-capabilities` run, cached until
// the resident exits (a failed probe for a minute). Pixels never cross these
// pipes: the helper writes one PNG into the staging directory and reports its
// path, size and hash.
class ScrollHelperBackend final : public QObject, public ScrollBackend {
    Q_OBJECT
public:
    explicit ScrollHelperBackend(QString helperPath = {}, QObject* parent = nullptr);
    ~ScrollHelperBackend() override;
    // Tests shorten these; production keeps the defaults (scroll::helper*Ms).
    void setTimeouts(int startupMs, int commandMs);
    // {available, reason?, message?, injector, frames, selection, maxHeight}.
    // Besides the four reasons of ScrollBackend.h, before the first probe has
    // answered: scroll_not_probed, scroll_probe_pending; and scroll_probe_failed.
    QJsonObject capabilities() const override;
    void refreshCapabilities(std::function<void()> done) override;
    void setEventSink(Event sink) override { sink_ = std::move(sink); }
    void prepare(const QString& id, const QJsonObject& params, const QString& stagingDirectory, Reply) override;
    void control(const QString& id, const QString& command, const QJsonObject& params, Reply) override;
    void discard(const QString& id) override;
    qint64 processId(const QString& id) const override;
    QJsonObject diagnostics() const;
    // The resolved interface scheme, "dark" or "light": the helper's bar uses its tokens.
    void setAppearance(const QString& scheme);
private:
    struct Child {
        QProcess* process = nullptr;
        UiControl* control = nullptr;
        QString id, nonce;
        quint64 generation = 0;
        bool ready = false, shuttingDown = false, exited = false;
        QHash<QString, Reply> outstanding;
        qint64 stderrBytes = 0;
    };
    void send(const QString& command, const QJsonObject& params, int timeoutMs, Reply);
    void receive(quint64 generation, const QString& type, const QJsonObject& payload);
    void failChild(quint64 generation, const Error&);
    void exited(quint64 generation, int code, QProcess::ExitStatus);
    void finishProbe();
    void launchQueued();
    QJsonObject envelope() const;
    QString helperPath_, scheme_ = QStringLiteral("dark");
    Event sink_;
    std::unique_ptr<Child> child_;
    // A prepare that arrived while the previous capture's helper was still
    // shutting down: it starts as soon as that helper exits.
    struct Queued { QString id; QJsonObject params; QString staging; Reply reply; };
    std::optional<Queued> queued_;
    quint64 generation_ = 0;
    QTimer startup_, stopDeadline_;
    int startupMs_ = scroll::helperStartupMs, commandMs_ = scroll::helperCommandMs;
    QJsonObject cached_, lastExit_;
    qint64 cachedAt_ = 0;
    QProcess* probe_ = nullptr;
    QTimer probeDeadline_;
    QList<std::function<void()>> probeWaiters_;
    HelperOutput output_{"putgd-record (scroll)", logRecording()}, probeOutput_{"putgd-record --scroll-capabilities", logRecording(), true};
};
}
