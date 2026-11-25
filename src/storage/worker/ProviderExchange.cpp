#include "ProviderExchange.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <cerrno>
#include <cstring>
#include <future>
namespace putgd::worker {
StartFailure classifyStartFailure(const QString& program, const QString& reason) {
    // A directory or a missing file is not a helper: only a regular executable file is.
    const QFileInfo file(program);
    if (!file.isFile() || !file.isExecutable()) return StartFailure::Installation;
    // Shortages that pass: descriptors (EMFILE, ENFILE), memory (ENOMEM), processes or
    // threads (EAGAIN), and a program file that is being written just then (ETXTBSY).
    for (const int code : {EMFILE, ENFILE, ENOMEM, EAGAIN, ETXTBSY})
        if (reason.contains(QString::fromLocal8Bit(std::strerror(code)), Qt::CaseInsensitive)) return StartFailure::Temporary;
    return StartFailure::Unclear;
}
QString startFailureMessage(StartFailure failure, const QString& installationMessage, const QString& subject) {
    // Qt's own text stays in the error details: for an existing helper it can name the
    // wrong cause ("No such file or directory" when Qt ran out of descriptors).
    if (failure == StartFailure::Installation) return installationMessage;
    if (failure == StartFailure::Temporary) return subject + " right now. Try again.";
    return subject + ". Try again, or reinstall put.gd if it keeps failing.";
}
QString unconfirmedEnding(const QString& method, bool mutation, const QString& place, const QString& upload) {
    if (method == "upload") return upload;
    if (method == "remove") return ", so the file may already be deleted. Check " + place + " before you retry the deletion.";
    if (mutation) return ", so the request may already have taken effect. Check " + place + " before trying again.";
    return QStringLiteral(". Try again.");
}
QJsonObject exchange(const QString& program, const QString& method, QJsonObject params, qint64 perRequest,
                     const ExchangePolicy& policy, std::atomic_bool& stopping_, Error& error, bool* uncertain,
                     std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress, DurableCheckpoint checkpoint, std::shared_ptr<std::atomic_bool> kill,
                     std::function<int()> lanes, std::function<void(const QJsonObject&)> notice) {
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, {"--stdio"});
    if (!process.waitForStarted(10000)) {
        // A helper that is installed but cannot start right now is not a broken installation: say whether trying again helps, with Qt's reason in the details.
        const auto reason = process.errorString();
        error = {policy.prefix + "_worker_start", startFailureMessage(classifyStartFailure(program, reason), policy.startMessage), {{"reason", reason}}};
        return {};
    }
    auto bytes = QJsonDocument(params).toJson(QJsonDocument::Compact);
    params = {};
    bytes.append('\n');
    // Keys travel only on this pipe. Once request bytes are delivered, a lost or
    // malformed completion cannot establish whether a change reached the server.
    const auto delivered = process.write(bytes);
    bytes.fill('\0');
    bytes.clear();
    if (uncertain) *uncertain = delivered > 0 && policy.mutation;
    if (!process.waitForBytesWritten(10000)) { process.kill(); process.waitForFinished(5000); error = {policy.unknownCode, "The " + policy.name + " helper didn't accept the request. Try again.", {}}; return {}; }
    // A bound on the whole call: every HTTP request has its own deadline in the worker.
    QElapsedTimer silence;
    silence.start();
    QByteArray pending;
    QJsonObject final;
    bool got = false, cancelSent = false;
    QElapsedTimer cancelClock;
    QElapsedTimer lanesPoll;
    lanesPoll.start();
    int lentLanes = 0;
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
        if (value["event"] == "signIn" && notice) { notice(value); return true; }
        if (got || !value["ok"].isBool()) { error = {policy.prefix + "_protocol", "The " + policy.name + " helper returned an invalid final reply.", {}}; return false; }
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
        if (pending.size() > 8 * 1024 * 1024) { error = {policy.prefix + "_protocol", policy.outputName + " helper sent too much output, so put.gd stopped it.", {}}; break; }
        bool failed = false;
        while (pending.contains('\n')) {
            const auto line = pending.left(pending.indexOf('\n'));
            pending.remove(0, line.size() + 1);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(line, &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject() || !handle(document.object())) {
                if (!error) error = {policy.prefix + "_protocol", "The " + policy.name + " helper returned an unreadable reply.", {}};
                failed = true;
                break;
            }
        }
        if (failed || got) break;
        // Quit: the helper is killed, not asked to stop, so it sends no further request.
        // What it had sent may have reached the server: a mutation's outcome is unknown.
        if (kill && kill->load()) {
            process.kill();
            process.waitForFinished(5000);
            error = {policy.unknownCode, "put.gd quit while the " + policy.name + " helper was running" +
                     unconfirmedEnding(method, policy.mutation, "the server", ", so the file may already be on the server. Check the server or select Retry upload."), {{"reason", "quit"}}};
            return {};
        }
        if (lanes && !cancelSent && lanesPoll.elapsed() >= 100) {
            lanesPoll.restart();
            int lent = 0;
            try { lent = qMax(0, lanes()); } catch (...) {}
            if (lent != lentLanes) {
                process.write("{\"lanes\":" + QByteArray::number(lent) + "}\n");
                lentLanes = lent;
            }
        }
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
