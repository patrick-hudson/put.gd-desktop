// putgd-b2-worker: one Backblaze B2 native-API request per process (#282).
//
// The resident writes one request line on stdin: {schemaVersion:1, method, destination,
// credentials:{keyId, applicationKey}, ...}. Credentials arrive only there, never in
// argv or the environment, and are never echoed. Later stdin lines are {"ack":true} or
// {"ack":false,"error":{...}} for a checkpoint, and {"cancel":true}. A closed stdin
// cancels. The worker writes newline JSON on stdout: {"event":"progress",...},
// {"event":"checkpoint","journal":{...}} (it then waits for the ack before relying on
// what it journaled), and one final {"ok":true,"result":{...}} or
// {"ok":false,"error":{...},"uncertain":bool}. All B2 HTTP happens here (Qt Network);
// the resident links none of it.
#include "B2Common.h"
#include "core/BuildInfo.h"
#include "core/CaBundle.h"
#include "core/StorageProvider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <cstdlib>
#include <memory>
namespace putgd::b2 {
namespace {
constexpr qint64 maximumLine = 256 * 1024;
std::atomic_bool cancelled{false};
std::mutex controlMutex;
std::condition_variable controlChanged;
std::deque<QJsonObject> acks;
bool parentGone = false;

// One newline-terminated line from stdin (portable: the worker also builds on Windows).
QByteArray readLine(bool& eof) {
    QByteArray line;
    while (true) {
        const int c = std::fgetc(stdin);
        if (c == EOF) { eof = true; return line; }
        if (c == '\n') return line;
        if (line.size() >= maximumLine) { eof = true; return {}; }
        line.append(char(c));
    }
}
std::mutex outputMutex;
void emitLine(const QJsonObject& value) {
    auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    std::lock_guard lock(outputMutex);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}
// Control lines after the request: acknowledgements and cancellation.
void readControl() {
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
Error checkpoint(const QJsonObject& journal) {
    emitLine({{"event", "checkpoint"}, {"journal", journal}});
    std::unique_lock lock(controlMutex);
    if (!controlChanged.wait_for(lock, std::chrono::minutes(5), [] { return !acks.empty() || parentGone; }))
        return {"b2_checkpoint_timeout", "The resident did not save the upload journal in time", {}};
    if (acks.empty()) return {"cancelled", "The resident stopped this request", {}};
    const auto ack = acks.front();
    acks.pop_front();
    if (ack["ack"].toBool()) return {};
    const auto e = ack["error"].toObject();
    return {e["code"].toString("checkpoint_failed"), e["message"].toString("The upload journal could not be saved"), {}};
}
Error stoppedError() { return {"cancelled", "Cancellation requested; sent requests may still have taken effect", {}}; }

// A request body that can be read exactly once. Qt Network silently sends a request
// again when the server closes the connection before replying; a body it cannot
// rewind turns that into ContentReSendError, so no upload, finish or deletion is ever
// replayed behind put.gd's back. The worker then reconciles, as for any lost reply.
class OneShot final : public QIODevice {
public:
    OneShot(QByteArray bytes) : bytes_(std::move(bytes)), length_(bytes_.size()) {}
    OneShot(const QString& path, qint64 offset, qint64 length) : file_(path), offset_(offset), length_(length), fromFile_(true) {}
    bool open(OpenMode mode) override {
        if (fromFile_ && (!file_.open(QIODevice::ReadOnly) || !file_.seek(offset_))) return false;
        return QIODevice::open(mode);
    }
    void close() override { if (fromFile_) file_.close(); QIODevice::close(); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (length_ - read_) + QIODevice::bytesAvailable(); }
    bool atEnd() const override { return read_ >= length_ && QIODevice::bytesAvailable() == 0; }
    bool reset() override { return false; }
    qint64 size() const override { return length_; }
protected:
    qint64 readData(char* data, qint64 maximum) override {
        const auto want = qMin(maximum, length_ - read_);
        if (want <= 0) return read_ >= length_ ? -1 : 0;
        qint64 n = 0;
        if (fromFile_) n = file_.read(data, want);
        else { std::memcpy(data, bytes_.constData() + read_, size_t(want)); n = want; }
        if (n > 0) read_ += n;
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QByteArray bytes_;
    QFile file_;
    qint64 offset_ = 0, length_ = 0, read_ = 0;
    bool fromFile_ = false;
};

struct Response {
    int status = 0;                 // HTTP status; 0 when no reply arrived
    QByteArray body;
    QJsonObject json;
    bool transport = false;         // no HTTP reply: the request may or may not have taken effect
    bool aborted = false;           // cancelled or past the deadline
    QString detail;                 // bounded transport description, never request data
    bool ok() const { return status >= 200 && status < 300; }
    QString code() const { return json["code"].toString(); }
};

struct Session {
    QString apiUrl, downloadUrl, token, accountId, bucketId, bucketType;
    QStringList capabilities;
    QJsonObject allowed;
};

class Client {
public:
    Client(QJsonObject settings, QJsonObject credentials) : settings_(std::move(settings)), credentials_(std::move(credentials)) {
        nam_.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    }
    ~Client() {
        // Overwrite the copied key before release.
        auto key = credentials_["applicationKey"].toString();
        key.fill(QChar('\0'));
        credentials_ = {};
    }
    Session session;
    std::function<void(qint64)> sentBytes;  // body bytes the transport has read for this request

    Response send(const QByteArray& verb, const QUrl& url, const QList<QPair<QByteArray, QByteArray>>& headers, const QByteArray& body = {}, QIODevice* device = nullptr) {
        QNetworkRequest request(url);
        for (const auto& [name, value] : headers) request.setRawHeader(name, value);
        const int deadline = settings_["requestTimeoutMs"].toInt(600000);
        request.setTransferTimeout(qMax(settings_["connectTimeoutMs"].toInt(10000), 30000));
        QNetworkReply* reply = nullptr;
        std::unique_ptr<OneShot> owned;
        if (verb == "GET") reply = nam_.get(request);
        else {
            if (!device) {
                owned = std::make_unique<OneShot>(body);
                owned->open(QIODevice::ReadOnly);
                device = owned.get();
                request.setHeader(QNetworkRequest::ContentLengthHeader, body.size());
            }
            request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
            reply = nam_.sendCustomRequest(request, verb, device);
        }
        QEventLoop loop;
        Response out;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        if (sentBytes) QObject::connect(reply, &QNetworkReply::uploadProgress, &loop, [this](qint64 sent, qint64) { sentBytes(sent); });
        QTimer poll;
        poll.setInterval(50);
        QElapsedTimer clock;
        clock.start();
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (cancelled.load() || clock.elapsed() > deadline) { out.aborted = true; reply->abort(); }
        });
        poll.start();
        if (!reply->isFinished()) loop.exec();
        poll.stop();
        out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        out.body = reply->readAll().left(4 * 1024 * 1024);
        out.json = QJsonDocument::fromJson(out.body).object();
        if (out.status == 0) {
            out.transport = true;
            out.detail = out.aborted ? (cancelled.load() ? "cancelled" : "deadline") : QString("network error %1").arg(int(reply->error()));
        }
        reply->deleteLater();
        return out;
    }
    QUrl api(const QString& name) const { return QUrl(session.apiUrl + "/b2api/v3/" + name); }
    // A JSON API call with the account token. Reads retry transient failures; writes
    // are sent once and the caller reconciles an uncertain outcome.
    Response call(const QString& name, const QJsonObject& body, bool idempotent) {
        Response out;
        for (int attempt = 0; attempt < (idempotent ? 3 : 1); ++attempt) {
            if (attempt) QThread::msleep(400 * attempt);
            out = send("POST", api(name), {{"Authorization", session.token.toUtf8()}, {"Content-Type", "application/json"}},
                       QJsonDocument(body).toJson(QJsonDocument::Compact));
            if (out.aborted || out.ok()) return out;
            if (out.status == 401 && out.code() == "expired_auth_token" && !authorize()) continue;
            if (!(out.transport || out.status == 429 || out.status >= 500)) return out;
        }
        return out;
    }
    Error authorize() {
        const auto basic = QByteArray(credentials_["keyId"].toString().toUtf8() + ':' + credentials_["applicationKey"].toString().toUtf8()).toBase64();
        Response r;
        for (int attempt = 0; attempt < 3; ++attempt) {
            r = send("GET", QUrl(settings_["apiUrl"].toString() + "/b2api/v3/b2_authorize_account"), {{"Authorization", "Basic " + basic}});
            if (r.aborted || r.ok() || !(r.transport || r.status >= 500 || r.status == 429)) break;
            QThread::msleep(400 * (attempt + 1));
        }
        if (!r.ok()) return failure(r, "authorize");
        const auto storage = r.json["apiInfo"].toObject()["storageApi"].toObject();
        session.apiUrl = storage["apiUrl"].toString();
        session.downloadUrl = storage["downloadUrl"].toString();
        session.token = r.json["authorizationToken"].toString();
        session.accountId = r.json["accountId"].toString();
        // b2api v3 lists the key's capabilities and bucket restriction in storageApi
        // itself; older shapes nest them under "allowed".
        session.allowed = storage.contains("allowed") ? storage["allowed"].toObject() : storage;
        session.capabilities.clear();
        for (const auto& c : session.allowed["capabilities"].toArray()) session.capabilities << c.toString();
        if (session.apiUrl.isEmpty() || session.downloadUrl.isEmpty() || session.token.isEmpty())
            return {"b2_protocol", "Backblaze B2 returned an incomplete authorization", {{"requestStage", "authorize"}}};
        return {};
    }
    // The bucket's ID, from the key's own restriction when it names this bucket.
    Error resolveBucket() {
        const auto name = settings_["bucket"].toString();
        QJsonArray restricted = session.allowed["buckets"].toArray();
        if (restricted.isEmpty() && session.allowed["bucketId"].isString())
            restricted.append(QJsonObject{{"id", session.allowed["bucketId"]}, {"name", session.allowed["bucketName"]}});
        for (const auto& value : restricted)
            if (value.toObject()["name"].toString() == name) session.bucketId = value.toObject()["id"].toString();
        if (!restricted.isEmpty() && session.bucketId.isEmpty())
            return {"b2_key_restricted", "This application key is restricted to another bucket. Create a key for bucket " + name + ".", {{"requestStage", "bucket"}}};
        const auto r = call("b2_list_buckets", {{"accountId", session.accountId}, {"bucketName", name}}, true);
        if (r.ok()) {
            const auto buckets = r.json["buckets"].toArray();
            if (!buckets.isEmpty()) {
                session.bucketId = buckets[0].toObject()["bucketId"].toString(session.bucketId);
                session.bucketType = buckets[0].toObject()["bucketType"].toString();
            }
        } else if (session.bucketId.isEmpty()) return failure(r, "bucket");
        if (session.bucketId.isEmpty())
            return {"b2_bucket_not_found", "Backblaze B2 could not find bucket " + name + ". Check the bucket name; it must already exist.", {{"requestStage", "bucket"}}};
        return {};
    }
    // Definite B2 refusals carry the stage, HTTP status and an allowlisted B2 code.
    static Error failure(const Response& r, const QString& stage) {
        static const QStringList known{"bad_request", "bad_auth_token", "expired_auth_token", "unauthorized", "cap_exceeded",
                                       "transaction_cap_exceeded", "storage_cap_exceeded", "download_cap_exceeded", "bad_bucket_id",
                                       "no_such_file", "file_not_present", "not_found", "too_many_requests", "service_unavailable",
                                       "request_timeout", "access_denied", "no_payment_history", "file_state_none", "file_state_unknown"};
        QJsonObject details{{"requestStage", stage}, {"httpStatus", r.status}, {"serviceCode", known.contains(r.code()) ? r.code() : QString("unclassified")}};
        if (r.aborted && cancelled.load()) return {"cancelled", "Cancellation requested; sent requests may still have taken effect", details};
        if (r.transport) {
            details["transport"] = r.detail;
            return {"b2_transport_unknown", "Backblaze B2 could not be reached reliably (" + r.detail + "). Check the network; any uncertain upload or cleanup stays recorded.", details};
        }
        const auto code = r.code();
        if (stage == "authorize" && r.status == 401)
            return {"b2_authentication_failed", "Backblaze B2 did not accept the application key ID and key. Check both, and that the key is still active; then Store credentials again.", details};
        if (r.status == 401 && code == "unauthorized")
            return {"b2_permission_denied", "The application key lacks a capability this step needs (" + stage + "). Give the key listBuckets, listFiles, readFiles, writeFiles and deleteFiles, plus shareFiles for authorized links.", details};
        if (r.status == 403 || code.endsWith("cap_exceeded"))
            return {"b2_cap_exceeded", "Backblaze B2 refused the request because an account cap or billing limit was reached. Raise the cap in the B2 account, then retry.", details};
        if (r.status == 429) return {"b2_rate_limited", "Backblaze B2 is limiting requests. Wait before retrying.", details};
        if (r.status >= 500) return {"b2_service_unavailable", "Backblaze B2 returned a server error. Retry after it recovers; any uncertain upload stays recorded.", details};
        if (code == "bad_bucket_id") return {"b2_bucket_not_found", "Backblaze B2 no longer has this bucket.", details};
        return {"b2_request_failed", QString("Backblaze B2 refused the request (HTTP %1, %2).").arg(r.status).arg(known.contains(code) ? code : QString("unclassified")), details};
    }
    // Every version of exactly this name, newest first; nullopt when there are more
    // than the bound or the listing failed.
    std::optional<QJsonArray> versions(const QString& name, Error& error) {
        const auto r = call("b2_list_file_versions", {{"bucketId", session.bucketId}, {"startFileName", name}, {"prefix", name}, {"maxFileCount", 100}}, true);
        if (!r.ok()) { error = failure(r, "list_versions"); return std::nullopt; }
        QJsonArray out;
        for (const auto& value : r.json["files"].toArray())
            if (value.toObject()["fileName"].toString() == name) out.append(value);
        if (out.size() >= 100 && r.json["nextFileName"].toString() == name) {
            error = {"b2_versions_unbounded", "This file name has more than 100 versions in B2; choose another folder or name.", {{"requestStage", "list_versions"}}};
            return std::nullopt;
        }
        return out;
    }
    QJsonObject settings_;
private:
    QJsonObject credentials_;
    QNetworkAccessManager nam_;
};

// File info travels percent-encoded in X-Bz-Info-* headers and as plain JSON for large
// files; put.gd's values (IDs and hex digests) need no encoding beyond '%'.
QByteArray infoHeader(const QString& value) { return QUrl::toPercentEncoding(value, "/:"); }
QString info(const QJsonObject& file, const char* name) { return QUrl::fromPercentEncoding(file["fileInfo"].toObject()[name].toString().toUtf8()); }

struct Hashes { QByteArray sha1, sha256; qint64 bytes = 0; };
std::optional<Hashes> hashFile(const QString& path, qint64 offset = 0, qint64 length = -1, bool sha256 = true) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) return std::nullopt;
    QCryptographicHash one(QCryptographicHash::Sha1), two(QCryptographicHash::Sha256);
    Hashes out;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (length < 0 || out.bytes < length) {
        const auto want = length < 0 ? buffer.size() : qMin<qint64>(buffer.size(), length - out.bytes);
        const auto n = file.read(buffer.data(), want);
        if (n < 0) return std::nullopt;
        if (n == 0) break;
        one.addData(QByteArrayView(buffer.constData(), n));
        if (sha256) two.addData(QByteArrayView(buffer.constData(), n));
        out.bytes += n;
        if (cancelled.load()) return std::nullopt;
    }
    out.sha1 = one.result().toHex();
    out.sha256 = two.result().toHex();
    return out;
}

class Worker {
public:
    Worker(const QJsonObject& request) : request_(request), destination_(Destination::fromJson(request["destination"].toObject())),
          client_(destination_.settings, request["credentials"].toObject()) { request_.remove("credentials"); }
    QJsonObject run(Error& error, bool& uncertain) {
        const auto method = request_["method"].toString();
        if (auto e = validateSettings(destination_.settings)) { error = e; return {}; }
        if (destination_.providerId != "backblaze-b2") { error = {"b2_protocol", "Request is not for a Backblaze B2 destination", {}}; return {}; }
        if (auto e = client_.authorize()) { error = e; return {}; }
        if (auto e = client_.resolveBucket()) { error = e; return {}; }
        if (method == "checkName") return checkName(error);
        if (method == "upload") return upload(error, uncertain);
        if (method == "reconcile") return reconcile(error);
        if (method == "stat") return stat(error);
        if (method == "remove") return remove(error, uncertain);
        if (method == "abandon") return abandon(error);
        if (method == "testDestination") return testDestination(error);
        error = {"b2_protocol", "Unsupported worker method", {}};
        return {};
    }
private:
    QJsonObject request_;
    Destination destination_;
    Client client_;
    QJsonObject settings() const { return destination_.settings; }

    void progress(const QString& phase, qint64 sent, qint64 total) {
        static QElapsedTimer last;
        if (phase == "uploading" && last.isValid() && last.elapsed() < 250 && sent < total) return;
        last.start();
        UploadProgress value;
        value.phase = phase;
        value.bytesTransferred = sent;
        value.bytesTotal = total;
        auto line = value.toJson();
        line["event"] = "progress";
        emitLine(line);
    }
    QJsonObject checkName(Error& error) {
        const auto name = fileName(settings(), request_["candidate"].toString());
        if (name.toUtf8().size() > maximumNameBytes) { error = {"b2_name_invalid", "The file name is too long for Backblaze B2", {}}; return {}; }
        const auto listed = client_.versions(name, error);
        if (!listed) return {{"status", "inconclusive"}};
        QJsonArray baseline;
        QString newestAction, newestId;
        for (const auto& value : *listed) {
            const auto file = value.toObject();
            if (file["action"].toString() == "start") continue;
            if (newestAction.isEmpty()) { newestAction = file["action"].toString(); newestId = file["fileId"].toString(); }
            baseline.append(file["fileId"]);
        }
        const bool present = newestAction == "upload";
        QJsonObject reference{{"schemaVersion", 1}, {"key", name}, {"baseline", baseline}};
        return {{"status", present ? "present" : "absent"}, {"objectReference", reference}, {"concurrencyToken", present ? newestId : QString()}};
    }
    // The link the receipt carries; an authorized link holds a token for exactly this name.
    QString link(const QString& name, QString& expiresAt, Error& error) {
        if (settings()["linkMode"].toString() != "authorized") return fileUrl(settings(), client_.session.downloadUrl, name);
        const int seconds = settings()["linkValidityDays"].toInt(maximumLinkDays) * 86400;
        const auto r = client_.call("b2_get_download_authorization", {{"bucketId", client_.session.bucketId}, {"fileNamePrefix", name}, {"validDurationInSeconds", seconds}}, true);
        if (!r.ok()) { error = Client::failure(r, "link_authorize"); return {}; }
        expiresAt = QDateTime::currentDateTimeUtc().addSecs(seconds).toString(Qt::ISODateWithMs);
        return fileUrl(settings(), client_.session.downloadUrl, name, r.json["authorizationToken"].toString());
    }
    StorageReceipt receipt(const QString& name, const QString& fileId, const QString& publication, const QString& sha256, qint64 bytes, Error& error) {
        StorageReceipt out;
        out.destination = destination_;
        QString expires;
        out.directUrl = link(name, expires, error);
        out.objectReference = {{"schemaVersion", 1}, {"key", name}, {"fileId", fileId}, {"bucketId", client_.session.bucketId}, {"sha256", sha256}, {"bytes", double(bytes)}};
        if (!expires.isEmpty()) out.objectReference["linkExpiresAt"] = expires;
        out.concurrencyToken = fileId;
        out.publicationId = publication;
        return out;
    }
    // The version this publication uploaded, found by its file info.
    std::optional<QJsonObject> published(const QString& name, const QString& publication, Error& error) {
        const auto listed = client_.versions(name, error);
        if (!listed) return std::nullopt;
        for (const auto& value : *listed) {
            const auto file = value.toObject();
            if (file["action"].toString() == "upload" && info(file, "putgd-publication") == publication) return file;
        }
        error = {};
        return QJsonObject{};
    }
    // After an upload: a version that is neither ours nor in the name check's baseline
    // is a concurrent upload. Our version then goes, so neither link shadows the other.
    // exposed runs from the moment our version became visible; a rollback reports how
    // long B2 served our bytes at this name (the race window S3's conditional write avoids).
    QString verifyNoOverwrite(const QString& name, const QString& fileId, const QJsonArray& baseline, const QElapsedTimer& exposed, Error& error) {
        const auto listed = client_.versions(name, error);
        if (!listed) return "unknown";
        bool ours = false, other = false;
        for (const auto& value : *listed) {
            const auto file = value.toObject();
            if (file["action"].toString() == "start") continue;
            if (file["fileId"].toString() == fileId) ours = true;
            else if (!baseline.contains(file["fileId"])) other = true;
        }
        if (!ours) { error = {"b2_upload_unconfirmed", "Backblaze B2 does not list the uploaded file version", {{"requestStage", "verify"}}}; return "unknown"; }
        if (!other) return "uploaded";
        const auto r = client_.call("b2_delete_file_version", {{"fileName", name}, {"fileId", fileId}}, false);
        if (!r.ok()) {
            error = Client::failure(r, "collision_rollback");
            error.message = "Another file with this name appeared in B2 during the upload, and put.gd could not remove its own copy. Check the bucket before retrying.";
            error.details["fileId"] = fileId;
            return "unknown";
        }
        error = {"precondition_changed", "Another file with this name appeared in Backblaze B2 during the upload; put.gd removed its own copy and will choose another name",
                 {{"requestStage", "verify"}, {"exposedMs", double(exposed.elapsed())}}};
        return "collision";
    }
    QJsonObject result(const QString& status, const StorageReceipt& r) { return {{"status", status}, {"receipt", r.toJson()}}; }

    QJsonObject upload(Error& error, bool& uncertain) {
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        const auto publication = request_["publicationId"].toString();
        const auto reference = request_["objectReference"].toObject();
        const auto name = fileName(settings(), request_["candidate"].toString());
        if (request_["condition"].toString() != "absent") { error = {"unsupported_capability", "Backblaze B2 cannot replace a file conditionally; choose rename or skip for name collisions", {}}; return {}; }
        if (reference["key"].toString() != name || !reference["baseline"].isArray()) { error = {"b2_name_check_required", "The name check for this upload is missing", {}}; return {}; }
        const auto baseline = reference["baseline"].toArray();
        progress("preparing", 0, artifact.bytes);
        const auto whole = hashFile(artifact.path);
        if (!whole) { error = cancelled.load() ? stoppedError() : Error{"artifact_invalid", "Retained artifact is unavailable", {}}; return {}; }
        if (whole->bytes != artifact.bytes || QString::fromLatin1(whole->sha256) != artifact.sha256) { error = {"artifact_changed", "The retained artifact changed since it was saved", {}}; return {}; }
        // A visible file under this name is never covered: the collision policy applies
        // before any bytes go up. Only a concurrent upload can still race us (verified below).
        {
            const auto listed = client_.versions(name, error);
            if (!listed) return {};
            for (const auto& value : *listed) {
                const auto file = value.toObject();
                if (file["action"].toString() == "start") continue;
                if (file["action"].toString() == "upload" && info(file, "putgd-publication") != publication) {
                    error = {"precondition_changed", "A file with this name already exists in Backblaze B2; put.gd uploaded nothing and will choose another name", {{"requestStage", "before_upload"}}};
                    return {{"status", "collision"}};
                }
                break;
            }
        }
        const auto part = qint64(request_["resume"].toObject()["partBytes"].toDouble());
        QString fileId;
        if (part > 0) {
            fileId = uploadLarge(artifact, name, publication, part, error, uncertain);
        } else {
            fileId = uploadSmall(artifact, name, publication, whole->sha1, error, uncertain);
        }
        if (fileId.isEmpty()) return {};
        QElapsedTimer exposed;
        exposed.start();
        progress("finalizing", artifact.bytes, artifact.bytes);
        const auto status = verifyNoOverwrite(name, fileId, baseline, exposed, error);
        if (status != "uploaded") {
            uncertain = status == "unknown";
            return {{"status", status}};
        }
        Error linkError;
        const auto r = receipt(name, fileId, publication, artifact.sha256, artifact.bytes, linkError);
        if (linkError) { error = linkError; error.details["fileId"] = fileId; uncertain = true; return {{"status", "unknown"}}; }
        return result("uploaded", r);
    }
    QList<QPair<QByteArray, QByteArray>> infoHeaders(const QString& publication, const QString& sha256) {
        return {{"X-Bz-Info-putgd-publication", infoHeader(publication)}, {"X-Bz-Info-putgd-sha256", infoHeader(sha256)}};
    }
    QString uploadSmall(const Artifact& artifact, const QString& name, const QString& publication, const QByteArray& sha1, Error& error, bool& uncertain) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (cancelled.load()) { error = stoppedError(); return {}; }
            const auto url = client_.call("b2_get_upload_url", {{"bucketId", client_.session.bucketId}}, true);
            if (!url.ok()) { error = Client::failure(url, "upload_url"); return {}; }
            OneShot body(artifact.path, 0, artifact.bytes);
            if (!body.open(QIODevice::ReadOnly)) { error = {"artifact_invalid", "Retained artifact is unavailable", {}}; return {}; }
            auto headers = infoHeaders(publication, artifact.sha256);
            headers += {{"Authorization", url.json["authorizationToken"].toString().toUtf8()}, {"X-Bz-File-Name", encodeName(name).toUtf8()},
                        {"Content-Type", artifact.mime.isEmpty() ? QByteArray("b2/x-auto") : artifact.mime.toUtf8()},
                        {"Content-Length", QByteArray::number(artifact.bytes)}, {"X-Bz-Content-Sha1", sha1}};
            client_.sentBytes = [&](qint64 sent) { progress("uploading", sent, artifact.bytes); };
            const auto r = client_.send("POST", QUrl(url.json["uploadUrl"].toString()), headers, {}, &body);
            client_.sentBytes = {};
            if (r.ok() && r.json["fileId"].isString()) return r.json["fileId"].toString();
            // A lost or failed reply may still have stored the file: look for it first.
            if (r.transport || r.status >= 500 || r.status == 408 || r.status == 429) {
                Error lookup;
                const auto found = published(name, publication, lookup);
                if (found && found->contains("fileId")) return (*found)["fileId"].toString();
                if (!found) { error = lookup; uncertain = true; return {}; }
                if (r.aborted && cancelled.load()) { error = stoppedError(); return {}; }
                error = Client::failure(r, "upload_file");
                continue;
            }
            if (r.status == 401 && (r.code() == "expired_auth_token" || r.code() == "bad_auth_token")) continue; // a new upload URL
            error = Client::failure(r, "upload_file");
            return {};
        }
        return {};
    }
    // Large files: start (journaled before any part), parts with SHA-1, finish.
    QString uploadLarge(const Artifact& artifact, const QString& name, const QString& publication, qint64 part, Error& error, bool& uncertain) {
        if (!validPlan(artifact.bytes, part)) { error = {"b2_plan_invalid", "The large-file part plan is outside Backblaze B2's limits", {}}; return {}; }
        auto journal = request_["resume"].toObject();
        QString fileId;
        QHash<int, QPair<qint64, QString>> stored;
        if (journal["uploadId"].isString()) {
            if (journal["key"].toString() != name || journal["artifactId"].toString() != artifact.id || journal["sha256"].toString() != artifact.sha256 || qint64(journal["partBytes"].toDouble()) != part) {
                cancelLarge(journal["uploadId"].toString());
            } else {
                fileId = journal["uploadId"].toString();
                // A finish whose reply was lost: the file already exists.
                const auto info = client_.call("b2_get_file_info", {{"fileId", fileId}}, true);
                if (info.ok() && info.json["action"].toString() == "upload") return fileId;
                int start = 1;
                while (true) {
                    const auto r = client_.call("b2_list_parts", {{"fileId", fileId}, {"startPartNumber", start}, {"maxPartCount", 1000}}, true);
                    if (!r.ok()) { fileId.clear(); stored.clear(); break; }
                    for (const auto& value : r.json["parts"].toArray()) {
                        const auto p = value.toObject();
                        stored.insert(p["partNumber"].toInt(), {qint64(p["contentLength"].toDouble()), p["contentSha1"].toString()});
                    }
                    if (r.json["nextPartNumber"].isNull() || !r.json["nextPartNumber"].isDouble()) break;
                    start = r.json["nextPartNumber"].toInt();
                }
            }
        }
        if (fileId.isEmpty()) {
            if (cancelled.load()) { error = stoppedError(); return {}; }
            const auto r = client_.call("b2_start_large_file", {{"bucketId", client_.session.bucketId}, {"fileName", name},
                {"contentType", artifact.mime.isEmpty() ? QString("b2/x-auto") : artifact.mime},
                {"fileInfo", QJsonObject{{"putgd-publication", publication}, {"putgd-sha256", artifact.sha256}}}}, false);
            if (!r.ok() || !r.json["fileId"].isString()) {
                // An unconfirmed start leaves nothing journaled; B2 lists unfinished large files for cleanup.
                error = Client::failure(r, "large_start");
                return {};
            }
            fileId = r.json["fileId"].toString();
            // "uploadId" is the core's name for a provider's resumable upload (the large file's ID here).
            journal = {{"schemaVersion", 1}, {"partBytes", double(part)}, {"key", name}, {"uploadId", fileId}, {"artifactId", artifact.id},
                       {"sha256", artifact.sha256}, {"bytes", double(artifact.bytes)}};
            if (auto e = checkpoint(journal)) { cancelLarge(fileId); error = e; return {}; }
        }
        const auto count = partCount(artifact.bytes, part);
        QJsonArray sha1s;
        QString uploadUrl, uploadToken;
        qint64 done = 0;
        for (qint64 n = 1; n <= count; ++n) {
            const qint64 offset = (n - 1) * part, length = qMin(part, artifact.bytes - offset);
            const auto hash = hashFile(artifact.path, offset, length, false);
            if (!hash || hash->bytes != length) return abortLarge(fileId, cancelled.load() ? stoppedError() : Error{"artifact_invalid", "Retained artifact is unavailable", {}}, error);
            const auto sha1 = QString::fromLatin1(hash->sha1);
            sha1s.append(sha1);
            if (stored.value(int(n)).first == length && stored.value(int(n)).second == sha1) { done += length; progress("uploading", done, artifact.bytes); continue; }
            bool sent = false;
            Error last;
            for (int attempt = 0; attempt < 3 && !sent; ++attempt) {
                if (cancelled.load()) return abortLarge(fileId, stoppedError(), error);
                if (uploadUrl.isEmpty()) {
                    const auto u = client_.call("b2_get_upload_part_url", {{"fileId", fileId}}, true);
                    if (!u.ok()) return abortLarge(fileId, Client::failure(u, "part_url"), error);
                    uploadUrl = u.json["uploadUrl"].toString();
                    uploadToken = u.json["authorizationToken"].toString();
                }
                OneShot body(artifact.path, offset, length);
                if (!body.open(QIODevice::ReadOnly)) return abortLarge(fileId, {"artifact_invalid", "Retained artifact is unavailable", {}}, error);
                client_.sentBytes = [&](qint64 bytes) { progress("uploading", done + bytes, artifact.bytes); };
                const auto r = client_.send("POST", QUrl(uploadUrl), {{"Authorization", uploadToken.toUtf8()}, {"X-Bz-Part-Number", QByteArray::number(n)},
                                                                      {"Content-Length", QByteArray::number(length)}, {"X-Bz-Content-Sha1", sha1.toUtf8()}}, {}, &body);
                client_.sentBytes = {};
                if (r.ok()) { sent = true; break; }
                last = Client::failure(r, "upload_part");
                last.details["partNumber"] = int(n);
                if (r.aborted && cancelled.load()) return abortLarge(fileId, stoppedError(), error);
                // Parts never publish: send again under the same number, with a new URL.
                if (r.transport || r.status >= 500 || r.status == 408 || r.status == 429 ||
                    (r.status == 401 && (r.code() == "expired_auth_token" || r.code() == "bad_auth_token"))) { uploadUrl.clear(); continue; }
                return abortLarge(fileId, last, error);
            }
            if (!sent) return abortLarge(fileId, last, error);
            done += length;
            progress("uploading", done, artifact.bytes);
        }
        if (cancelled.load()) return abortLarge(fileId, stoppedError(), error);
        const auto finish = client_.call("b2_finish_large_file", {{"fileId", fileId}, {"partSha1Array", sha1s}}, false);
        if (!finish.ok()) {
            // A lost reply, or a repeated finish (400), may still have completed the file.
            const auto info = client_.call("b2_get_file_info", {{"fileId", fileId}}, true);
            if (info.ok() && info.json["action"].toString() == "upload") { checkpoint({}); return fileId; }
            if (finish.transport || finish.status >= 500 || !info.ok()) { error = Client::failure(finish, "large_finish"); uncertain = true; return {}; }
            return abortLarge(fileId, Client::failure(finish, "large_finish"), error);
        }
        if (auto e = checkpoint({})) { error = e; error.details["fileId"] = fileId; }
        return fileId;
    }
    // Cancel the unfinished large file so its parts stop using storage.
    Error cancelLarge(const QString& fileId) {
        const auto r = client_.call("b2_cancel_large_file", {{"fileId", fileId}}, true);
        if (r.ok() || r.status == 400 || r.status == 404) return {};
        return Client::failure(r, "large_cancel");
    }
    QString abortLarge(const QString& fileId, Error cause, Error& error) {
        const bool wasCancelled = cancelled.exchange(false);
        const auto refused = cancelLarge(fileId);
        cancelled = wasCancelled;
        error = cause;
        if (refused) {
            error.details["incompleteUpload"] = true;
            error.details["abortError"] = QJsonObject{{"code", refused.code}, {"message", refused.message}};
        } else if (auto e = checkpoint({})) {
            error.details["journalError"] = e.code;
        }
        return {};
    }
    QJsonObject reconcile(Error& error) {
        const auto reference = request_["objectReference"].toObject();
        const auto name = reference["key"].isString() ? reference["key"].toString() : fileName(settings(), request_["candidate"].toString());
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        const auto found = published(name, request_["publicationId"].toString(), error);
        if (!found) return {{"status", "unknown"}};
        if (found->isEmpty()) return {{"status", "missing"}};
        if (info(*found, "putgd-sha256") != artifact.sha256 || qint64((*found)["contentLength"].toDouble()) != artifact.bytes)
            return {{"status", "foreign"}};
        const auto r = receipt(name, (*found)["fileId"].toString(), request_["publicationId"].toString(), artifact.sha256, artifact.bytes, error);
        if (error) return {{"status", "unknown"}};
        return result("own", r);
    }
    // own: the recorded version still exists with its identity; deletion is exact by ID.
    QString identity(const StorageReceipt& expected, Error& error) {
        const auto r = client_.call("b2_get_file_info", {{"fileId", expected.objectReference["fileId"].toString()}}, true);
        if (r.status == 404 || (r.status == 400 && (r.code() == "file_not_present" || r.code() == "bad_request" || r.code() == "no_such_file"))) return "missing";
        if (!r.ok()) { error = Client::failure(r, "file_info"); return "unknown"; }
        if (r.json["fileName"].toString() != expected.objectReference["key"].toString() || r.json["action"].toString() != "upload" ||
            info(r.json, "putgd-publication") != expected.publicationId || info(r.json, "putgd-sha256") != expected.objectReference["sha256"].toString() ||
            qint64(r.json["contentLength"].toDouble()) != qint64(expected.objectReference["bytes"].toDouble()))
            return "foreign";
        return "own";
    }
    QJsonObject stat(Error& error) {
        const auto expected = StorageReceipt::fromJson(request_["receipt"].toObject());
        const auto status = identity(expected, error);
        return status == "own" ? result("own", expected) : QJsonObject{{"status", status}};
    }
    QJsonObject remove(Error& error, bool& uncertain) {
        const auto expected = StorageReceipt::fromJson(request_["receipt"].toObject());
        auto progress = request_["progress"].toObject();
        const auto target = expected.toJson();
        if (progress.contains("target") && progress["target"].toObject() != target) return {{"status", "changed"}, {"progress", progress}};
        progress["schemaVersion"] = 1;
        progress["target"] = target;
        const auto phase = progress["phase"].toString();
        if (phase == "done") return {{"status", "removed"}, {"progress", progress}};
        const auto finish = [&](const QString& status) {
            progress["phase"] = "done";
            if (auto e = checkpoint(progress)) { error = e; return QJsonObject{{"status", "unknown"}, {"progress", progress}}; }
            return QJsonObject{{"status", status}, {"progress", progress}};
        };
        const auto status = identity(expected, error);
        if (status == "missing") return finish(phase == "delete_intent" ? "removed" : "missing");
        if (status == "foreign") return {{"status", "changed"}, {"progress", progress}};
        if (status != "own") return {{"status", "unknown"}, {"progress", progress}};
        if (cancelled.load()) { error = stoppedError(); return {{"status", "failed"}, {"progress", progress}}; }
        progress["phase"] = "delete_intent";
        if (auto e = checkpoint(progress)) { error = e; progress["phase"] = phase; return {{"status", "failed"}, {"progress", progress}}; }
        const auto r = client_.call("b2_delete_file_version", {{"fileName", expected.objectReference["key"].toString()}, {"fileId", expected.objectReference["fileId"].toString()}}, false);
        if (r.ok()) return finish("removed");
        Error again;
        if (identity(expected, again) == "missing") return finish("removed");
        error = Client::failure(r, "delete_version");
        uncertain = r.transport || r.status >= 500;
        return {{"status", uncertain ? "unknown" : "failed"}, {"progress", progress}};
    }
    QJsonObject abandon(Error& error) {
        const auto journal = request_["journal"].toObject();
        if (!journal["uploadId"].isString()) return {};
        if (journal["key"].toString().isEmpty() || !journal["key"].toString().startsWith(fileName(settings(), {})))
            { error = {"b2_journal_invalid", "The saved large-file upload does not belong to this destination", {}}; return {}; }
        error = cancelLarge(journal["uploadId"].toString());
        return {};
    }
    // Synthetic text through the production upload path, fetched through the same kind
    // of link put.gd hands out, then removed by its exact version ID.
    QJsonObject testDestination(Error& error) {
        QStringList missing;
        QStringList needed{"listFiles", "readFiles", "writeFiles", "deleteFiles"};
        if (settings()["linkMode"].toString() == "authorized") needed << "shareFiles";
        for (const auto& capability : needed) if (!client_.session.capabilities.contains(capability)) missing << capability;
        if (!missing.isEmpty()) { error = {"b2_permission_denied", "The application key lacks: " + missing.join(", ") + ". Create a key with those capabilities for this bucket.", {{"requestStage", "capabilities"}}}; return {}; }
        QJsonObject out{{"bucketType", client_.session.bucketType}, {"linkMode", settings()["linkMode"]}};
        if (settings()["linkMode"].toString() == "public" && client_.session.bucketType == "allPrivate") {
            error = {"b2_bucket_private", "The bucket is private, so public links would not open. Make the bucket public in B2 (it needs payment history on the account), or choose authorized links.", {{"requestStage", "bucket"}}};
            return out;
        }
        const auto synthetic = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto bytes = QByteArray("put.gd connection test " + synthetic.toUtf8() + "\n");
        QTemporaryDir dir;
        const auto path = dir.filePath("synthetic.txt");
        QFile file(path);
        if (!dir.isValid() || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { error = {"synthetic_artifact_failed", "Could not create the private synthetic file", {}}; return out; }
        file.close();
        Artifact artifact;
        artifact.id = "synthetic-" + synthetic;
        artifact.path = path;
        artifact.name = "putgd-test-" + synthetic + ".txt";
        artifact.mime = "text/plain";
        artifact.bytes = bytes.size();
        artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        request_["candidate"] = artifact.name;
        const auto checked = checkName(error);
        if (error || checked["status"] != "absent") { if (!error) error = {"synthetic_collision", "Synthetic test file name is unavailable", {}}; return out; }
        request_["artifact"] = artifact.toJson();
        request_["publicationId"] = synthetic;
        request_["condition"] = "absent";
        request_["objectReference"] = checked["objectReference"];
        request_["resume"] = QJsonObject{};
        bool uncertain = false;
        const auto uploaded = upload(error, uncertain);
        out["uploadStatus"] = uploaded["status"].toString(error ? "failed" : "unknown");
        if (error || uploaded["status"] != "uploaded") return out;
        const auto receipt = StorageReceipt::fromJson(uploaded["receipt"].toObject());
        out["receipt"] = receipt.toJson();
        const auto fetched = client_.send("GET", QUrl(receipt.directUrl, QUrl::StrictMode), {});
        out["publicReachable"] = fetched.status == 200 && fetched.body == bytes;
        QJsonObject progress;
        request_["receipt"] = receipt.toJson();
        request_["progress"] = progress;
        Error cleanup;
        bool cleanupUncertain = false;
        const auto removed = remove(cleanup, cleanupUncertain);
        out["cleanupStatus"] = removed["status"];
        out["cleanupProgress"] = removed["progress"];
        // Large files need writeFiles too; prove start and cancel without sending a part.
        const auto start = client_.call("b2_start_large_file", {{"bucketId", client_.session.bucketId}, {"fileName", fileName(settings(), "putgd-test-large-" + synthetic)},
                                                               {"contentType", "text/plain"}}, false);
        QJsonObject large{{"start", start.ok() ? "ok" : "failed"}};
        if (start.ok()) large["cancel"] = cancelLarge(start.json["fileId"].toString()) ? "failed" : "cancelled";
        out["largeFile"] = large;
        if (cleanup) error = cleanup;
        else if (!out["publicReachable"].toBool()) {
            error = {"public_url_unreachable", settings()["linkMode"].toString() == "authorized"
                ? "Uploads work, but the authorized link did not open. Check that the key has shareFiles and the link domain."
                : "Uploads work, but the public link did not open. Make the bucket public in B2, or check the link domain.", {{"httpStatus", fetched.status}}};
        } else if (!start.ok()) error = Client::failure(start, "large_start");
        return out;
    }
};
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version-json") == 0) { std::printf("%s\n", putgd::buildManifestJson()); return 0; }
    if (argc != 2 || std::strcmp(argv[1], "--stdio") != 0) return 2;
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("putgd-b2-worker");
    putgd::useHostCaBundle();
    bool eof = false;
    auto line = putgd::b2::readLine(eof);
    auto request = QJsonDocument::fromJson(line).object();
    line.fill('\0');
    if (request["schemaVersion"].toInt() != 1 || !request["method"].isString()) {
        putgd::b2::emitLine({{"ok", false}, {"error", QJsonObject{{"code", "b2_protocol"}, {"message", "Invalid worker request"}}}, {"uncertain", false}});
        return 0;
    }
    std::thread control(putgd::b2::readControl);
    control.detach();
    putgd::Error error;
    bool uncertain = false;
    QJsonObject result;
    {
        putgd::b2::Worker worker(request);
        request = {};
        result = worker.run(error, uncertain);
    }
    if (error)
        putgd::b2::emitLine({{"ok", false}, {"error", QJsonObject{{"code", error.code}, {"message", error.message}, {"details", error.details}}},
                             {"uncertain", uncertain}, {"result", result}});
    else putgd::b2::emitLine({{"ok", true}, {"result", result}});
    std::_Exit(0);
}
