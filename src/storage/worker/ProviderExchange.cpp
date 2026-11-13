#include "ProviderExchange.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <future>
namespace putgd::worker {
QJsonObject exchange(const QString& program, const QString& method, QJsonObject params, qint64 perRequest,
                     const ExchangePolicy& policy, std::atomic_bool& stopping_, Error& error, bool* uncertain,
                     std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress, DurableCheckpoint checkpoint) {
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, {"--stdio"});
    if (!process.waitForStarted(10000)) { error = {policy.prefix + "_worker_start", policy.startMessage, {}}; return {}; }
    auto bytes = QJsonDocument(params).toJson(QJsonDocument::Compact);
    params = {};
    bytes.append('\n');
    // Keys travel only on this pipe. Once request bytes are delivered, a lost or
    // malformed completion cannot establish whether a change reached the server.
    const auto delivered = process.write(bytes);
    bytes.fill('\0');
    bytes.clear();
    if (uncertain) *uncertain = delivered > 0 && policy.mutation;
    if (!process.waitForBytesWritten(10000)) { process.kill(); process.waitForFinished(5000); error = {policy.unknownCode, "The " + policy.name + " helper did not accept its request", {}}; return {}; }
    // A bound on the whole call: every HTTP request has its own deadline in the worker.
    QElapsedTimer silence;
    silence.start();
    QByteArray pending;
    QJsonObject final;
    bool got = false, cancelSent = false;
    QElapsedTimer cancelClock;
    const auto handle = [&](const QJsonObject& value) -> bool {
        if (value["event"] == "progress") {
            auto fields = value;
            fields.remove("event");
            const auto report = UploadProgress::fromJson(fields);
            if (!report) { error = {policy.prefix + "_protocol", "Invalid upload progress from the " + policy.name + " helper", {}}; return false; }
            if (progress && method == "upload") progress(*report);
            return true;
        }
        if (value["event"] == "checkpoint") {
            if (!value["journal"].isObject() || !policy.checkpointMethods.contains(method)) { error = {policy.prefix + "_protocol", "Unexpected checkpoint from the " + policy.name + " helper", {}}; return false; }
            Error saved;
            if (checkpoint) {
                auto promise = std::make_shared<std::promise<Error>>();
                auto future = promise->get_future();
                checkpoint(value["journal"].toObject(), [promise](Error e) { promise->set_value(e); });
                saved = future.get();
            }
            QJsonObject ack{{"ack", !saved}};
            if (saved) ack["error"] = QJsonObject{{"code", saved.code}, {"message", saved.message}};
            process.write(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');
            return true;
        }
        if (got || !value["ok"].isBool()) { error = {policy.prefix + "_protocol", "Invalid completion from the " + policy.name + " helper", {}}; return false; }
        final = value;
        got = true;
        return true;
    };
    while (process.state() != QProcess::NotRunning || process.bytesAvailable()) {
        process.waitForReadyRead(100);
        process.readAllStandardError();
        process.setReadChannel(QProcess::StandardOutput);
        const auto chunk = process.readAll();
        if (!chunk.isEmpty()) silence.restart();
        pending += chunk;
        if (pending.size() > 8 * 1024 * 1024) { error = {policy.prefix + "_protocol", policy.outputName + " helper output exceeded its bound", {}}; break; }
        bool failed = false;
        while (pending.contains('\n')) {
            const auto line = pending.left(pending.indexOf('\n'));
            pending.remove(0, line.size() + 1);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(line, &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject() || !handle(document.object())) {
                if (!error) error = {policy.prefix + "_protocol", "Malformed " + policy.name + " helper output", {}};
                failed = true;
                break;
            }
        }
        if (failed || got) break;
        if (!cancelSent && (stopping_.load() || (cancellation && cancellation->load()))) {
            process.write("{\"cancel\":true}\n");
            cancelSent = true;
            cancelClock.start();
        }
        // Stuck work: no output for several request deadlines, or a cancellation the
        // worker did not finish within a grace period.
        if ((cancelSent && cancelClock.elapsed() > 30000) || silence.elapsed() > 3 * perRequest + 60000) {
            error = {policy.unknownCode, policy.stalledMessage, {{"reason", cancelSent ? "cancelled" : "deadline"}}};
            break;
        }
        if (process.state() == QProcess::NotRunning && !process.bytesAvailable()) break;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(got ? 5000 : 100)) { process.kill(); process.waitForFinished(5000); }
    if (error) { if (policy.reclassifyOnError && uncertain && !got) *uncertain = policy.mutation; return {}; }
    if (!got) { error = {policy.unknownCode, policy.missingMessage, {}}; return {}; }
    return final;
}
}
