// Opt-in live Backblaze B2 acceptance probe (#282). Synthetic files only, into an
// isolated prefix of a test bucket; every object it publishes is removed again through
// put.gd's own history deletion, and the receipts are printed.
//
//   b2_live_probe BUCKET PREFIX LINK_MODE DATA_DIR SMALL CANCEL INTERRUPT BIG < credentials.json
//
// Credentials arrive only on stdin as {"keyId","applicationKey"}; they are never printed,
// logged or written. The production B2Provider, putgd-b2-worker, OperationService and
// HistoryService run as in the resident: Test connection, a small upload, a large file,
// a cancellation at a third, a killed worker then Retry (resume), collisions (a stale name
// check refused before any bytes, then five true races with their exposure windows), and
// the deletion of every uploaded object.
// SMALL set to --abandon-only interrupts INTERRUPT's upload, marks the item failed and past
// retention, and lets artifact cleanup cancel the retained large file (#146).
// SMALL set to --publish uploads CANCEL alone and keeps it, printing its link;
// SMALL set to --delete DATA_DIR removes what an earlier --publish left, through History.
#include "app/HistoryService.h"
#include "storage/b2/B2Provider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
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
#include <future>
#include <mutex>
#include <thread>
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
std::pair<Error, QJsonObject> call(const std::function<void(JsonReply)>& start, qint64 timeoutMs = 600000) {
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
        if (fields.left(close).contains("(putgd-b2-worke") && fields.mid(close + 2).split(' ').value(1).toLongLong() == getpid()) found.append(entry.toLongLong());
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
template<class T, class F> T await(F start) {
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    start([promise](T value) { promise->set_value(std::move(value)); });
    return future.get();
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 9) { qCritical("usage: b2_live_probe BUCKET PREFIX LINK_MODE DATA_DIR SMALL CANCEL INTERRUPT BIG < credentials.json"); return 2; }
    CredentialStore credentials;
    {
        // Bounded stdin; the buffer is overwritten before it is released.
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
        auto raw = input.read(4096);
        const auto value = QJsonDocument::fromJson(raw).object();
        raw.fill('\0');
        if (!value["keyId"].isString() || !value["applicationKey"].isString() || value["keyId"].toString().isEmpty() || value["applicationKey"].toString().isEmpty()) {
            report({{"step", "credentials"}, {"ok", false}});
            return 2;
        }
        credentials.setSession("live282", {value["keyId"].toString(), value["applicationKey"].toString(), {}});
    }
    auto b2 = b2::defaults();
    b2["bucket"] = args[1];
    b2["objectPrefix"] = args[2];
    b2["linkMode"] = args[3];
    auto provider = std::make_shared<B2Provider>(credentials);
    ProviderRegistry registry;
    registry.add("backblaze-b2", provider);
    const QJsonObject reference{{"mode", "session"}, {"reference", QJsonValue::Null}};
    const Destination destination{"backblaze-b2", "live282", "live-revision", b2, reference};
    const QString mode = args[5];
    if (mode != "--delete") {
        // 1. Test connection: synthetic upload, the link put.gd hands out, exact removal, large-file probe.
        const auto [result, error] = await<std::pair<QJsonObject, Error>>([&](auto done) {
            provider->testDestination(destination, [](const QJsonObject&, std::function<void(Error)> ack) { ack({}); },
                                      [done](QJsonObject value, Error e) { done({value, e}); });
        });
        report({{"step", "testConnection"}, {"uploadStatus", result["uploadStatus"]}, {"publicReachable", result["publicReachable"]},
                {"cleanupStatus", result["cleanupStatus"]}, {"largeFile", result["largeFile"]}, {"bucketType", result["bucketType"]},
                {"linkMode", result["linkMode"]}, {"errorCode", error.code}, {"errorMessage", error.message}, {"stage", error.details["requestStage"]},
                {"httpStatus", error.details["httpStatus"]}, {"serviceCode", error.details["serviceCode"]}});
        if (mode == "--test-only") return error ? 1 : 0;
    }
    const QString root = QDir(args[4]).absolutePath();
    auto document = Settings::defaults();
    document["profiles"] = QJsonArray{QJsonObject{{"id", "live282"}, {"label", "#282 live acceptance"}, {"provider", "backblaze-b2"}, {"settings", b2}, {"credentials", reference}}};
    auto actions = document["actions"].toObject();
    auto clipboard = actions["clipboard"].toObject();
    clipboard["profileId"] = "live282";
    actions["clipboard"] = clipboard;
    document["actions"] = actions;
    auto archive = document["archive"].toObject();
    archive["root"] = root + "/archive";
    document["archive"] = archive;
    SettingsService settings(root + "/config.json", [&](const QString& id, const QJsonObject& value) { return registry.validate(id, value); });
    if (mode != "--delete" && !settings.save(document, {})) { report({{"step", "settings"}, {"ok", false}}); return 2; }
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
        const auto receipt = item["receipt"].toObject();
        report({{"step", name}, {"operationId", id}, {"invokeError", error.code}, {"state", op["state"]}, {"bytes", double(bytes)},
              {"uploadStatus", item["uploadStatus"]}, {"itemError", item["error"].toObject()["code"]}, {"key", receipt["objectReference"].toObject()["key"]},
              {"fileId", receipt["objectReference"].toObject()["fileId"]}, {"directUrl", receipt["directUrl"].toString().section('?', 0, 0)},
              {"authorizedLink", receipt["directUrl"].toString().contains("?Authorization=")}, {"linkExpiresAt", receipt["objectReference"].toObject()["linkExpiresAt"]},
              {"elapsedMs", elapsed}, {"mibPerSecond", elapsed > 0 ? bytes / 1048576.0 / (elapsed / 1000.0) : 0}, {"workerPeakRssKiB", peak}});
        return id;
    };
    QStringList uploaded;
    auto remember = [&](const QString& id) {
        for (const auto& item : inspect(id)["items"].toArray())
            if (item.toObject()["uploadStatus"] == "uploaded") uploaded.append(item.toObject()["artifact"].toObject()["id"].toString());
    };
    auto deleteAll = [&] {
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
                                        {"key", item["receipt"].toObject()["objectReference"].toObject()["key"]},
                                        {"fileId", item["receipt"].toObject()["objectReference"].toObject()["fileId"]}});
        }
        report({{"step", "deleteConfirmed"}, {"error", confirmError.code}, {"deleteOperationId", deletion}, {"state", finished["state"]}, {"items", receipts}});
    };
    auto stop = [&] { sampling = false; sampler->wait(); };
    if (mode == "--abandon-only") {
        const auto id = run("workerKilledAtThird", args[7], [&](const QString&) { for (auto pid : workers()) ::kill(pid_t(pid), SIGKILL); });
        stop();
        // Stand in for an attempt whose own cancel was refused: a failed item that still
        // journals its large file, older than retention.failedArtifactDays.
        Operation op;
        {
            HistoryStore store(operations.databasePath());
            op = store.operation(id);
            if (op.items.isEmpty() || op.items[0].uploadJournal["uploadId"].toString().isEmpty()) { report({{"step", "abandon"}, {"ok", false}, {"reason", "no journal"}}); return 1; }
            op.items[0].uploadStatus = "failed";
            op.created = "2000-01-01T00:00:00.000Z";
            if (store.saveOperation(op)) { report({{"step", "abandon"}, {"ok", false}, {"reason", "journal write"}}); return 1; }
        }
        report({{"step", "journaledLargeFile"}, {"fileId", op.items[0].uploadJournal["uploadId"]}});
        const auto [error, result] = call([&](JsonReply reply) { operations.cleanup(reply); }, 300000);
        const auto after = HistoryStore(operations.databasePath()).operation(id);
        report({{"step", "cleanupCancelsRetainedLargeFile"}, {"error", error.code}, {"removedOperations", result["removedOperations"]},
              {"abortedUploads", result["abortedUploads"]}, {"incompleteUploads", result["incompleteUploads"].toArray().size()},
              {"journalCleared", after.items.value(0).uploadJournal.isEmpty()}, {"artifactRemoved", !QFileInfo::exists(op.items[0].artifact.path)}});
        return error || result["abortedUploads"].toInt() != 1 ? 1 : 0;
    }
    if (mode == "--publish") {
        const auto id = run("publish", args[6], {});
        stop();
        // The full link (with its token for authorized links) goes to a private file for the
        // browser picture only; stdout carries it without the token.
        const auto url = inspect(id)["items"].toArray().first().toObject()["receipt"].toObject()["directUrl"].toString();
        QFile link(root + "/link.txt");
        if (!url.isEmpty() && link.open(QIODevice::WriteOnly)) { link.setPermissions(QFile::ReadOwner | QFile::WriteOwner); link.write(url.toUtf8()); }
        report({{"step", "published"}, {"directUrl", url.section('?', 0, 0)}, {"authorized", url.contains("?Authorization=")}});
        return url.isEmpty() ? 1 : 0;
    }
    if (mode == "--delete") {
        stop();
        HistoryQuery query;
        query.limit = 50;
        query.uploadStatus = "uploaded";
        const auto [listError, page] = call([&](JsonReply reply) { history.list(query, reply); });
        for (const auto& value : page["records"].toArray())
            if (value.toObject()["remoteStatus"].toString() != "removed") uploaded.append(value.toObject()["id"].toString());
        report({{"step", "existing"}, {"error", listError.code}, {"items", uploaded.size()}});
        deleteAll();
        return 0;
    }
    remember(run("smallUpload", args[5], {}));
    remember(run("largeFile", args[8], {}));
    remember(run("cancelAtThird", args[6], [&](const QString& id) { operations.cancel(id, [](Error, QJsonObject) {}); }));
    const auto interrupted = run("workerKilledAtThird", args[7], [&](const QString&) { for (auto pid : workers()) ::kill(pid_t(pid), SIGKILL); });
    {
        QElapsedTimer clock;
        clock.start();
        const auto [error, retried] = call([&](JsonReply reply) { operations.retry(interrupted, reply); });
        spin([&] { return operations.diagnostics()["activeOperations"].toInt() == 0 && clock.elapsed() > 500; }, 6 * 3600 * 1000);
        const auto item = inspect(interrupted)["items"].toArray().first().toObject();
        report({{"step", "retryResumes"}, {"retryError", error.code}, {"uploadStatus", item["uploadStatus"]}, {"itemError", item["error"].toObject()["code"]},
                {"fileId", item["receipt"].toObject()["objectReference"].toObject()["fileId"]}, {"elapsedMs", clock.elapsed()}});
        remember(interrupted);
    }
    stop();
    {
        // Collisions, live: a stale name check, then true races (#282).
        Artifact artifact;
        artifact.id = "live-collision";
        artifact.path = args[5];
        artifact.name = QFileInfo(args[5]).fileName();
        artifact.mime = "application/octet-stream";
        QFile file(artifact.path);
        file.open(QIODevice::ReadOnly);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        artifact.sha256 = QString::fromLatin1(hash.result().toHex());
        artifact.bytes = file.size();
        // (a) A stale name check: the name holds a visible file by the time the second
        // upload starts, so it must be refused before any bytes go up.
        const auto name = "collision-" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".bin";
        const auto checked = await<NameCheck>([&](auto cb) { provider->checkName(destination, name, cb); });
        UploadRequest first{destination, artifact, name, "live-collision-first", "absent", checked.objectReference, {}, std::make_shared<std::atomic_bool>(false)};
        auto second = first;
        second.publicationId = "live-collision-second";
        second.cancellation = std::make_shared<std::atomic_bool>(false);
        const auto a = await<UploadResult>([&](auto cb) { provider->upload(first, cb); });
        const auto b = await<UploadResult>([&](auto cb) { provider->upload(second, cb); });
        report({{"step", "staleNameCheck"}, {"checkStatus", checked.status}, {"firstStatus", a.status}, {"secondStatus", b.status}, {"secondError", b.error.code},
                {"secondStage", b.error.details["requestStage"]}});
        if (a.status == "uploaded") await<DeleteResult>([&](auto cb) { provider->remove({"live-collision-delete", a.receipt, {}, {}}, [](auto, auto ack) { ack({}); }, cb); });
        // (b) True races: two uploads of one name start together from the same name check.
        // A racer that sees the other removes its own version; exposedMs is how long B2
        // served its bytes at the name before that removal finished.
        for (int race = 1; race <= 5; ++race) {
            const auto raceName = "race-" + QString::number(race) + "-" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".bin";
            const auto raceCheck = await<NameCheck>([&](auto cb) { provider->checkName(destination, raceName, cb); });
            UploadRequest x{destination, artifact, raceName, "live-race-x", "absent", raceCheck.objectReference, {}, std::make_shared<std::atomic_bool>(false)};
            auto y = x;
            y.publicationId = "live-race-y";
            y.cancellation = std::make_shared<std::atomic_bool>(false);
            UploadResult rx, ry;
            std::thread one([&] { rx = await<UploadResult>([&](auto cb) { provider->upload(x, cb); }); });
            std::thread two([&] { ry = await<UploadResult>([&](auto cb) { provider->upload(y, cb); }); });
            one.join();
            two.join();
            const auto after = await<NameCheck>([&](auto cb) { provider->checkName(destination, raceName, cb); });
            report({{"step", "race"}, {"race", race}, {"x", rx.status}, {"y", ry.status}, {"xExposedMs", rx.error.details["exposedMs"]}, {"yExposedMs", ry.error.details["exposedMs"]},
                    {"xStage", rx.error.details["requestStage"]}, {"yStage", ry.error.details["requestStage"]}, {"nameAfter", after.status},
                    {"versionsAfter", after.objectReference["baseline"].toArray().size()}});
            for (const auto* winner : {&rx, &ry})
                if (winner->status == "uploaded")
                    await<DeleteResult>([&](auto cb) { provider->remove({"live-race-delete", winner->receipt, {}, {}}, [](auto, auto ack) { ack({}); }, cb); });
        }
    }
    deleteAll();
    return 0;
}
