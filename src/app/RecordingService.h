#pragma once
#include "RecordingBackend.h"
#include "core/RecordingClicks.h"
#include "core/Operation.h"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include <optional>

namespace putgd {
using JsonReply = std::function<void(Error, QJsonObject)>;
// Owns the one active recording session: its durable intent (an operation in
// state `recording`), the authoritative state machine, the edit model and the
// private export staging. Media work lives behind RecordingBackend. Only the
// explicit `upload` command admits bytes to the shared artifact pipeline;
// preview, edits, local copies, cancel and restart never do.
class RecordingService final : public QObject {
    Q_OBJECT
public:
    struct Host {
        std::function<Settings()> settings;
        // Fills identity, settings snapshot, destination and ownership; rejects when busy.
        std::function<Error(Operation&)> accept;
        std::function<QJsonObject(const Operation&)> reviewOptions;
        std::function<void(const Operation&, std::function<void(Error)>)> persist;
        std::function<void(const Operation&)> observe;
        // Hands the validated export file to the immutable artifact pipeline.
        // `released` runs once the pipeline no longer needs the staged file.
        std::function<void(Operation, ClipboardPayload, std::function<void()> released)> admit;
        // Terminal cancel/failure without any upload.
        std::function<void(Operation)> finish;
        // Saves a change the recording bar made to a remembered preference
        // (#1364: the microphone toggle); optional.
        std::function<void(const QJsonObject& preferences)> remember;
    };
    RecordingService(QString stagingRoot, Host, QObject* parent = nullptr);
    ~RecordingService() override;
    void setBackend(std::unique_ptr<RecordingBackend>);
    // The desktop shell that selects regions and frames the recording where the
    // helper cannot (GNOME); not owned, and it must outlive the service.
    void setShell(RecordingShell* shell) { shell_ = shell; }
    // The desktop's window list for window recordings (KDE, GNOME Wayland); not owned.
    void setWindows(RecordingWindows* windows) { windows_ = windows; }
    // Where the desktop reports pointer presses for click highlights (#1364);
    // not owned. Unset on X11, where the helper reads them from the X server.
    void setClicks(RecordingClicks* clicks) { clicks_ = clicks; }
    QJsonObject capabilities() const;
    bool active() const { return session_.has_value(); }
    QString activeId() const { return session_ ? session_->op.id : QString{}; }
    // capabilities | windows | prepare | start | pause | resume | stop | cancel |
    // inspect | select | raise | overlay | edit | preview | saveCopy | upload
    void command(const QString& command, const QJsonObject& params, JsonReply);
    // The tray and shortcut entry: with no session, prepare a dragged region with
    // the default configuration; otherwise "open" raises the session's window and
    // "toggle" starts a ready session or stops a recording (paused) one.
    void invoke(const QString& mode, JsonReply);
    void shutdown();
    // Abandoned exports never survive their resident.
    static void discardStaging(const QString& root);
    static RecordingLimits limits(const Settings&);
    // The smallest budget a recording starts with; prepare refuses below it (#213).
    static constexpr qint64 minimumBudgetBytes = 64LL * 1024 * 1024;
signals:
    void changed(const QString& operationId);
    // The capture ended itself on its byte budget (#225); `stop` is recording.inspect's stopDetails.
    void budgetStopped(const QString& operationId, const QJsonObject& stop);
private:
    struct Session {
        Operation op;
        RecordingState state = RecordingState::Preparing;
        QString pending;
        // A step outside recording::allowedStep was logged for this session (#1434).
        bool offTableStep = false;
        RecordingTarget target;
        RecordingConfig config;
        QSize canvas;
        qint64 duration = 0;
        RecordingEdit edit;
        QJsonObject progress;
        // The latest saveCopy/upload export: {command, status, revision, path?, bytes?, durationUs?, error?}.
        QJsonObject lastExport;
        QJsonObject lastExportRequest;
        std::optional<bool> overlayExcluded;
        // outside | inside | hidden | compositor: where the control bar ended up.
        QString overlayPlacement;
        // "helper" or "shell" while a region selection is open; `selection`
        // numbers the shell selector so a superseded reply is ignored.
        QString selector;
        quint64 selection = 0;
        // The logical rectangle the stream records, and whether the shell draws its frame.
        QRect recorded;
        bool shellFrame = false;
        QString staging;
        // Why the helper ended the capture: user, or budget when the disk/file cap ran out.
        QString stopReason;
        // Finalization survives later review/edit/export state transitions.
        bool captureStopped = false;
        // Capture capability survives silent export formats during review.
        bool capturedSystemAudio = false;
        // #1364: the track also (or only) holds the microphone.
        bool capturedMicrophone = false;
        bool clickWatchTried = false;  // the desktop was asked to report clicks for this recording, whatever it answered
        // Which bound set the budget at prepare (disk_space or max_file_bytes), and after
        // a budget stop {reason, message, hint, budgetBytes, maxFileBytes, durationUs, freeBytes?, ...}.
        QString budgetLimit;
        QJsonObject stopDetails;
        int exports = 0;
        quint64 generation = 0;
        // Since the session was accepted: the log's step timings (#250).
        QElapsedTimer clock;
    };
    QJsonObject publicJson(const Session&) const;
    // The phase defaults to the state; Upload's export reports as `encoding`.
    void transition(Session&, RecordingState, const QString& phase = {});
    // Logs, once per session, a step outside the from/to table; the step is taken all the same.
    void checkStep(Session&, RecordingState to);
    QJsonObject limitsJson(const Session&) const;
    void fail(Error);
    void cancelActive(JsonReply);
    void handleEvent(const QJsonObject&);
    void stoppedOnBudget(Session&, const QJsonObject& observed = {});
    // `remembered`: the tray, dashboard and shortcut entry, whose microphone
    // and click-highlight choices are preferences, not requests: where one is
    // unavailable the recording prepares without it instead of failing.
    void prepareSession(const QJsonObject& params, JsonReply, bool remembered = false);
    // {available, route, drawnBy, targets, reason?, message?} for this desktop.
    QJsonObject clickCapability() const;
    // Click highlights run from Record to Stop: the desktop's presses go to the
    // helper, which draws them (or the compositor's own effect is switched on).
    // then: runs once the desktop has answered (or after clickWatchWaitMs), whether or not it could watch.
    void watchClicks(Session&, std::function<void()> then = {});
    void startCapture(const QString& id, JsonReply);
    static constexpr int clickWatchWaitMs = 1500;
    void unwatchClicks();
    // What a capture reported it records: {audioSources: {system, microphone}}, or
    // only audioEncoder from a backend that predates the microphone.
    static void noteCaptured(Session&, const QJsonObject& result, bool required);
    // The input a prepare (or the capabilities) asks about: the request's own
    // device, else the one chosen in Settings, else the system default.
    QString microphoneDevice(const QJsonObject& params) const;
    // capabilities() with what the sound server offers right now.
    QJsonObject withMicrophone(QJsonObject capabilities, const QJsonObject& microphones) const;
    // After the durable intent: a shell selection when needed, then the backend.
    void beginPrepare(quint64 generation);
    // A window target: the active external window or the identity asked for,
    // from the desktop's window list, before the helper opens its stream.
    void resolveWindow(quint64 generation);
    void prepareBackend(quint64 generation);
    void prepared(Error, const QJsonObject& result);
    QJsonObject shellFrame(const Session&, RecordingState) const;
    void closeShell(Session&);
    void commandSession(const QString& command, const QJsonObject& params, JsonReply);
    // `reply` answers once the export starts (or is refused); `done`, when
    // given, runs with the export's outcome.
    void retryExport(JsonReply reply, JsonReply done = {});
    void exportTo(const QString& command, const QJsonObject& params, JsonReply reply, JsonReply done = {});
    RecordingBackend::Reply marshal(quint64 generation, std::function<void(Error, QJsonObject)>);
    QString stagingRoot_;
    Host host_;
    std::unique_ptr<RecordingBackend> backend_;
    RecordingShell* shell_ = nullptr;
    RecordingWindows* windows_ = nullptr;
    RecordingClicks* clicks_ = nullptr;
    bool clicksWatched_ = false;
    std::optional<Session> session_;
    quint64 generation_ = 0;
    bool stopping_ = false;
    // Set by invoke() for the prepare it issues: its choices are remembered preferences.
    bool rememberedPreferences_ = false;
};
}
