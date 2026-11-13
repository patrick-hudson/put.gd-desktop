#include "WorkerStdio.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
namespace putgd::worker {
// One bounded, newline-terminated line from stdin.
QByteArray Stdio::readLine(bool& eof) {
    QByteArray line;
    while (true) {
        const int c = std::fgetc(stdin);
        if (c == EOF) { eof = true; return line; }
        if (c == '\n') return line;
        if (line.size() >= maximumLine) { eof = true; return {}; }
        line.append(char(c));
    }
}
void Stdio::emitLine(const QJsonObject& value) {
    auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    std::lock_guard lock(outputMutex);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}
// Control lines after the request: acknowledgements and cancellation.
void Stdio::readControl() {
    while (true) {
        bool eof = false;
        const auto line = readLine(eof);
        if (!line.isEmpty()) {
            const auto value = QJsonDocument::fromJson(line).object();
            if (value["cancel"].toBool()) cancelled = true;
            else if (value.contains("ack")) {
                std::lock_guard lock(controlMutex);
                acks.push_back(value);
                controlChanged.notify_all();
            }
        }
        if (eof) {
            cancelled = true;
            std::lock_guard lock(controlMutex);
            parentGone = true;
            controlChanged.notify_all();
            return;
        }
    }
}
// Durable acknowledgement from the resident, bounded like the S3 worker's five minutes.
Error Stdio::checkpoint(const QJsonObject& journal) {
    emitLine({{"event", "checkpoint"}, {"journal", journal}});
    std::unique_lock lock(controlMutex);
    if (!controlChanged.wait_for(lock, std::chrono::minutes(5), [this] { return !acks.empty() || parentGone; }))
        return {prefix_ + "_checkpoint_timeout", "The resident did not save the " + journal_ + " journal in time", {}};
    if (acks.empty()) return {"cancelled", "The resident stopped this request", {}};
    const auto ack = acks.front();
    acks.pop_front();
    if (ack["ack"].toBool()) return {};
    const auto e = ack["error"].toObject();
    return {e["code"].toString("checkpoint_failed"), e["message"].toString("The " + journal_ + " journal could not be saved"), {}};
}

void Stdio::progress(const QString& phase, qint64 sent, qint64 total) {
    if (phase == "uploading" && last_.isValid() && last_.elapsed() < 250 && sent < total) return;
    last_.start();
    UploadProgress value;
    value.phase = phase;
    value.bytesTransferred = qBound<qint64>(0, sent, total);
    value.bytesTotal = total;
    auto line = value.toJson();
    line["event"] = "progress";
    emitLine(line);
}
int Stdio::run(int argc, char** argv, const char* name, const char* manifest, bool (*initialize)(), const Run& run) {
    if (argc == 2 && std::strcmp(argv[1], "--version-json") == 0) { std::printf("%s\n", manifest); return 0; }
    if (argc != 2 || std::strcmp(argv[1], "--stdio") != 0) return 2;
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(name);
    initialize();
    bool eof = false;
    auto line = readLine(eof);
    auto request = QJsonDocument::fromJson(line).object();
    line.fill('\0');
    if (request["schemaVersion"].toInt() != 1 || !request["method"].isString()) {
        emitLine({{"ok", false}, {"error", QJsonObject{{"code", prefix_ + "_protocol"}, {"message", "Invalid worker request"}}}, {"uncertain", false}});
        return 0;
    }
    std::thread control([this] { readControl(); });
    control.detach();
    putgd::Error error;
    bool uncertain = false;
    QJsonObject result;
    result = run(request, error, uncertain);
    if (error)
        emitLine({{"ok", false}, {"error", QJsonObject{{"code", error.code}, {"message", error.message}, {"details", error.details}}},
                             {"uncertain", uncertain}, {"result", result}});
    else emitLine({{"ok", true}, {"result", result}});
    std::_Exit(0);
}

}
