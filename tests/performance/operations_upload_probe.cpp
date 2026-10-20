// Opt-in end-to-end probe for concurrent uploads (#28) against a local S3-compatible
// server with the synthetic fixture credentials: never point it at a real account.
// It runs the production OperationService (import copy, naming, slots, history) with
// the production S3Provider and putgd-s3-worker, starts one clipboard file action per
// group, and reports one JSON line: per-file finish times, the most worker processes
// alive at once, and the peak resident memory of this process plus its workers.
//
//   operations_upload_probe ENDPOINT BUCKET CONCURRENCY GROUP [GROUP...]
//   GROUP: comma-separated absolute file paths, started 200 ms after the previous group
#include "app/OperationService.h"
#include "storage/s3/S3Provider.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <mutex>
#include <unistd.h>
using namespace putgd;
namespace {
qint64 statusKiB(const QString& path, const QByteArray& name) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    for (const auto& line : file.readAll().split('\n'))
        if (line.startsWith(name)) return line.mid(name.size()).trimmed().split(' ').first().toLongLong();
    return 0;
}
class Files final : public SourceAdapter {
public:
    QStringList next;
    quint64 clipboardRevision() const override { return 0; }
    QJsonObject capabilities() const override { return {{"clipboardImport", QJsonArray{"files"}}}; }
    void acquire(const QString&, const ResourceLimits&, std::function<void(PayloadResult)> reply) override {
        ClipboardPayload payload;
        payload.kind = "files";
        payload.files = next;
        reply({payload, {}});
    }
    void cancelAcquisition() override {}
    void copy(const CopyPayload&, std::function<void(Error)> reply) override { reply({}); }
};
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 5) { qCritical("usage: operations_upload_probe ENDPOINT BUCKET CONCURRENCY GROUP [GROUP...]"); return 2; }
    QTemporaryDir root;
    CredentialStore credentials;
    auto s3 = std::make_shared<S3Provider>(credentials);
    ProviderRegistry registry;
    registry.add("s3", s3);
    auto document = Settings::defaults();
    document["profiles"] = QJsonArray{QJsonObject{{"id", "probe"}, {"label", "Local probe"}, {"provider", "s3"},
        {"settings", QJsonObject{{"bucket", args[2]}, {"region", "us-east-1"}, {"endpoint", args[1]}, {"objectPrefix", "putgd-probe"},
                                 {"storageClass", "STANDARD"}, {"aclMode", "omit"}, {"publicUrlBase", QJsonValue::Null}, {"addressing", "path"},
                                 {"connectTimeoutMs", 5000}, {"requestTimeoutMs", 600000}, {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 30}}},
        {"credentials", QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}}}}};
    auto actions = document["actions"].toObject();
    auto clipboard = actions["clipboard"].toObject();
    clipboard["profileId"] = "probe";
    actions["clipboard"] = clipboard;
    document["actions"] = actions;
    auto limits = document["limits"].toObject();
    limits["concurrentUploads"] = args[3].toInt();
    document["limits"] = limits;
    auto archive = document["archive"].toObject();
    archive["root"] = root.filePath("archive");
    document["archive"] = archive;
    SettingsService settings(root.filePath("config.json"), [&](const QString& id, const QJsonObject& value) { return registry.validate(id, value); });
    if (auto saved = settings.save(document, {}); saved.error) { qCritical("%s", qPrintable(saved.error.message)); return 2; }
    // The offline fixture's synthetic pair, also the local server's root user.
    credentials.setSession("probe", {"PUTGD_SYNTHETIC_ACCESS", "putgd-synthetic-secret-not-a-real-key", {}});
    Files source;
    OperationService operations(root.filePath("data"), settings, registry, source);
    std::atomic_bool running = true;
    std::mutex mutex;
    qint64 peakTotalKiB = 0, peakWorkerKiB = 0;
    int mostWorkers = 0;
    auto sampler = QThread::create([&] {
        while (running) {
            qint64 total = statusKiB("/proc/self/status", "VmRSS:"), workers = 0;
            int alive = 0;
            for (const auto& entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QFile stat("/proc/" + entry + "/stat");
                if (!stat.open(QIODevice::ReadOnly)) continue;
                const auto fields = stat.readAll();
                const auto close = fields.lastIndexOf(')');
                if (!fields.left(close).contains("(putgd-s3-worke") || fields.mid(close + 2).split(' ').value(1).toLongLong() != getpid()) continue;
                const auto rss = statusKiB("/proc/" + entry + "/status", "VmRSS:");
                workers += rss;
                ++alive;
            }
            {
                std::lock_guard lock(mutex);
                peakTotalKiB = qMax(peakTotalKiB, total + workers);
                peakWorkerKiB = qMax(peakWorkerKiB, workers);
                mostWorkers = qMax(mostWorkers, alive);
            }
            QThread::msleep(50);
        }
    });
    sampler->start();
    QElapsedTimer clock;
    QStringList ids;
    int finished = 0;
    QObject::connect(&operations, &OperationService::resultReady, &app, [&](const QString&) { ++finished; });
    QObject::connect(&operations, &OperationService::initialized, &app, [&] {
        clock.start();
        for (int group = 4; group < args.size(); ++group)
            QTimer::singleShot((group - 4) * 200, &app, [&, group] {
                source.next = args[group].split(',');
                operations.invoke("clipboard", [&](Error error, QJsonObject result) {
                    if (error) qCritical("invoke: %s", qPrintable(error.code));
                    ids.append(result["operationId"].toString());
                });
            });
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (finished < args.size() - 4 || operations.diagnostics()["activeOperations"].toInt() != 0) return;
        poll.stop();
        const auto elapsed = clock.elapsed();
        running = false;
        sampler->wait();
        QJsonArray groups;
        int pending = ids.size();
        for (const auto& id : ids)
            operations.inspect(id, [&, id](Error, QJsonObject op) {
                QJsonArray files;
                for (const auto& value : op["items"].toArray()) {
                    const auto item = value.toObject();
                    files.append(QJsonObject{{"name", item["candidate"]}, {"bytes", item["artifact"].toObject()["bytes"]}, {"status", item["uploadStatus"]}});
                }
                groups.append(QJsonObject{{"state", op["state"]}, {"files", files}, {"created", op["created"]}});
                if (--pending) return;
                std::lock_guard lock(mutex);
                printf("%s\n", QJsonDocument(QJsonObject{{"concurrency", args[3].toInt()}, {"elapsedMs", elapsed}, {"operations", groups},
                                                         {"mostWorkersAtOnce", mostWorkers}, {"peakWorkersRssKiB", peakWorkerKiB},
                                                         {"peakTotalRssKiB", peakTotalKiB}}).toJson(QJsonDocument::Compact).constData());
                app.quit();
            });
    });
    poll.start(100);
    // Finish times per operation come from the log-free resultReady order; print them too.
    QObject::connect(&operations, &OperationService::resultReady, &app, [&](const QString& id) {
        fprintf(stderr, "{\"finished\":\"%s\",\"atMs\":%lld}\n", qPrintable(id), clock.elapsed());
    });
    return app.exec();
}
