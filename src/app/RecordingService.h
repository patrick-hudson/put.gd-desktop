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
    QJsonObject capabilities() const;
    bool active() const { return session_.has_value(); }
    QString activeId() const { return session_ ? session_->op.id : QString{}; }
    // capabilities | prepare | start | pause | resume | stop | cancel | inspect |
    // raise | overlay | edit | preview | saveCopy | upload
    void command(const QString& command, const QJsonObject& params, JsonReply);
    void shutdown();
    // Abandoned exports never survive their resident.
    static void discardStaging(const QString& root);
    static RecordingLimits limits(const Settings&);
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
        QString staging;
        int exports = 0;
        quint64 generation = 0;
    };
    QJsonObject publicJson(const Session&) const;
    void transition(Session&, const QString& state);
    void fail(Error);
    void cancelActive(JsonReply);
    void handleEvent(const QJsonObject&);
    void prepareSession(const QJsonObject& params, JsonReply);
    void commandSession(const QString& command, const QJsonObject& params, JsonReply);
    void exportTo(const QString& command, const QJsonObject& params, JsonReply);
    RecordingBackend::Reply marshal(quint64 generation, std::function<void(Error, QJsonObject)>);
    QString stagingRoot_;
    Host host_;
    std::unique_ptr<RecordingBackend> backend_;
    std::optional<Session> session_;
    quint64 generation_ = 0;
    bool stopping_ = false;
};
}
