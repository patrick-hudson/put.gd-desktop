// Opt-in live S3 acceptance probe for multipart uploads (#57, #145). Synthetic files only,
// into an isolated prefix the owner approved; every object it creates is removed again
// through put.gd's own history deletion, and the receipts are printed.
//
//   s3_live_probe BUCKET REGION ENDPOINT PREFIX DATA_DIR SMALL CANCEL INTERRUPT BIG < credentials.json
//
// Credentials arrive only on stdin as {"accessKey","secretKey"}; they are never printed,
// logged or written. The production S3Provider, OperationService and HistoryService run as
// in the resident: Test connection (with its multipart probe), a small multipart upload,
// a cancellation at a third, a killed worker then Retry (resume), a large upload, and the
// deletion of every uploaded object. Output is one JSON line per step, without secrets.
// With SMALL set to --abandon-only (#146), it only interrupts INTERRUPT's upload, marks the
// item failed and its operation past retention, and lets artifact cleanup abort the
// retained multipart upload; nothing is published, so nothing needs deleting.
#include "app/HistoryService.h"
#include "storage/s3/S3Provider.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <unistd.h>
using namespace putgd;
namespace {
void report(const QJsonObject& line) { printf("%s\n", QJsonDocument(line).toJson(QJsonDocument::Compact).constData()); fflush(stdout); }
bool spin(const std::function<bool()>& done, qint64 timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    return true;
}
std::pair<Error, QJsonObject> call(const std::function<void(JsonReply)>& start, qint64 timeoutMs = 120000) {
    bool done = false;
    Error error;
    QJsonObject result;
    start([&](Error e, QJsonObject r) { error = e; result = r; done = true; });
    if (!spin([&] { return done; }, timeoutMs)) return {{"probe_timeout", "No reply in time", {}}, {}};
    return {error, result};
}
QList<qint64> workers() {
    QList<qint64> found;
    for (const auto& entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile stat("/proc/" + entry + "/stat");
        if (!stat.open(QIODevice::ReadOnly)) continue;
        const auto fields = stat.readAll();
        const auto close = fields.lastIndexOf(')');
        if (fields.left(close).contains("(putgd-s3-worke") && fields.mid(close + 2).split(' ').value(1).toLongLong() == getpid()) found.append(entry.toLongLong());
    }
    return found;
}
qint64 rssKiB(qint64 pid) {
    QFile file(QString("/proc/%1/status").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return 0;
    for (const auto& line : file.readAll().split('\n'))
        if (line.startsWith("VmRSS:")) return line.mid(6).trimmed().split(' ').first().toLongLong();
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
    if (args.size() != 10) { qCritical("usage: s3_live_probe BUCKET REGION ENDPOINT PREFIX DATA_DIR SMALL CANCEL INTERRUPT BIG < credentials.json"); return 2; }
    CredentialStore credentials;
    {
        // Bounded stdin; the buffer is overwritten before it is released.
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
        auto raw = input.read(4096);
        const auto value = QJsonDocument::fromJson(raw).object();
        raw.fill('\0');
        if (!value["accessKey"].isString() || !value["secretKey"].isString() || value["accessKey"].toString().isEmpty() || value["secretKey"].toString().isEmpty()) {
            report({{"step", "credentials"}, {"ok", false}});
            return 2;
        }
        credentials.setSession("live57", {value["accessKey"].toString(), value["secretKey"].toString(), {}});
    }
    const QJsonObject s3{{"bucket", args[1]}, {"region", args[2]}, {"endpoint", args[3]}, {"objectPrefix", args[4]},
                         {"storageClass", "STANDARD"}, {"aclMode", "omit"}, {"publicUrlBase", QJsonValue::Null}, {"addressing", "path"},
                         {"connectTimeoutMs", 10000}, {"requestTimeoutMs", 600000}, {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 60}};
    auto provider = std::make_shared<S3Provider>(credentials);
    ProviderRegistry registry;
    registry.add("s3", provider);
    const QJsonObject reference{{"mode", "session"}, {"reference", QJsonValue::Null}};
    // 1. Test connection, including the multipart permission probe.
    {
        const Destination destination{"s3", "live57", "live-revision", s3, reference};
        bool done = false;
        QJsonObject result;
        Error error;
        provider->testDestination(destination, [](const QJsonObject&, std::function<void(Error)> ack) { ack({}); },
                                  [&](QJsonObject value, Error e) { result = value; error = e; done = true; });
        spin([&] { return done; }, 300000);
        report({{"step", "testConnection"}, {"uploadStatus", result["uploadStatus"]}, {"cleanupStatus", result["cleanupStatus"]},
              {"multipart", result["multipart"]}, {"publicReachable", result["publicReachable"]}, {"errorCode", error.code},
              {"stage", error.details["requestStage"]}, {"httpStatus", error.details["httpStatus"]}, {"serviceCode", error.details["serviceCode"]}});
        if (args.value(6) == "--test-only") return error ? 1 : 0;
    }
    const QString root = QDir(args[5]).absolutePath();
    auto document = Settings::defaults();
    document["profiles"] = QJsonArray{QJsonObject{{"id", "live57"}, {"label", "#57 live acceptance"}, {"provider", "s3"}, {"settings", s3}, {"credentials", reference}}};
    auto actions = document["actions"].toObject();
    auto clipboard = actions["clipboard"].toObject();
    clipboard["profileId"] = "live57";
    actions["clipboard"] = clipboard;
    document["actions"] = actions;
    auto archive = document["archive"].toObject();
    archive["root"] = root + "/archive";
    document["archive"] = archive;
    SettingsService settings(root + "/config.json", [&](const QString& id, const QJsonObject& value) { return registry.validate(id, value); });
    if (!settings.save(document, {})) { report({{"step", "settings"}, {"ok", false}}); return 2; }
    Files source;
    OperationService operations(root + "/data", settings, registry, source);
    HistoryService history(operations);
    if (!spin([&] { return operations.ready(); }, 30000)) { report({{"step", "journal"}, {"ok", false}}); return 2; }
    std::atomic_bool sampling = true;
    std::mutex mutex;
    qint64 peakWorkerKiB = 0;
    auto sampler = QThread::create([&] {
        while (sampling) {
            qint64 total = 0;
            for (auto pid : workers()) total += rssKiB(pid);
            { std::lock_guard lock(mutex); peakWorkerKiB = qMax(peakWorkerKiB, total); }
            QThread::msleep(100);
        }
    });
    sampler->start();
    auto inspect = [&](const QString& id) { return call([&](JsonReply reply) { operations.inspect(id, reply); }).second; };
    auto transferred = [&](const QString& id) -> qint64 {
        for (const auto& value : operations.activeObservations())
            if (value.toObject()["id"] == id)
                for (const auto& item : value.toObject()["items"].toArray()) return item.toObject()["uploadProgress"].toObject()["bytesTransferred"].toInteger();
        return 0;
    };
    // Runs one clipboard file action; `at` fires once when a third has been sent.
    auto run = [&](const QString& name, const QString& file, const std::function<void(const QString&)>& at) {
        { std::lock_guard lock(mutex); peakWorkerKiB = 0; }
        source.next = {file};
        const auto bytes = QFileInfo(file).size();
        QElapsedTimer clock;
        clock.start();
        const auto [error, accepted] = call([&](JsonReply reply) { operations.invoke("clipboard", reply); });
        const auto id = accepted["operationId"].toString();
        bool fired = false;
        spin([&] {
            if (at && !fired && transferred(id) > bytes / 3) { fired = true; at(id); }
            return !id.isEmpty() && operations.diagnostics()["activeOperations"].toInt() == 0;
        }, 6 * 3600 * 1000);
        const auto elapsed = clock.elapsed();
        const auto op = inspect(id);
        const auto item = op["items"].toArray().first().toObject();
        qint64 peak;
        { std::lock_guard lock(mutex); peak = peakWorkerKiB; }
        report({{"step", name}, {"operationId", id}, {"invokeError", error.code}, {"state", op["state"]}, {"bytes", double(bytes)},
              {"uploadStatus", item["uploadStatus"]}, {"itemError", item["error"].toObject()["code"]}, {"key", item["receipt"].toObject()["objectReference"].toObject()["key"]},
              {"etag", item["receipt"].toObject()["concurrencyToken"]}, {"elapsedMs", elapsed},
              {"mibPerSecond", elapsed > 0 ? bytes / 1048576.0 / (elapsed / 1000.0) : 0}, {"workerPeakRssKiB", peak}});
        return id;
    };
    QStringList uploaded;
    auto remember = [&](const QString& id) {
        for (const auto& item : inspect(id)["items"].toArray())
            if (item.toObject()["uploadStatus"] == "uploaded") uploaded.append(item.toObject()["artifact"].toObject()["id"].toString());
    };
    if (args.value(6) == "--abandon-only") {
        const auto id = run("workerKilledAtThird", args[8], [&](const QString&) { for (auto pid : workers()) ::kill(pid_t(pid), SIGKILL); });
        sampling = false;
        sampler->wait();
        // Stand in for an attempt whose own abort was refused: a failed item that still
        // journals its upload, older than retention.failedArtifactDays.
        Operation op;
        {
            HistoryStore store(operations.databasePath());
            op = store.operation(id);
            if (op.items.isEmpty() || op.items[0].uploadJournal["uploadId"].toString().isEmpty()) { report({{"step", "abandon"}, {"ok", false}, {"reason", "no journal"}}); return 1; }
            op.items[0].uploadStatus = "failed";
            op.created = "2000-01-01T00:00:00.000Z";
            if (store.saveOperation(op)) { report({{"step", "abandon"}, {"ok", false}, {"reason", "journal write"}}); return 1; }
        }
        const auto [error, result] = call([&](JsonReply reply) { operations.cleanup(reply); }, 300000);
        const auto after = HistoryStore(operations.databasePath()).operation(id);
        report({{"step", "cleanupAbortsRetainedUpload"}, {"error", error.code}, {"removedOperations", result["removedOperations"]},
              {"abortedUploads", result["abortedUploads"]}, {"incompleteUploads", result["incompleteUploads"].toArray().size()},
              {"journalCleared", after.items.value(0).uploadJournal.isEmpty()}, {"artifactRemoved", !QFileInfo::exists(op.items[0].artifact.path)}});
        return error || result["abortedUploads"].toInt() != 1 ? 1 : 0;
    }
    remember(run("smallMultipart", args[6], {}));
    remember(run("cancelAtThird", args[7], [&](const QString& id) { operations.cancel(id, [](Error, QJsonObject) {}); }));
    const auto interrupted = run("workerKilledAtThird", args[8], [&](const QString&) { for (auto pid : workers()) ::kill(pid_t(pid), SIGKILL); });
    {
        QElapsedTimer clock;
        clock.start();
        const auto [error, retried] = call([&](JsonReply reply) { operations.retry(interrupted, reply); });
        spin([&] { return operations.diagnostics()["activeOperations"].toInt() == 0 && clock.elapsed() > 500; }, 6 * 3600 * 1000);
        const auto item = inspect(interrupted)["items"].toArray().first().toObject();
        report({{"step", "retryResumes"}, {"retryError", error.code}, {"uploadStatus", item["uploadStatus"]}, {"itemError", item["error"].toObject()["code"]},
              {"etag", item["receipt"].toObject()["concurrencyToken"]}, {"elapsedMs", clock.elapsed()}});
        remember(interrupted);
    }
    remember(run("largeMultipart", args[9], {}));
    sampling = false;
    sampler->wait();
    // Remove every uploaded object through put.gd's history deletion and keep its receipts.
    const auto [prepareError, preview] = call([&](JsonReply reply) { history.prepareDelete(uploaded, reply); });
    report({{"step", "deletePrepare"}, {"error", prepareError.code}, {"targets", preview["targets"].toArray().size()}});
    const auto [confirmError, accepted] = call([&](JsonReply reply) { history.deleteConfirmed(uploaded, preview["acknowledgement"].toString(), reply); });
    const auto deletion = accepted["deleteOperationId"].toString();
    QJsonObject finished;
    spin([&] {
        finished = call([&](JsonReply reply) { history.inspectDelete(deletion, reply); }).second;
        return !QStringList{"accepted", "running"}.contains(finished["state"].toString());
    }, 600000);
    QJsonArray receipts;
    for (const auto& value : finished["items"].toArray()) {
        const auto item = value.toObject();
        receipts.append(QJsonObject{{"historyId", item["historyId"].toString(item["id"].toString())}, {"status", item["status"]},
                                    {"key", item["receipt"].toObject()["objectReference"].toObject()["key"]}});
    }
    report({{"step", "deleteConfirmed"}, {"error", confirmError.code}, {"deleteOperationId", deletion}, {"state", finished["state"]}, {"items", receipts}});
    return 0;
}
