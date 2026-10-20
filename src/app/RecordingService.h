#pragma once
#include "RecordingBackend.h"
#include "core/Operation.h"
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
        std::function<void(const Operation&, std::function<void(Error)>)> persist;
        std::function<void(const Operation&)> observe;
        // Hands the validated export file to the immutable artifact pipeline.
        // `released` runs once the pipeline no longer needs the staged file.
        std::function<void(Operation, ClipboardPayload, std::function<void()> released)> admit;
        // Terminal cancel/failure without any upload.
        std::function<void(Operation)> finish;
    };
    RecordingService(QString stagingRoot, Host, QObject* parent = nullptr);
    ~RecordingService() override;
    void setBackend(std::unique_ptr<RecordingBackend>);
    // The desktop shell that selects regions and frames the recording where the
    // helper cannot (GNOME); not owned, and it must outlive the service.
    void setShell(RecordingShell* shell) { shell_ = shell; }
    QJsonObject capabilities() const;
    bool active() const { return session_.has_value(); }
    QString activeId() const { return session_ ? session_->op.id : QString{}; }
    // capabilities | prepare | start | pause | resume | stop | cancel | inspect |
    // select | raise | overlay | edit | preview | saveCopy | upload
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
private:
    struct Session {
        Operation op;
        QString state = "preparing", pending;
        RecordingTarget target;
        RecordingConfig config;
        QSize canvas;
        qint64 duration = 0;
        RecordingEdit edit;
        QJsonObject progress;
        // The latest saveCopy/upload export: {command, status, revision, path?, bytes?, durationUs?, error?}.
        QJsonObject lastExport;
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
        int exports = 0;
        quint64 generation = 0;
    };
    QJsonObject publicJson(const Session&) const;
    // The phase defaults to the state; Upload's export reports as `encoding`.
    void transition(Session&, const QString& state, const QString& phase = {});
    QJsonObject limitsJson(const Session&) const;
    void fail(Error);
    void cancelActive(JsonReply);
    void handleEvent(const QJsonObject&);
    void prepareSession(const QJsonObject& params, JsonReply);
    // After the durable intent: a shell selection when needed, then the backend.
    void beginPrepare(quint64 generation);
    void prepareBackend(quint64 generation);
    void prepared(Error, const QJsonObject& result);
    QJsonObject shellFrame(const Session&, const QString& state) const;
    void closeShell(Session&);
    void commandSession(const QString& command, const QJsonObject& params, JsonReply);
    // `reply` answers once the export starts (or is refused); `done`, when
    // given, runs with the export's outcome.
    void exportTo(const QString& command, const QJsonObject& params, JsonReply reply, JsonReply done = {});
    RecordingBackend::Reply marshal(quint64 generation, std::function<void(Error, QJsonObject)>);
    QString stagingRoot_;
    Host host_;
    std::unique_ptr<RecordingBackend> backend_;
    RecordingShell* shell_ = nullptr;
    std::optional<Session> session_;
    quint64 generation_ = 0;
    bool stopping_ = false;
};
}
