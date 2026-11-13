#pragma once
#include "core/Error.h"
#include "core/Log.h"
#include "ipc/UiControl.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace putgd {
// What went wrong with a helper, before its policy names it for the caller.
enum class HelperFault { Closed, Protocol, Build, StartFailed, StartTimeout, CommandTimeout, Exited, Crashed };
// The capability probe: a separate `putgd-record <argument>` run, cached until the
// resident exits, a failed probe for a minute.
struct HelperProbePolicy {
    QString argument;
    qsizetype limit = 0;
    // Logged with the reason when the probe says the feature is unavailable.
    QString unavailableLog;
    // The cache for an answer that is missing or not usable: {available: false, reason, ...}.
    std::function<QJsonObject(const QString& reason)> unavailable;
    // The cache for a probe answer, or an empty object when the answer breaks the contract.
    std::function<QJsonObject(const QJsonObject& answer)> accept;
    // The reason for a clean exit whose answer breaks the contract; empty treats it as a failed probe.
    QString invalidReason;
    // A cached answer that is asked again whatever its age.
    std::function<bool(const QJsonObject& cached)> stale;
    // String members of the previous cache that a failed probe keeps.
    QStringList kept;
};
// What tells the helper's two session kinds apart.
struct HelperPolicy {
    // In messages: "The <noun> is not running", "No <noun> owns this session".
    QString noun;
    // The supervisor's own kill and probe reasons: <prefix>_stop_timeout,
    // <prefix>_probe_timeout, <prefix>_probe_failed, <prefix>_probe_pending, <prefix>_not_probed.
    QString prefix;
    // The process name in the resident log.
    QString process;
    // The bootstrap frame's session kind; empty for a recording.
    QString session;
    // An unexpected exit says "nothing was <lost>".
    QString lost;
    // The message to the requests a discard settles.
    QString discarded;
    // The answer to a prepare while another helper runs.
    Error active;
    // A recording keeps its helper when a command misses its deadline: the helper
    // may hold the only copy of the clip, and RecordingService ends the session
    // itself where a timeout means that. The late answer to that request is then
    // dropped. A scroll capture ends its helper: one that stopped answering may
    // still be turning the wheel, and the capture has nothing to lose.
    bool timeoutEndsHelper = false;
    // The code and details the caller sees; the message already names the helper.
    std::function<Error(HelperFault, const QString& message, const QJsonObject& details)> error;
    HelperProbePolicy probe;
};
// Supervises one transient `putgd-record` process at a time over private
// UiControl pipes: build-identity handshake, request/result commands, unsolicited
// events, bounded deadlines and a required normal exit. RecordingHelperBackend
// and ScrollHelperBackend each own one, with their own policy.
class HelperSupervisor final : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    using Event = std::function<void(QJsonObject)>;
    HelperSupervisor(HelperPolicy policy, QString helperPath);
    ~HelperSupervisor() override;
    const QString& helperPath() const { return helperPath_; }
    bool executable() const;
    // The helper's start and the probe's deadline.
    void setStartupMs(int ms) { startupMs_ = ms; }
    int startupMs() const { return startupMs_; }
    void setEventSink(Event sink) { sink_ = std::move(sink); }
    QJsonObject capabilities() const;
    void refreshCapabilities(std::function<void()> done);
    // Starts the helper for id and, once it is ready, sends it `prepare` with
    // params; the result answers reply. While the previous helper is still
    // leaving, the prepare waits for its exit.
    void prepare(const QString& id, const QJsonObject& params, int prepareMs, Reply reply);
    // A command to the helper that owns id.
    void command(const QString& id, const QString& command, const QJsonObject& params, int timeoutMs, Reply reply);
    void discard(const QString& id);
    qint64 processId(const QString& id) const;
    QJsonObject diagnostics() const;
    // "dark" or "light": goes in the bootstrap frame, and a running helper gets an `appearance` frame.
    void setAppearance(const QString& scheme);
private:
    struct Child {
        QProcess* process = nullptr;
        UiControl* control = nullptr;
        QString id, nonce;
        quint64 generation = 0;
        bool ready = false, shuttingDown = false, exited = false;
        QHash<QString, Reply> outstanding;
        // Requests answered with a timeout while the helper was kept, by id, with their command.
        QHash<QString, QString> timedOut;
        qint64 stderrBytes = 0;
    };
    Error fault(HelperFault, const QString& message, const QJsonObject& details = {}) const;
    QString the() const { return QStringLiteral("The ") + policy_.noun; }
    void send(const QString& command, const QJsonObject& params, int timeoutMs, Reply);
    void receive(quint64 generation, const QString& type, const QJsonObject& payload);
    void failChild(quint64 generation, const Error&);
    void exited(quint64 generation, int code, QProcess::ExitStatus);
    void finishProbe();
    void launchQueued();
    QJsonObject envelope() const;
    HelperPolicy policy_;
    QString helperPath_, scheme_ = QStringLiteral("dark");
    Event sink_;
    std::unique_ptr<Child> child_;
    // A prepare that arrived while the previous session's helper was still
    // shutting down (#1296): the resident had already ended that session, so
    // it starts as soon as the old helper exits, which stopDeadline_ bounds.
    struct Queued { QString id; QJsonObject params; int prepareMs = 0; Reply reply; };
    std::optional<Queued> queued_;
    quint64 generation_ = 0;
    QTimer startup_, stopDeadline_;
    int startupMs_ = 10000;
    QJsonObject cached_, lastExit_;
    qint64 cachedAt_ = 0;
    QProcess* probe_ = nullptr;
    QTimer probeDeadline_;
    QList<std::function<void()>> probeWaiters_;
    // Helper stderr and exits go to the resident log, bounded per generation.
    HelperOutput output_, probeOutput_;
};
}
