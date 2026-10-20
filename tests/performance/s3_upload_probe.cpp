// Opt-in S3 upload probe for a local S3-compatible server (MinIO, moto) with the
// synthetic fixture credentials: never point it at a real account. It drives the
// production S3Provider and putgd-s3-worker and reports one JSON line per run:
// outcome, wall time, throughput, the worker's peak resident memory and part counts.
//
//   s3_upload_probe ENDPOINT BUCKET FILE MODE [THRESHOLD_BYTES [EXTRA_LANES]]
//   MODE: upload | single | cancel | interrupt | collision | probe
//   EXTRA_LANES: idle upload slots lent to a multipart upload (0-3, default 0, #174)
#include "storage/s3/S3Provider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QThread>
#include <QUuid>
#include <atomic>
#include <csignal>
#include <future>
#include <mutex>
#include <unistd.h>
using namespace putgd;
namespace {
struct Peak { qint64 rssKiB = 0, hwmKiB = 0; QList<qint64> pids; };
qint64 statusField(const QString& path, const QByteArray& name) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    for (const auto& line : file.readAll().split('\n'))
        if (line.startsWith(name)) return line.mid(name.size()).trimmed().split(' ').first().toLongLong();
    return 0;
}
// Samples every putgd-s3-worker child of this process every 50 ms.
class Sampler {
public:
    void start() {
        running_ = true;
        thread_ = QThread::create([this] {
            while (running_) {
                for (const auto& entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    bool numeric = false;
                    const auto pid = entry.toLongLong(&numeric);
                    if (!numeric) continue;
                    QFile stat("/proc/" + entry + "/stat");
                    if (!stat.open(QIODevice::ReadOnly)) continue;
                    const auto fields = stat.readAll();
                    const auto close = fields.lastIndexOf(')');
                    if (!fields.mid(0, close).contains("(putgd-s3-worke")) continue;
                    if (fields.mid(close + 2).split(' ').value(1).toLongLong() != getpid()) continue;
                    std::lock_guard lock(mutex_);
                    peak_.rssKiB = qMax(peak_.rssKiB, statusField("/proc/" + entry + "/status", "VmRSS:"));
                    peak_.hwmKiB = qMax(peak_.hwmKiB, statusField("/proc/" + entry + "/status", "VmHWM:"));
                    if (!peak_.pids.contains(pid)) peak_.pids.append(pid);
                }
                QThread::msleep(50);
            }
        });
        thread_->start();
    }
    Peak stop() { running_ = false; thread_->wait(); delete thread_; std::lock_guard lock(mutex_); return peak_; }
    QList<qint64> pids() { std::lock_guard lock(mutex_); return peak_.pids; }
private:
    std::atomic_bool running_ = false;
    QThread* thread_ = nullptr;
    std::mutex mutex_;
    Peak peak_;
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
    if (args.size() < 5) { qCritical("usage: s3_upload_probe ENDPOINT BUCKET FILE MODE [THRESHOLD_BYTES [EXTRA_LANES]]"); return 2; }
    const auto mode = args[4];
    S3UploadPolicy policy;
    if (args.size() > 5 && args[5].toLongLong() > 0) policy.multipartThresholdBytes = args[5].toLongLong();
    const int extraLanes = args.size() > 6 ? qBound(0, args[6].toInt(), s3MaximumPartLanes - 1) : 0;
    if (mode == "single") policy.multipartThresholdBytes = s3MaximumObjectBytes;
    CredentialStore credentials;
    S3Provider provider(credentials, policy);
    Destination destination{"s3", "probe", "probe-revision",
        {{"bucket", args[2]}, {"region", "us-east-1"}, {"endpoint", args[1]}, {"objectPrefix", "putgd-probe"},
         {"storageClass", "STANDARD"}, {"aclMode", "omit"}, {"publicUrlBase", QJsonValue::Null}, {"addressing", "path"},
         {"connectTimeoutMs", 5000}, {"requestTimeoutMs", 600000}, {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 30}},
        {{"mode", "session"}, {"reference", QJsonValue::Null}}};
    // The offline fixture's synthetic pair, also the local server's root user.
    credentials.setSession("probe", {"PUTGD_SYNTHETIC_ACCESS", "putgd-synthetic-secret-not-a-real-key", {}});
    if (mode == "probe") {
        const auto [result, error] = await<std::pair<QJsonObject, Error>>([&](auto done) {
            provider.testDestination(destination, [](const QJsonObject&, std::function<void(Error)> ack) { ack({}); },
                                     [done](QJsonObject value, Error e) { done({value, e}); });
        });
        printf("%s\n", QJsonDocument(QJsonObject{{"mode", mode}, {"multipart", result["multipart"]}, {"uploadStatus", result["uploadStatus"]},
                                                 {"errorCode", error.code}}).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    Artifact artifact;
    artifact.id = "probe-item";
    artifact.path = QFileInfo(args[3]).absoluteFilePath();
    artifact.name = QFileInfo(args[3]).fileName();
    artifact.mime = "application/octet-stream";
    artifact.category = "other";
    QFile file(artifact.path);
    if (!file.open(QIODevice::ReadOnly)) return 2;
    artifact.bytes = file.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    artifact.sha256 = QString::fromLatin1(hash.result().toHex());
    const auto name = QUuid::createUuid().toString(QUuid::WithoutBraces) + "-" + artifact.name;
    const auto checked = await<NameCheck>([&](auto done) { provider.checkName(destination, name, done); });
    if (checked.status != "absent") { printf("{\"error\":\"name check %s\"}\n", qPrintable(checked.status + " " + checked.error.code)); return 1; }
    UploadRequest request{destination, artifact, name, QUuid::createUuid().toString(QUuid::WithoutBraces), "absent", checked.objectReference, {}, std::make_shared<std::atomic_bool>(false)};
    std::mutex mutex;
    QList<QJsonObject> journal;
    request.checkpoint = [&](const QJsonObject& value, std::function<void(Error)> ack) { { std::lock_guard lock(mutex); journal.append(value); } ack({}); };
    request.extraLanes = [extraLanes](int wanted) { return qMin(wanted, extraLanes); };
    // Part records (#148): the most lanes any part saw, and the slowest part.
    std::atomic_int widest{0};
    std::atomic<qint64> slowestPartMs{0};
    request.trace = [&](const QJsonObject& record) {
        if (record["event"] != "part") return;
        widest = qMax(widest.load(), record["lanes"].toInt());
        slowestPartMs = qMax(slowestPartMs.load(), record["elapsedMs"].toInteger());
    };
    std::atomic<qint64> sent{0};
    std::atomic_bool stopped{false};
    // Wall time from the cancellation to the provider's result (#203).
    QElapsedTimer cancelClock;
    std::atomic<qint64> cancelledAtBytes{0};
    Sampler sampler;
    request.progress = [&](const UploadProgress& value) {
        if (value.bytesTransferred) sent = *value.bytesTransferred;
        // cancel: stop after a third; interrupt: kill the worker there instead, as a crash would.
        if ((mode == "cancel" || mode == "interrupt") && value.phase == "uploading" && *value.bytesTransferred > artifact.bytes / 3 && !stopped.exchange(true)) {
            if (mode == "cancel") {
                cancelledAtBytes = *value.bytesTransferred;
                cancelClock.start();
                request.cancellation->store(true);
            }
            else for (auto pid : sampler.pids()) ::kill(pid_t(pid), SIGKILL);
        }
    };
    QElapsedTimer clock;
    clock.start();
    sampler.start();
    auto result = await<UploadResult>([&](auto done) { provider.upload(request, done); });
    const auto elapsed = clock.elapsed();
    auto peak = sampler.stop();
    QJsonObject line{{"mode", mode}, {"bytes", double(artifact.bytes)}, {"status", result.status}, {"errorCode", result.error.code},
                     {"elapsedMs", elapsed}, {"mibPerSecond", elapsed > 0 ? double(artifact.bytes) / 1048576.0 / (elapsed / 1000.0) : 0},
                     {"workerPeakRssKiB", peak.rssKiB}, {"workerPeakHwmKiB", peak.hwmKiB}, {"workers", peak.pids.size()},
                     {"partBytes", double(s3PartBytes(artifact.bytes, policy))}, {"journals", journal.size()},
                     {"lastJournalPhase", journal.isEmpty() ? QString() : journal.last()["phase"].toString()},
                     {"incompleteUpload", result.error.details["incompleteUpload"].toBool()},
                     {"extraLanes", extraLanes}, {"widestLanes", widest.load()}, {"slowestPartMs", slowestPartMs.load()}};
    if (mode == "cancel" && cancelClock.isValid()) {
        line["cancelLatencyMs"] = cancelClock.elapsed();
        line["cancelledAtBytes"] = double(cancelledAtBytes.load());
    }
    if (mode == "interrupt" && !journal.isEmpty() && !journal.last().isEmpty()) {
        // Retry with the journal, as History's Retry would after a crash.
        request.resume = journal.last();
        request.cancellation = std::make_shared<std::atomic_bool>(false);
        request.progress = [&](const UploadProgress& value) { if (value.bytesTransferred) sent = *value.bytesTransferred; };
        clock.restart();
        sampler.start();
        result = await<UploadResult>([&](auto done) { provider.upload(request, done); });
        line["resumeStatus"] = result.status;
        line["resumeErrorCode"] = result.error.code;
        line["resumeElapsedMs"] = clock.elapsed();
        line["resumeWorkerPeakRssKiB"] = sampler.stop().rssKiB;
    }
    if (mode == "collision" && result.status == "uploaded") {
        // A second upload to the same key must not replace the first. moto enforces
        // If-None-Match on completion only over a single-request object, so the
        // probe first replaces the key's object with a small single PUT.
        S3Provider single(credentials, {s3MaximumObjectBytes, s3MinimumPartBytes});
        auto small = request;
        QFile marker(QDir::temp().filePath("putgd-probe-marker.txt"));
        if (marker.open(QIODevice::WriteOnly)) { marker.write("putgd probe marker\n"); marker.close(); }
        small.artifact.path = marker.fileName();
        small.artifact.bytes = marker.size();
        small.artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(QByteArray("putgd probe marker\n"), QCryptographicHash::Sha256).toHex());
        small.condition = "match";
        small.concurrencyToken = result.receipt.concurrencyToken;
        small.resume = {};
        small.checkpoint = {};
        small.cancellation = std::make_shared<std::atomic_bool>(false);
        const auto replaced = await<UploadResult>([&](auto done) { single.upload(small, done); });
        line["markerStatus"] = replaced.status;
        auto again = request;
        again.publicationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        again.resume = {};
        again.cancellation = std::make_shared<std::atomic_bool>(false);
        const auto second = await<UploadResult>([&](auto done) { provider.upload(again, done); });
        line["secondStatus"] = second.status;
        line["secondErrorCode"] = second.error.code;
    }
    if (result.status == "uploaded") line["etag"] = result.receipt.concurrencyToken;
    line["probeHwmKiB"] = statusField("/proc/self/status", "VmHWM:");
    printf("%s\n", QJsonDocument(line).toJson(QJsonDocument::Compact).constData());
    return 0;
}
