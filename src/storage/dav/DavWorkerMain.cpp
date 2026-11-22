// putgd-webdav-worker: one Nextcloud or WebDAV request per process (#1872).
//
// The resident writes one request line on stdin: {schemaVersion:1, method, destination,
// credentials:{user, password}, ...}. The password (a Nextcloud app password or a WebDAV
// password) arrives only there, never in argv or the environment, and is never echoed.
// Later stdin lines are {"ack":true} or {"ack":false,"error":{...}} for a checkpoint, and
// {"cancel":true}. A closed stdin cancels. The worker writes newline JSON on stdout:
// {"event":"progress",...}, {"event":"checkpoint","journal":{...}} (it then waits for the
// ack before relying on what it journaled), {"event":"signIn","url":...} once during a
// Nextcloud Login Flow v2, and one final {"ok":true,"result":{...}} or
// {"ok":false,"error":{...},"uncertain":bool}. All HTTP happens here (Qt Network); the
// resident links none of it.
#include "DavCommon.h"
#include "core/BuildInfo.h"
#include "core/ByteSize.h"
#include "core/CaBundle.h"
#include "core/StorageProvider.h"
#include "storage/http/HttpCommon.h"
#include "storage/worker/WorkerStdio.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslError>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QXmlStreamReader>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
namespace putgd::dav {
namespace {
constexpr qint64 maximumLine = 256 * 1024;
constexpr qsizetype maximumReplyBytes = 4 * 1024 * 1024;
worker::Stdio stdio{maximumLine, "webdav", "upload"};
auto& cancelled = stdio.cancelled;
Error checkpoint(const QJsonObject& journal) { return stdio.checkpoint(journal); }
Error stoppedError() { return {"cancelled", "Cancelled; a request already sent may still have taken effect", {}}; }
using Headers = QList<QPair<QByteArray, QByteArray>>;

// A request body read exactly once (a file range or bytes). Qt Network silently sends a
// request again when the server closes the connection before replying; a body it cannot
// rewind turns that into an error, so no upload is ever repeated behind put.gd's back.
class OneShot final : public QIODevice {
public:
    explicit OneShot(QByteArray bytes) : bytes_(std::move(bytes)), length_(bytes_.size()) {}
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
    // Never zero: newer Qt drops an upload device of size zero, and with it the guard
    // against a silent resend. An empty body is still sent with Content-Length: 0.
    qint64 size() const override { return qMax<qint64>(length_, 1); }
    qint64 length() const { return length_; }
    bool consumed() const { return read_ >= length_; }
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

struct Reply {
    int status = 0;            // HTTP status; 0 when no reply arrived
    QByteArray body;
    Headers headers;
    bool reached = false;      // the whole request was handed to the transport
    bool stopped = false, deadline = false;
    QString detail;            // bounded transport description, never request data
    QList<QSslError> tls;      // certificate problems the handshake met
    QString header(const char* name) const {
        for (const auto& [key, value] : headers) if (key.compare(name, Qt::CaseInsensitive) == 0) return QString::fromUtf8(value);
        return {};
    }
    bool ok() const { return status >= 200 && status < 300; }
    QJsonObject ocs() const { return QJsonDocument::fromJson(body).object()["ocs"].toObject(); }
    QJsonValue data() const { return ocs()["data"]; }
    QString ocsMessage() const { return http::boundedMessage(ocs()["meta"].toObject()["message"].toString(), 200); }
};
QString transportReason(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ConnectionRefusedError: return QStringLiteral("connection refused");
    case QNetworkReply::RemoteHostClosedError: return QStringLiteral("the server closed the connection");
    case QNetworkReply::HostNotFoundError: return QStringLiteral("host not found");
    case QNetworkReply::TimeoutError: return QStringLiteral("timed out");
    case QNetworkReply::SslHandshakeFailedError: return QStringLiteral("the secure connection failed");
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError: return QStringLiteral("the network is unavailable");
    case QNetworkReply::ContentReSendError: return QStringLiteral("the connection closed before the reply");
    case QNetworkReply::OperationCanceledError: return QStringLiteral("stopped");
    default: return QStringLiteral("network error %1").arg(int(error));
    }
}
class NoCookies final : public QNetworkCookieJar {
public:
    bool setCookiesFromUrl(const QList<QNetworkCookie>&, const QUrl&) override { return false; }
};

// The command that adds a certificate authority to this distribution's trust store.
QString trustCommand() {
    QFile release("/etc/os-release");
    QString ids;
    if (release.open(QIODevice::ReadOnly))
        for (const auto& line : QString::fromUtf8(release.read(64 * 1024)).split('\n'))
            if (line.startsWith("ID=") || line.startsWith("ID_LIKE=")) ids += ' ' + line.section('=', 1).remove('"').toLower();
    if (ids.contains("fedora") || ids.contains("rhel") || ids.contains("centos"))
        return QStringLiteral("sudo cp server-ca.crt /etc/pki/ca-trust/source/anchors/ && sudo update-ca-trust");
    if (ids.contains("suse")) return QStringLiteral("sudo cp server-ca.crt /etc/pki/trust/anchors/ && sudo update-ca-certificates");
    if (ids.contains("arch")) return QStringLiteral("sudo trust anchor --store server-ca.crt");
    return QStringLiteral("sudo cp server-ca.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates");
}
QString certificateName(const QStringList& names) { return names.isEmpty() ? QStringLiteral("an unnamed authority") : names.join(", "); }
// Why the secure connection to `url` failed, in words that say what to do.
Error tlsError(const Reply& r, const QUrl& url) {
    QSslCertificate certificate;
    for (const auto& error : r.tls) if (!error.certificate().isNull()) { certificate = error.certificate(); break; }
    const auto fingerprint = certificate.isNull() ? QString() : QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex(':')).toUpper();
    const auto suffix = fingerprint.isEmpty() ? QString() : " Certificate SHA-256: " + fingerprint + ".";
    QJsonObject details{{"host", url.host()}};
    if (!fingerprint.isEmpty()) details["sha256"] = fingerprint;
    QStringList kinds;
    for (const auto& error : r.tls) kinds << QString::number(int(error.error()));
    details["tlsErrors"] = kinds.join(',');
    for (const auto& error : r.tls) {
        switch (error.error()) {
        case QSslError::CertificateExpired:
            return {"webdav_tls_failed", "The certificate of " + url.host() + " expired on " + certificate.expiryDate().toUTC().toString("yyyy-MM-dd") +
                                             ". Renew it on the server, then test again." + suffix, details};
        case QSslError::CertificateNotYetValid:
            return {"webdav_tls_failed", "The certificate of " + url.host() + " is not valid until " + certificate.effectiveDate().toUTC().toString("yyyy-MM-dd") +
                                             ". Check the clock on this computer and on the server." + suffix, details};
        case QSslError::HostNameMismatch: {
            auto names = certificate.subjectAlternativeNames().values(QSsl::DnsEntry);
            if (names.isEmpty()) names = certificate.subjectInfo(QSslCertificate::CommonName);
            return {"webdav_tls_failed", "The certificate of " + url.host() + " is for " + certificateName(names) + ", not " + url.host() +
                                             ". Use the address the certificate names, or fix the certificate." + suffix, details};
        }
        default: break;
        }
    }
    if (!r.tls.isEmpty()) {
        auto issuer = certificate.issuerInfo(QSslCertificate::Organization);
        if (issuer.isEmpty()) issuer = certificate.issuerInfo(QSslCertificate::CommonName);
        return {"webdav_tls_failed", "This computer doesn't trust the certificate of " + url.host() + ", issued by " + certificateName(issuer) +
                                         ". If that is your own certificate authority, add it to the system trust store (" + trustCommand() + "), then test again." + suffix, details};
    }
    return {"webdav_tls_failed", "The secure connection to " + url.host() + " failed. Check that the server speaks HTTPS on this address and port.", details};
}

class Client {
public:
    Client(QString user, QString password, bool allowInsecure) : user_(std::move(user)), password_(std::move(password)), allowInsecure_(allowInsecure) {
        nam_.setCookieJar(new NoCookies);
    }
    ~Client() { password_.fill(QChar('\0')); }
    bool anonymous() const { return user_.isEmpty(); }
    // One request, no redirect followed. A request carrying file data (`body` with
    // `data`) has no whole-request deadline, only the stall limit; every other one keeps
    // the deadline. `sent` hears the body bytes the transport has read.
    // `sink`, when set, takes the reply body as it arrives instead of keeping it (a download
    // that is hashed); such a GET counts as file data, with only the stall limit.
    Reply send(const QByteArray& verb, const QUrl& url, Headers headers, OneShot* body = nullptr, bool data = false,
               const std::function<void(qint64)>& sent = {}, int deadlineMs = requestTimeoutMs, const std::function<void(const QByteArray&)>& sink = {}) {
        Reply out;
        if (!http::transportAllowed(url, allowInsecure_)) { out.detail = QStringLiteral("plain HTTP is not allowed for this destination"); return out; }
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setTransferTimeout(stallTimeoutMs);
        request.setRawHeader("User-Agent", QByteArray("put.gd/") + buildVersion());
        request.setRawHeader("Accept-Encoding", "identity");
        if (!user_.isEmpty()) request.setRawHeader("Authorization", "Basic " + (user_ + ':' + password_).toUtf8().toBase64());
        for (const auto& [name, value] : headers) request.setRawHeader(name, value);
        std::unique_ptr<OneShot> empty;
        if (!body && verb != "GET" && verb != "HEAD") {
            empty = std::make_unique<OneShot>(QByteArray());
            body = empty.get();
        }
        QNetworkReply* reply = nullptr;
        if (body) {
            body->open(QIODevice::ReadOnly);
            request.setHeader(QNetworkRequest::ContentLengthHeader, body->length());
            request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
            reply = nam_.sendCustomRequest(request, verb, body);
        } else reply = nam_.sendCustomRequest(request, verb);
        QEventLoop loop;
        bool connected = false;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::encrypted, &loop, [&] { connected = true; });
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] { connected = true; });
        QObject::connect(reply, &QNetworkReply::requestSent, &loop, [&] { connected = true; });
        QObject::connect(reply, &QNetworkReply::sslErrors, &loop, [&](const QList<QSslError>& errors) { out.tls = errors; });
        QObject::connect(reply, &QNetworkReply::uploadProgress, &loop, [&](qint64 bytes, qint64) { if (bytes > 0) connected = true; if (sent) sent(bytes); });
        const auto take = [&](const QByteArray& chunk) {
            if (sink && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) sink(chunk);
            else if (out.body.size() < maximumReplyBytes) out.body.append(chunk.left(maximumReplyBytes - out.body.size()));
        };
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] { take(reply->readAll()); });
        QTimer poll;
        poll.setInterval(50);
        QElapsedTimer clock;
        clock.start();
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (out.stopped || out.deadline) return;
            if (cancelled.load()) { out.stopped = true; reply->abort(); }
            else if ((!data && clock.elapsed() > deadlineMs) || (!connected && clock.elapsed() > connectTimeoutMs)) { out.deadline = true; reply->abort(); }
        });
        poll.start();
        if (!reply->isFinished()) loop.exec();
        poll.stop();
        out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        take(reply->readAll());
        out.headers = reply->rawHeaderPairs();
        out.reached = body ? body->consumed() && connected : connected;
        if (out.stopped || out.deadline) out.status = 0;
        if (out.status == 0) {
            const auto failure = reply->error();
            const bool timedOut = !out.stopped && !out.deadline && (failure == QNetworkReply::OperationCanceledError || failure == QNetworkReply::TimeoutError);
            out.detail = out.stopped ? QStringLiteral("cancelled") : out.deadline ? (connected ? QStringLiteral("no reply in time") : QStringLiteral("could not connect in time"))
                       : timedOut ? QStringLiteral("the transfer stalled") : transportReason(failure);
            // A connection that ended under a request whose bytes were all sent may have done its work.
            if (failure == QNetworkReply::RemoteHostClosedError || failure == QNetworkReply::ContentReSendError) out.reached = out.reached || connected;
        }
        reply->deleteLater();
        return out;
    }
    Reply ocs(const QByteArray& verb, const QString& server, const QString& path, const QByteArray& form = {}) {
        QUrl url(server + path);
        QUrlQuery query(url);
        query.addQueryItem("format", "json");
        url.setQuery(query);
        Headers headers{{"OCS-APIRequest", "true"}, {"Accept", "application/json"}};
        std::unique_ptr<OneShot> body;
        if (!form.isEmpty()) {
            headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/x-www-form-urlencoded")));
            body = std::make_unique<OneShot>(form);
        }
        return send(verb, url, headers, body.get());
    }
    // A GET that follows up to three redirects on the same host (http to https allowed).
    Reply fetch(QUrl url) {
        Reply out;
        for (int hop = 0; hop < 4; ++hop) {
            out = send("GET", url, {});
            if (!QList<int>{301, 302, 303, 307, 308}.contains(out.status)) return out;
            const auto target = url.resolved(QUrl(out.header("location")));
            if (!target.isValid() || target.host().compare(url.host(), Qt::CaseInsensitive) != 0 || (url.scheme() == "https" && target.scheme() != "https")) return out;
            url = target;
        }
        return out;
    }
private:
    QNetworkAccessManager nam_;
    QString user_, password_;
    bool allowInsecure_;
};

struct Entry { bool found = false, collection = false; QString etag, fileId; qint64 bytes = -1; };
QByteArray propfindBody() {
    return "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"><d:prop>"
           "<d:getetag/><d:getcontentlength/><d:resourcetype/><oc:fileid/></d:prop></d:propfind>";
}
// The first <d:response>'s properties that the server reported with 200.
Entry parsePropfind(const QByteArray& xml) {
    Entry out;
    QXmlStreamReader reader(xml);
    int responses = 0;
    QString current;
    bool inPropstat = false, collection = false;
    Entry pending;
    QString status;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name();
            if (name == QLatin1String("response")) { ++responses; if (responses > 1) break; }
            else if (name == QLatin1String("propstat")) { inPropstat = true; pending = {}; collection = false; status.clear(); }
            else if (name == QLatin1String("collection") && inPropstat) collection = true;
            else if (inPropstat && (name == QLatin1String("getetag") || name == QLatin1String("getcontentlength") || name == QLatin1String("fileid") || name == QLatin1String("status"))) {
                const auto text = reader.readElementText().trimmed();
                if (name == QLatin1String("getetag")) pending.etag = text;
                else if (name == QLatin1String("getcontentlength")) pending.bytes = text.toLongLong();
                else if (name == QLatin1String("fileid")) pending.fileId = text;
                else status = text;
            }
        } else if (reader.isEndElement() && reader.name() == QLatin1String("propstat")) {
            inPropstat = false;
            if (status.contains(" 200")) {
                out.found = true;
                if (!pending.etag.isEmpty()) out.etag = pending.etag;
                if (pending.bytes >= 0) out.bytes = pending.bytes;
                if (!pending.fileId.isEmpty()) out.fileId = pending.fileId;
                out.collection = out.collection || collection;
            }
        }
    }
    return out;
}
// A strong ETag as sent in If-Match: quoted, without a weak prefix.
QByteArray matchTag(const QString& etag) {
    auto tag = etag.trimmed();
    if (tag.startsWith("W/")) tag = tag.mid(2);
    if (!tag.startsWith('"')) tag = '"' + tag + '"';
    return tag.toUtf8();
}
QString isoDate(const QString& nextcloud) {
    // Nextcloud keeps the date a link stops working ("2026-12-01 00:00:00"); the link works
    // through the end of the day before. Read as UTC: the server's own zone is not reported.
    const auto date = QDate::fromString(nextcloud.left(10), "yyyy-MM-dd");
    if (!date.isValid()) return {};
    return date.startOfDay(QTimeZone::utc()).toString(Qt::ISODateWithMs);
}

class Worker {
public:
    explicit Worker(const QJsonObject& request)
        : request_(request), destination_(Destination::fromJson(request["destination"].toObject())), provider_(destination_.providerId),
          client_(request["credentials"].toObject()["user"].toString(), request["credentials"].toObject()["password"].toString(),
                  destination_.settings["allowInsecureHttp"].toBool()),
          anonymous_({}, {}, destination_.settings["allowInsecureHttp"].toBool()) {
        request_.remove("credentials");
    }
    QJsonObject run(Error& error, bool& uncertain) {
        const auto method = request_["method"].toString();
        if (!isDav(provider_)) { error = {"webdav_protocol", "Not a Nextcloud or WebDAV destination", {}}; return {}; }
        if (method == "signIn") return signIn(error);
        if (auto e = validateSettings(provider_, settings())) { error = e; return {}; }
        if (provider_ == nextcloudId && settings()["server"].toString().isEmpty()) { error = {"webdav_settings_invalid", "Enter the Nextcloud server address first", {}}; return {}; }
        if (provider_ == webdavId && settings()["address"].toString().isEmpty()) { error = {"webdav_settings_invalid", "Enter the WebDAV folder address first", {}}; return {}; }
        if (method == "signOut") return signOut(error);
        if (client_.anonymous()) { error = {"credential_setup_required", provider_ == nextcloudId ? "Sign in to Nextcloud first" : "Enter the WebDAV user name and password first", {}}; return {}; }
        if (method == "inspectAccount") return inspectAccount(error);
        if (method == "createFolder") return createFolder(error);
        if (method == "checkName") return checkName(error);
        if (method == "upload") return upload(error, uncertain);
        if (method == "stat") return stat(error);
        if (method == "reconcile") return reconcile(error);
        if (method == "remove") return remove(error, uncertain);
        if (method == "share") return share(error, uncertain);
        if (method == "abandon") return abandon(error);
        if (method == "testDestination") return testDestination(error);
        error = {"webdav_protocol", "Unsupported WebDAV worker method", {}};
        return {};
    }
private:
    QJsonObject request_;
    Destination destination_;
    QString provider_;
    Client client_, anonymous_;
    QString userId_;
    QJsonObject account_, capabilities_;
    bool synthetic_ = false;
    QJsonObject settings() const { return destination_.settings; }
    bool nextcloud() const { return provider_ == nextcloudId; }
    QString server() const { return settings()["server"].toString(); }
    QString service() const { return nextcloud() ? QStringLiteral("Nextcloud") : QStringLiteral("The WebDAV server"); }
    QUrl folder() const { return folderUrl(settings(), provider_, userId_); }
    QUrl file(const QString& name) const { return fileUrl(settings(), provider_, userId_, name); }
    // The path of a file below the user's files, as the OCS Share API names it.
    QString sharePath(const QString& name) const {
        const auto folder = settings()["folder"].toString();
        return "/" + (folder.isEmpty() ? name : folder + "/" + name);
    }

    // What a failed request means to the user. `what` names the request's subject.
    Error failure(const Reply& r, const QUrl& url, const QString& stage) {
        Error e;
        if (r.status == 0) {
            if (!r.tls.isEmpty() || r.detail == QLatin1String("the secure connection failed")) e = tlsError(r, url);
            else if (r.stopped) e = stoppedError();
            else if (!http::transportAllowed(url, settings()["allowInsecureHttp"].toBool()))
                e = {"webdav_plain_http_refused", "put.gd doesn't send to a plain http:// address on another computer unless Allow plain HTTP for this destination is on.", {}};
            else e = {"webdav_unreachable", "Couldn't reach " + url.host() + ": " + r.detail + ".", {}};
        } else if (r.status == 401) {
            e = nextcloud() ? Error{"webdav_auth_failed", "Nextcloud refused put.gd's app password; it may have been revoked. Sign in again.", {}}
                            : Error{"webdav_auth_failed", "The WebDAV server refused the user name or password. Check them and store them again.", {}};
        } else if (r.status == 403) e = {"webdav_forbidden", service() + " refused access (HTTP 403). Check that this account may write to the folder.", {}};
        else if (r.status == 507) e = quotaError();
        else if (r.status == 413) e = {"webdav_too_large", service() + " refused a file this large (HTTP 413). Its upload size limit is lower than this file.", {}};
        else if (r.status == 429) e = {"webdav_rate_limited", service() + " is limiting how often put.gd may do this (HTTP 429). Try again in a few minutes.", {}};
        else if (r.status == 423) e = {"webdav_locked", service() + " says the file is locked by another app (HTTP 423). Try again later.", {}};
        else if (r.status >= 500) e = {"webdav_server_error", QString("%1 had an internal error (HTTP %2). Try again later.").arg(service()).arg(r.status), {}};
        else e = {"webdav_failed", QString("%1 answered HTTP %2.").arg(service()).arg(r.status), {}};
        e.details["requestStage"] = stage;
        if (r.status) e.details["httpStatus"] = r.status;
        return e;
    }
    Error quotaError() {
        Error e{"webdav_quota", service() + " is out of space for this account. Free some space or ask the administrator for more, then Retry.", {}};
        if (nextcloud() && !userId_.isEmpty()) {
            // The account's own record carries its quota; /cloud/user may not.
            auto quota = account_["quota"].toObject();
            if (quota.isEmpty()) {
                const auto r = client_.ocs("GET", server(), "/ocs/v2.php/cloud/users/" + QString::fromLatin1(QUrl::toPercentEncoding(userId_)));
                if (r.ok()) quota = r.data().toObject()["quota"].toObject();
            }
            const auto free = qint64(quota["free"].toDouble(-1)), total = qint64(quota["quota"].toDouble(quota["total"].toDouble(-1)));
            const auto used = qint64(quota["used"].toDouble(-1));
            const auto size = [](qint64 bytes) { return ByteSize::localized(bytes, 1, QLocale::c()); };
            if (free > 0)
                e.message = "Nextcloud refused the file: this account has " + size(free) + " left" + (total > 0 ? " of its " + size(total) + " quota" : QString()) +
                            ". Free some space or ask the administrator for more, then Retry.";
            else if (total > 0 && used >= 0)
                e.message = "Nextcloud refused the file: this account has used " + size(used) + " of its " + size(total) + " quota, so no space is left. Free some space or ask the administrator for more, then Retry.";
        }
        return e;
    }
    bool uncertainAfter(const Reply& r) const { return (r.status == 0 && r.reached) || (r.status >= 500 && r.reached); }

    // Nextcloud: the account behind the app password (its DAV user is the OCS id, not the
    // login name) and what the administrator allows for links.
    Error resolveAccount() {
        if (!nextcloud() || !userId_.isEmpty()) return {};
        const auto r = client_.ocs("GET", server(), "/ocs/v2.php/cloud/user");
        if (r.status == 404 || (r.ok() && r.ocs().isEmpty())) return notNextcloud(r);
        if (!r.ok()) return failure(r, QUrl(server()), "account");
        account_ = r.data().toObject();
        userId_ = account_["id"].toString();
        if (userId_.isEmpty()) return notNextcloud(r);
        return {};
    }
    Error notNextcloud(const Reply& r) {
        return {"nextcloud_not_found", server() + " doesn't answer like a Nextcloud server. Check the address: it is the one you open Nextcloud at in a browser.", {{"httpStatus", r.status}}};
    }
    Error loadCapabilities() {
        if (!capabilities_.isEmpty()) return {};
        const auto r = client_.ocs("GET", server(), "/ocs/v2.php/cloud/capabilities");
        if (!r.ok()) return failure(r, QUrl(server()), "capabilities");
        capabilities_ = r.data().toObject();
        return {};
    }
    QJsonObject sharing() const { return capabilities_["capabilities"].toObject()["files_sharing"].toObject(); }
    bool linksEnabled() const {
        const auto s = sharing();
        return s["api_enabled"].toBool(true) && s["public"].toObject()["enabled"].toBool(false);
    }
    bool passwordEnforced() const {
        const auto password = sharing()["public"].toObject()["password"].toObject();
        return password["enforced"].toBool() || password["enforced_for"].toObject()["read_only"].toBool();
    }
    // Days a link may last when the administrator enforces an expiry; zero when not enforced.
    int enforcedDays() const {
        const auto expiry = sharing()["public"].toObject()["expire_date"].toObject();
        return expiry["enforced"].toBool() ? qMax(1, expiry["days"].toInt(7)) : 0;
    }
    Error sharingDisabled() const {
        return {"nextcloud_sharing_disabled", "Public links are turned off on this Nextcloud server, so put.gd can't make a link. Ask the administrator to allow sharing with public links.", {}};
    }
    Error passwordRequired() const {
        return {"nextcloud_share_password_required", "This Nextcloud server requires a password on every public link, which put.gd doesn't set. Ask the administrator to stop enforcing link passwords, or use another destination.", {}};
    }
    QJsonObject accountJson() const {
        QJsonObject out{{"userId", userId_}, {"accountName", account_["display-name"].toString(account_["displayname"].toString(userId_))}};
        if (!capabilities_.isEmpty()) {
            out["sharing"] = linksEnabled();
            out["sharePasswordEnforced"] = passwordEnforced();
            out["shareExpiryDays"] = enforcedDays();
            out["version"] = capabilities_["version"].toObject()["string"].toString();
        }
        const auto quota = account_["quota"].toObject();
        if (quota["free"].toDouble(-1) >= 0) out["freeBytes"] = quota["free"].toDouble();
        return out;
    }

    Reply propfind(const QUrl& url) {
        OneShot body(propfindBody());
        return client_.send("PROPFIND", url, {{"Depth", "0"}, {"Content-Type", "application/xml; charset=utf-8"}}, &body);
    }
    // The Nextcloud folder, created segment by segment when missing: it is in the user's own files.
    Error ensureFolder(bool* created = nullptr) {
        if (created) *created = false;
        const auto probe = propfind(folder());
        if (probe.status == 207) return {};
        if (probe.status != 404 && probe.status != 409) return failure(probe, folder(), "folder");
        if (!nextcloud()) return folderMissing();
        auto url = folderUrl(QJsonObject{{"server", server()}, {"folder", ""}}, provider_, userId_).toString(QUrl::FullyEncoded);
        for (const auto& segment : settings()["folder"].toString().split('/', Qt::SkipEmptyParts)) {
            url += QString::fromLatin1(QUrl::toPercentEncoding(segment)) + "/";
            const auto made = client_.send("MKCOL", QUrl(url, QUrl::StrictMode), {});
            if (made.status == 201 || made.status == 405) continue; // 405: it exists
            return failure(made, QUrl(url), "folder");
        }
        if (created) *created = true;
        return {};
    }
    Error folderMissing() const {
        return {"webdav_folder_missing", "The folder " + settings()["address"].toString() + " doesn't exist on the WebDAV server. Create it, or choose Create folder.", {{"createFolder", true}}};
    }
    QJsonObject createFolder(Error& error) {
        if (nextcloud()) {
            if ((error = resolveAccount())) return {};
            bool created = false;
            error = ensureFolder(&created);
            return {{"created", created}};
        }
        const auto probe = propfind(folder());
        if (probe.status == 207) return {{"created", false}};
        const auto made = client_.send("MKCOL", folder(), {});
        if (made.status == 201) return {{"created", true}};
        if (made.status == 409) { error = {"webdav_folder_missing", "The WebDAV server can't create this folder because the folder above it doesn't exist. Create the folders above it first.", {}}; return {}; }
        error = failure(made, folder(), "create_folder");
        return {};
    }

    QJsonObject inspectAccount(Error& error) {
        if (!nextcloud()) {
            const auto probe = propfind(folder());
            if (probe.status == 207) return {{"folder", "present"}};
            error = probe.status == 404 || probe.status == 409 ? folderMissing() : failure(probe, folder(), "folder");
            return {};
        }
        if ((error = resolveAccount()) || (error = loadCapabilities())) return {};
        return accountJson();
    }

    QJsonObject checkName(Error& error) {
        const auto name = request_["candidate"].toString();
        if (name.isEmpty() || name.toUtf8().size() > maximumNameBytes || name.contains('/')) { error = {"webdav_name_invalid", "The file name is empty, too long or has a /", {}}; return {{"status", "inconclusive"}}; }
        if ((error = resolveAccount())) return {{"status", "inconclusive"}};
        const auto r = propfind(file(name));
        if (r.status == 404 || r.status == 409) return {{"status", "absent"}, {"objectReference", QJsonObject{{"schemaVersion", 1}, {"name", name}}}};
        if (r.status == 207) return {{"status", "present"}, {"concurrencyToken", parsePropfind(r.body).etag}};
        error = failure(r, file(name), "check_name");
        return {{"status", "inconclusive"}};
    }

    QJsonObject receiptJson(const QString& name, const QString& etag, const QString& fileId, const Artifact& artifact) {
        StorageReceipt receipt;
        receipt.destination = destination_;
        QJsonObject reference{{"schemaVersion", 1}, {"name", name}, {"bytes", double(artifact.bytes)}, {"sha256", artifact.sha256}};
        if (nextcloud()) reference["path"] = sharePath(name);
        if (!fileId.isEmpty()) reference["fileId"] = fileId;
        if (!etag.isEmpty()) reference["etag"] = etag;
        receipt.objectReference = reference;
        receipt.concurrencyToken = etag;
        receipt.publicationId = request_["publicationId"].toString();
        if (!nextcloud()) receipt.directUrl = webdavLink(settings(), name);
        return receipt.toJson();
    }
    Entry stored(const QUrl& url) {
        const auto r = propfind(url);
        return r.status == 207 ? parsePropfind(r.body) : Entry{};
    }
    // The receipt of a file just stored, with the ETag and file ID that a later PROPFIND reports:
    // the write's own reply can differ (Apache's PUT ETag, Nextcloud's padded OC-FileId), and
    // deletion compares against PROPFIND. The reply's ETag stands in only when PROPFIND has none.
    QJsonObject storedReceipt(const QUrl& target, const Reply& write, const QString& name, const Artifact& artifact) {
        const auto entry = stored(target);
        auto etag = entry.etag;
        if (etag.isEmpty()) etag = write.header("etag").isEmpty() ? write.header("oc-etag") : write.header("etag");
        return receiptJson(name, etag, entry.fileId, artifact);
    }

    QJsonObject upload(Error& error, bool& uncertain) {
        uncertain = false;
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        const auto name = request_["candidate"].toString();
        if (request_["condition"].toString() != "absent") { error = {"unsupported_capability", "put.gd never uploads over an existing file here; choose rename or skip for name collisions", {}}; return {{"status", "failed"}}; }
        if (name.isEmpty() || name.toUtf8().size() > maximumNameBytes || name.contains('/')) { error = {"webdav_name_invalid", "The file name is empty, too long or has a /", {}}; return {{"status", "failed"}}; }
        progress("preparing", 0, artifact.bytes);
        if ((error = resolveAccount())) return {{"status", "failed"}};
        if (nextcloud()) {
            // The only refusal before the upload: links are off, so no link could follow.
            if ((error = loadCapabilities())) return {{"status", "failed"}};
            if (!linksEnabled()) { error = sharingDisabled(); return {{"status", "failed"}}; }
        }
        if ((error = ensureFolder())) return {{"status", "failed"}};
        // A journal left by an earlier chunked attempt: its chunks are removed, never reused.
        if (const auto old = request_["resume"].toObject(); old["uploadId"].isString()) dropChunks(old["uploadId"].toString());
        const auto threshold = qint64(request_["chunkThresholdBytes"].toDouble(double(chunkThresholdBytes)));
        if (nextcloud() && artifact.bytes > threshold) return chunked(artifact, name, error, uncertain);
        const auto target = file(name);
        OneShot body(artifact.path, 0, artifact.bytes);
        const auto total = artifact.bytes;
        const auto r = client_.send("PUT", target, {{"If-None-Match", "*"}, {"Content-Type", artifact.mime.isEmpty() ? QByteArray("application/octet-stream") : artifact.mime.toUtf8()}},
                                    &body, true, [total](qint64 sent) { stdio.progress("uploading", sent, total); });
        if (r.status == 412) return {{"status", "collision"}};
        if (!r.ok()) {
            if (r.status == 404 || r.status == 409) { error = nextcloud() ? Error{"webdav_folder_missing", "The Nextcloud folder disappeared during the upload. Retry to create it again.", {}} : folderMissing(); return {{"status", "failed"}}; }
            error = failure(r, target, "upload");
            uncertain = uncertainAfter(r);
            return {{"status", uncertain ? "unknown" : "failed"}};
        }
        progress("finalizing", total, total);
        return {{"status", "uploaded"}, {"receipt", storedReceipt(target, r, name, artifact)}};
    }
    QUrl chunkFolder(const QString& id) const {
        return QUrl(server() + "/remote.php/dav/uploads/" + QString::fromLatin1(QUrl::toPercentEncoding(userId_)) + "/" + QString::fromLatin1(QUrl::toPercentEncoding(id)) + "/", QUrl::StrictMode);
    }
    void dropChunks(const QString& id) {
        if (!id.startsWith("putgd-") || resolveAccount()) return;
        client_.send("DELETE", chunkFolder(id), {});
    }
    // Nextcloud chunked upload v2: a folder of chunks, then a MOVE that assembles them. The
    // journal names the chunk folder before it exists, so a failed upload's chunks are removed.
    QJsonObject chunked(const Artifact& artifact, const QString& name, Error& error, bool& uncertain) {
        const auto target = file(name);
        const auto destinationHeader = target.toString(QUrl::FullyEncoded).toUtf8();
        const auto id = "putgd-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto part = qint64(request_["chunkBytes"].toDouble(double(chunkBytes)));
        part = qMax(part, qMax(minimumChunkBytes, (artifact.bytes + maximumChunks - 1) / maximumChunks));
        if (auto e = checkpoint({{"schemaVersion", 1}, {"uploadId", id}, {"name", name}})) { error = e; return {{"status", "failed"}}; }
        const auto folderUrl = chunkFolder(id);
        const auto made = client_.send("MKCOL", folderUrl, {{"Destination", destinationHeader}});
        if (made.status != 201) { error = failure(made, folderUrl, "chunk_start"); dropChunks(id); checkpoint({}); return {{"status", "failed"}}; }
        const QByteArray totalHeader = QByteArray::number(artifact.bytes);
        qint64 done = 0;
        const auto total = artifact.bytes;
        for (int index = 1; done < total; ++index) {
            if (cancelled.load()) { error = stoppedError(); dropChunks(id); checkpoint({}); return {{"status", "failed"}}; }
            const auto length = qMin(part, total - done);
            OneShot body(artifact.path, done, length);
            const auto url = QUrl(folderUrl.toString(QUrl::FullyEncoded) + QString("%1").arg(index, 5, 10, QChar('0')), QUrl::StrictMode);
            const auto base = done;
            const auto r = client_.send("PUT", url, {{"Destination", destinationHeader}, {"OC-Total-Length", totalHeader}}, &body, true,
                                        [base, total](qint64 sent) { stdio.progress("uploading", base + sent, total); });
            if (!r.ok()) {
                // A failed chunk changed nothing visible: the file appears only at the MOVE.
                error = failure(r, url, "chunk");
                dropChunks(id);
                checkpoint({});
                return {{"status", "failed"}};
            }
            done += length;
        }
        progress("finalizing", total, total);
        // Assembly can take a while for a large file; Overwrite: F keeps it from replacing a
        // file that appeared meanwhile.
        const auto assembled = client_.send("MOVE", QUrl(folderUrl.toString(QUrl::FullyEncoded) + ".file", QUrl::StrictMode),
                                            {{"Destination", destinationHeader}, {"Overwrite", "F"}, {"OC-Total-Length", totalHeader}}, nullptr, false, {}, 15 * 60 * 1000);
        if (assembled.status == 412) { dropChunks(id); checkpoint({}); return {{"status", "collision"}}; }
        if (assembled.ok()) {
            checkpoint({});
            return {{"status", "uploaded"}, {"receipt", storedReceipt(target, assembled, name, artifact)}};
        }
        // No answer to the MOVE: the file may be assembled. It is ours when it has this size and
        // content; the chunks then are gone.
        if (assembled.status == 0 || assembled.status >= 500) {
            for (int attempt = 0; attempt < 6 && !cancelled.load(); ++attempt) {
                const auto entry = stored(target);
                if (entry.found && entry.bytes == total) {
                    if (sameContent(target, artifact)) {
                        dropChunks(id);
                        checkpoint({});
                        return {{"status", "uploaded"}, {"receipt", receiptJson(name, entry.etag, entry.fileId, artifact)}};
                    }
                    dropChunks(id);
                    checkpoint({});
                    return {{"status", "collision"}};
                }
                QThread::msleep(5000);
            }
            error = failure(assembled, target, "chunk_assemble");
            uncertain = true;
            return {{"status", "unknown"}};
        }
        error = failure(assembled, target, "chunk_assemble");
        dropChunks(id);
        checkpoint({});
        return {{"status", "failed"}};
    }
    // Whether the file at `url` holds exactly the artifact's bytes (read, hashed, never kept).
    bool sameContent(const QUrl& url, const Artifact& artifact) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 bytes = 0;
        const auto r = client_.send("GET", url, {}, nullptr, true, {}, requestTimeoutMs, [&](const QByteArray& chunk) { hash.addData(chunk); bytes += chunk.size(); });
        return r.status == 200 && bytes == artifact.bytes && QString::fromLatin1(hash.result().toHex()) == artifact.sha256;
    }
    QString identity(const StorageReceipt& expected, Error& error, Entry* entry = nullptr) {
        if ((error = resolveAccount())) return "unknown";
        const auto name = expected.objectReference["name"].toString();
        const auto r = propfind(file(name));
        if (r.status == 404) return "missing";
        if (r.status != 207) { error = failure(r, file(name), "stat"); return "unknown"; }
        const auto found = parsePropfind(r.body);
        if (entry) *entry = found;
        const auto fileId = expected.objectReference["fileId"].toString();
        if (!fileId.isEmpty() && !found.fileId.isEmpty() && fileId != found.fileId) return "foreign";
        // Another size is another file, whatever the ETag says.
        const auto bytes = qint64(expected.objectReference["bytes"].toDouble(-1));
        if (bytes >= 0 && found.bytes >= 0 && found.bytes != bytes) return "foreign";
        if (!expected.concurrencyToken.isEmpty()) return matchTag(found.etag) == matchTag(expected.concurrencyToken) ? "own" : "foreign";
        // No ETag was recorded: the size is all there is to compare.
        return found.bytes == qint64(expected.objectReference["bytes"].toDouble(-1)) ? "own" : "foreign";
    }
    QJsonObject stat(Error& error) {
        const auto expected = StorageReceipt::fromJson(request_["receipt"].toObject());
        const auto status = identity(expected, error);
        return status == "own" ? QJsonObject{{"status", "own"}, {"receipt", expected.toJson()}} : QJsonObject{{"status", status}};
    }
    QJsonObject reconcile(Error& error) {
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        const auto name = request_["objectReference"].toObject()["name"].toString(request_["candidate"].toString());
        if ((error = resolveAccount())) return {{"status", "unknown"}};
        const auto target = file(name);
        const auto r = propfind(target);
        if (r.status == 404 || r.status == 409) return {{"status", "missing"}};
        if (r.status != 207) { error = failure(r, target, "reconcile"); return {{"status", "unknown"}}; }
        const auto entry = parsePropfind(r.body);
        if (entry.bytes != artifact.bytes) return {{"status", "foreign"}};
        if (!sameContent(target, artifact)) return {{"status", cancelled.load() ? "unknown" : "foreign"}};
        return {{"status", "own"}, {"receipt", receiptJson(name, entry.etag, entry.fileId, artifact)}};
    }
    QJsonObject remove(Error& error, bool& uncertain) {
        const auto expected = StorageReceipt::fromJson(request_["receipt"].toObject());
        auto progress = request_["progress"].toObject();
        const auto target = expected.toJson();
        if (progress.contains("target") && progress["target"].toObject() != target) return {{"status", "changed"}, {"progress", progress}};
        progress["schemaVersion"] = 1;
        progress["target"] = target;
        const auto previous = progress.value("phase");
        if (previous.toString() == "done") return {{"status", "removed"}, {"progress", progress}};
        const auto finish = [&](const QString& status) {
            progress["phase"] = "done";
            if (auto e = checkpoint(progress)) { error = e; return QJsonObject{{"status", "unknown"}, {"progress", progress}}; }
            return QJsonObject{{"status", status}, {"progress", progress}};
        };
        if (expected.concurrencyToken.isEmpty()) {
            error = {"unsupported_capability", "The server gave no ETag for this file, so put.gd can't delete it without risking another file", {}};
            return {{"status", "failed"}, {"progress", progress}};
        }
        const auto status = identity(expected, error);
        if (status == "missing") return finish(previous.toString() == "delete_intent" ? "removed" : "missing");
        if (status == "foreign") return {{"status", "changed"}, {"progress", progress}};
        if (status != "own") return {{"status", "unknown"}, {"progress", progress}};
        if (cancelled.load()) { error = stoppedError(); return {{"status", "failed"}, {"progress", progress}}; }
        progress["phase"] = "delete_intent";
        if (auto e = checkpoint(progress)) { error = e; progress["phase"] = previous; return {{"status", "failed"}, {"progress", progress}}; }
        const auto url = file(expected.objectReference["name"].toString());
        auto r = client_.send("DELETE", url, {{"If-Match", matchTag(expected.concurrencyToken)}});
        // Apache reports a weak ETag for a file written within the last second, and a weak ETag
        // never satisfies If-Match. While the file is still the same one, its ETag turns strong a
        // moment later; the deletion is sent again with it, at most twice.
        for (int attempt = 0; r.status == 412 && attempt < 2 && !cancelled.load(); ++attempt) {
            QThread::msleep(1100);
            Entry now;
            Error again;
            if (identity(expected, again, &now) != "own" || now.etag.startsWith("W/")) continue;
            r = client_.send("DELETE", url, {{"If-Match", matchTag(now.etag)}});
        }
        if (r.ok()) return finish("removed");
        if (r.status == 412) return {{"status", "changed"}, {"progress", progress}};
        if (r.status == 404) return finish("removed");
        error = failure(r, url, "delete");
        uncertain = r.status == 0 || r.status >= 500;
        return {{"status", uncertain ? "unknown" : "failed"}, {"progress", progress}};
    }
    QJsonObject abandon(Error&) {
        const auto journal = request_["journal"].toObject();
        if (journal["uploadId"].isString()) dropChunks(journal["uploadId"].toString());
        return {};
    }

    // The link shares of this file that put.gd's account owns.
    std::optional<QJsonArray> linkShares(const QString& path, Error& error) {
        const auto r = client_.ocs("GET", server(), "/ocs/v2.php/apps/files_sharing/api/v1/shares?path=" + QString::fromLatin1(QUrl::toPercentEncoding(path)) + "&reshares=false");
        if (r.status == 404) return QJsonArray{};
        if (!r.ok()) { error = failure(r, QUrl(server()), "share_inspect"); return std::nullopt; }
        QJsonArray out;
        for (const auto& value : r.data().toArray()) {
            const auto share = value.toObject();
            if (share["share_type"].toInt(-1) == 3 && !share["token"].toString().isEmpty()) out.append(share);
        }
        return out;
    }
    QJsonObject shareResult(const QString& status, StorageReceipt receipt, const QJsonObject& share = {}) {
        receipt.directUrl = status == "shared" ? sharePage(server(), share["token"].toString()) : QString();
        QJsonObject out{{"status", status}, {"receipt", receipt.toJson()}};
        if (status == "shared") {
            const auto expires = isoDate(share["expiration"].toString());
            if (!expires.isEmpty()) out["expiresAt"] = expires;
        }
        return out;
    }
    QJsonObject share(Error& error, bool& uncertain) {
        const auto receipt = StorageReceipt::fromJson(request_["receipt"].toObject());
        const auto options = request_["options"].toObject();
        const auto action = options["action"].toString();
        if (!nextcloud()) { error = {"sharing_unsupported", "Plain WebDAV links come from the link address; there is no share step", {}}; return {}; }
        if ((error = resolveAccount()) || (error = loadCapabilities())) return {{"status", "failed"}};
        // The file must still be the one uploaded: a replaced file gets no link.
        const auto current = identity(receipt, error);
        if (current == "missing" || current == "foreign") { error = {}; return shareResult("changed", receipt); }
        if (current != "own") return {{"status", "unknown"}};
        const auto path = receipt.objectReference["path"].toString(sharePath(receipt.objectReference["name"].toString()));
        const auto existing = linkShares(path, error);
        if (!existing) return {{"status", action == "inspect" ? "unknown" : "failed"}};
        if (action == "inspect") return existing->isEmpty() ? shareResult("private", receipt) : shareResult("shared", receipt, existing->first().toObject());
        if (action == "revoke") {
            for (const auto& value : *existing) {
                const auto r = client_.ocs("DELETE", server(), "/ocs/v2.php/apps/files_sharing/api/v1/shares/" + QString::number(value.toObject()["id"].toVariant().toLongLong()));
                if (!r.ok() && r.status != 404) { error = failure(r, QUrl(server()), "share_revoke"); uncertain = r.status == 0; return {{"status", uncertain ? "unknown" : "failed"}}; }
            }
            return shareResult("revoked", receipt);
        }
        if (action != "create") { error = {"invalid_request", "Unknown sharing action", {}}; return {}; }
        // One link per file: a link that already exists (an earlier attempt's) is the link.
        if (!existing->isEmpty()) return shareResult("shared", receipt, existing->first().toObject());
        if (!linksEnabled()) { error = sharingDisabled(); return {{"status", "failed"}}; }
        if (passwordEnforced()) { error = passwordRequired(); return {{"status", "failed"}}; }
        QUrlQuery form;
        form.addQueryItem("path", path);
        form.addQueryItem("shareType", "3");
        form.addQueryItem("permissions", "1");
        int days = options["expirationDays"].toInt(0);
        if (const auto enforced = enforcedDays(); enforced > 0) days = days > 0 ? qMin(days, enforced) : enforced;
        if (days > 0) form.addQueryItem("expireDate", QDate::currentDate().addDays(days).toString("yyyy-MM-dd"));
        if (auto e = checkpoint({{"schemaVersion", 1}, {"phase", "intent"}, {"action", "create"}})) { error = e; return {{"status", "failed"}}; }
        const auto r = client_.ocs("POST", server(), "/ocs/v2.php/apps/files_sharing/api/v1/shares", form.query(QUrl::FullyEncoded).toUtf8());
        if (r.ok()) {
            const auto made = r.data().toObject();
            if (made["token"].toString().isEmpty()) { error = {"webdav_protocol", "Nextcloud made a share without a link token", {}}; uncertain = true; return {{"status", "unknown"}}; }
            return shareResult("shared", receipt, made);
        }
        const auto message = r.ocsMessage();
        if (message.contains("assword", Qt::CaseInsensitive)) { error = passwordRequired(); return {{"status", "failed"}}; }
        if (r.status == 403 || r.status == 404) {
            if (message.contains("disabled", Qt::CaseInsensitive) || message.contains("not allowed", Qt::CaseInsensitive)) { error = sharingDisabled(); return {{"status", "failed"}}; }
        }
        error = failure(r, QUrl(server()), "share_create");
        if (!message.isEmpty()) error.message += " Nextcloud said: " + message;
        uncertain = uncertainAfter(r);
        return {{"status", uncertain ? "unknown" : "failed"}};
    }

    // Login Flow v2: the user signs in in the browser; put.gd polls until Nextcloud hands
    // over an app password, the flow times out, or the user cancels.
    QJsonObject signIn(Error& error) {
        const auto server = nextcloudServer(settings()["server"].toString());
        const bool insecure = settings()["allowInsecureHttp"].toBool();
        if (server.isEmpty()) { error = {"webdav_settings_invalid", "Enter the Nextcloud server address first", {}}; return {}; }
        if (!http::transportAllowed(QUrl(server), insecure)) {
            error = {"webdav_plain_http_refused", "put.gd doesn't sign in over plain http:// to another computer unless Allow plain HTTP for this destination is on.", {}};
            return {};
        }
        Client client({}, {}, insecure);
        const auto entered = QUrl(server);
        const auto start = client.send("POST", QUrl(server + "/index.php/login/v2"), {{"Accept", "application/json"}, {"User-Agent", "put.gd (" + QHostInfo::localHostName().left(64).toUtf8() + ")"}});
        if (start.status == 404 || start.status == 405) {
            error = {"nextcloud_signin_unavailable", "This server doesn't offer Nextcloud's browser sign-in (Login Flow v2). Create an app password in its security settings and use App password instead.", {}};
            return {};
        }
        if (!start.ok()) { destination_.settings["server"] = server; error = failure(start, QUrl(server), "sign_in"); return {}; }
        const auto flow = QJsonDocument::fromJson(start.body).object();
        const QUrl login(flow["login"].toString(), QUrl::StrictMode);
        const QUrl endpoint(flow["poll"].toObject()["endpoint"].toString(), QUrl::StrictMode);
        const auto token = flow["poll"].toObject()["token"].toString();
        if (token.isEmpty() || !login.isValid() || !endpoint.isValid()) {
            error = {"nextcloud_not_found", server + " doesn't answer like a Nextcloud server. Check the address: it is the one you open Nextcloud at in a browser.", {}};
            return {};
        }
        // The poll and the browser page stay on the server the user entered.
        const auto sameHost = [&](const QUrl& url) { return url.host().compare(entered.host(), Qt::CaseInsensitive) == 0 && QStringList{"http", "https"}.contains(url.scheme()); };
        if (!sameHost(login) || !sameOrigin(endpoint, entered)) {
            error = {"nextcloud_server_mismatch", "Nextcloud named another address for sign-in (" + (sameHost(login) ? endpoint.host() : login.host()) +
                                                      "). Its overwrite.cli.url or trusted proxy settings may be wrong; use the address it names, or ask the administrator.", {}};
            return {};
        }
        stdio.emitLine({{"event", "signIn"}, {"url", login.toString(QUrl::FullyEncoded)}});
        QElapsedTimer clock;
        clock.start();
        const auto timeout = qBound(1000, request_["timeoutMs"].toInt(signInTimeoutMs), signInTimeoutMs);
        QJsonObject granted;
        while (true) {
            if (cancelled.load()) { error = {"nextcloud_signin_cancelled", "Sign-in cancelled", {}}; return {}; }
            if (clock.elapsed() > timeout) { error = {"nextcloud_signin_timeout", "Sign-in wasn't finished in the browser in time. Choose Sign in to start again.", {}}; return {}; }
            OneShot body("token=" + QUrl::toPercentEncoding(token));
            const auto poll = client.send("POST", endpoint, {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}}, &body);
            if (poll.ok()) { granted = QJsonDocument::fromJson(poll.body).object(); break; }
            if (poll.status != 404 && poll.status != 0) { error = failure(poll, endpoint, "sign_in_poll"); return {}; }
            for (int wait = 0; wait < 40 && !cancelled.load(); ++wait) QThread::msleep(50);
        }
        const QUrl returned(granted["server"].toString());
        const auto loginName = granted["loginName"].toString();
        auto appPassword = granted["appPassword"].toString();
        if (loginName.isEmpty() || appPassword.isEmpty()) { error = {"webdav_protocol", "Nextcloud finished sign-in without an app password", {}}; return {}; }
        if (!sameOrigin(returned, entered)) {
            error = {"nextcloud_server_mismatch", "Nextcloud finished sign-in for another address (" + returned.host() + "). Enter that address as the server, or ask the administrator to fix overwrite.cli.url.", {}};
            Client revoke(loginName, appPassword, insecure);
            revoke.ocs("DELETE", server, "/ocs/v2.php/core/apppassword");
            return {};
        }
        destination_.settings["server"] = server;
        Client signedIn(loginName, appPassword, insecure);
        const auto user = signedIn.ocs("GET", server, "/ocs/v2.php/cloud/user");
        const auto caps = signedIn.ocs("GET", server, "/ocs/v2.php/cloud/capabilities");
        if (!user.ok()) { error = failure(user, QUrl(server), "account"); return {}; }
        account_ = user.data().toObject();
        userId_ = account_["id"].toString();
        if (caps.ok()) capabilities_ = caps.data().toObject();
        auto out = accountJson();
        out["loginName"] = loginName;
        out["appPassword"] = appPassword;
        appPassword.fill(QChar('\0'));
        return out;
    }
    // Revokes the app password on the server. A password that is already gone, or one that
    // is the account's own password (not an app password), is not revoked and not an error.
    QJsonObject signOut(Error& error) {
        if (client_.anonymous()) return {{"revoked", false}};
        const auto r = client_.ocs("DELETE", server(), "/ocs/v2.php/core/apppassword");
        if (r.ok()) return {{"revoked", true}};
        if (r.status == 401 || r.status == 403 || r.status == 404) return {{"revoked", false}};
        error = failure(r, QUrl(server()), "sign_out");
        return {{"revoked", false}};
    }

    void progress(const QString& phase, qint64 sent, qint64 total) { stdio.progress(phase, sent, total); }

    // Synthetic test file through the production upload path, fetched anonymously through the
    // link put.gd would hand out and compared byte for byte, then removed with If-Match.
    QJsonObject testDestination(Error& error) {
        QJsonObject out;
        QJsonArray warnings;
        const auto warn = [&](const QString& code, const QString& message) { warnings.append(QJsonObject{{"code", code}, {"message", message}}); out["warnings"] = warnings; };
        if (nextcloud()) {
            if ((error = resolveAccount()) || (error = loadCapabilities())) return out;
            out = accountJson();
            if (!linksEnabled()) { error = sharingDisabled(); return out; }
            if (passwordEnforced()) { error = passwordRequired(); return out; }
            if (const auto days = enforcedDays(); days > 0)
                warn("nextcloud_share_expiry_enforced", QString("This server makes every public link expire after %1 days; put.gd's links follow that.").arg(days));
            bool created = false;
            if ((error = ensureFolder(&created))) return out;
            out["folderCreated"] = created;
        } else {
            // A Nextcloud server works better as a Nextcloud destination; never switched by itself.
            const auto base = nextcloudServer(settings()["address"].toString());
            const auto status = anonymous_.send("GET", QUrl(base + "/status.php"), {{"Accept", "application/json"}});
            const auto product = QJsonDocument::fromJson(status.body).object();
            if (status.ok() && product["installed"].toBool() && product.contains("versionstring")) {
                out["nextcloudDetected"] = true;
                warn("webdav_is_nextcloud", "This is a " + http::boundedMessage(product["productname"].toString("Nextcloud"), 40) +
                                                " server. A Nextcloud destination signs in through the browser and makes share links; change Storage service to Nextcloud to use it.");
            }
            const auto probe = propfind(folder());
            if (probe.status == 404 || probe.status == 409) { error = folderMissing(); return out; }
            if (probe.status != 207) { error = failure(probe, folder(), probe.status == 0 ? "connect" : "folder"); return out; }
            if (!parsePropfind(probe.body).collection) { error = {"webdav_folder_missing", "The address is a file, not a folder. Enter the folder's address, ending with /.", {}}; return out; }
        }
        const auto synthetic = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto bytes = QByteArray("put.gd connection test " + synthetic.toUtf8() + "\n");
        QTemporaryDir dir;
        const auto path = dir.filePath("synthetic.txt");
        QFile temporary(path);
        if (!dir.isValid() || !temporary.open(QIODevice::WriteOnly) || temporary.write(bytes) != bytes.size()) { error = {"synthetic_artifact_failed", "Could not create the private synthetic file", {}}; return out; }
        temporary.close();
        Artifact artifact;
        artifact.id = "synthetic-" + synthetic;
        artifact.path = path;
        artifact.name = "putgd-test-" + synthetic + ".txt";
        artifact.mime = "text/plain";
        artifact.bytes = bytes.size();
        artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        request_["candidate"] = artifact.name;
        request_["artifact"] = artifact.toJson();
        request_["publicationId"] = synthetic;
        request_["condition"] = "absent";
        if (auto e = checkpoint({{"schemaVersion", 1}, {"phase", "synthetic_upload_intent"}, {"destination", destination_.toJson()},
                                 {"objectReference", QJsonObject{{"schemaVersion", 1}, {"name", artifact.name}}}, {"publicationId", synthetic}, {"sha256", artifact.sha256}, {"bytes", double(artifact.bytes)}})) {
            error = e;
            return out;
        }
        bool uncertain = false;
        const auto uploaded = upload(error, uncertain);
        out["uploadStatus"] = uploaded["status"].toString(uncertain || !error ? "unknown" : "failed");
        if (error || uploaded["status"] != "uploaded") {
            if (!error) error = {"synthetic_collision", "The synthetic test file name is unavailable", {}};
            return out;
        }
        const auto receipt = StorageReceipt::fromJson(uploaded["receipt"].toObject());
        // The journal keeps the receipt without its link: a Nextcloud link's token is a secret in its path.
        auto recorded = receipt;
        recorded.directUrl.clear();
        if (auto e = checkpoint({{"schemaVersion", 1}, {"phase", "synthetic_uploaded"}, {"receipt", recorded.toJson()}})) error = e;
        out["receipt"] = recorded.toJson();
        QString link, fetchUrl;
        if (!error && nextcloud()) {
            request_["receipt"] = receipt.toJson();
            request_["options"] = QJsonObject{{"action", "create"}};
            bool shareUncertain = false;
            const auto shared = share(error, shareUncertain);
            out["shareStatus"] = shared["status"];
            if (shared["status"] == "shared") {
                link = StorageReceipt::fromJson(shared["receipt"].toObject()).directUrl;
                fetchUrl = shareDownload(server(), shareToken(link));
                if (shared.contains("expiresAt")) out["expiresAt"] = shared["expiresAt"];
            }
        } else if (!error) {
            link = receipt.directUrl;
            fetchUrl = link;
            // Whether the server honours the preconditions put.gd relies on: a second create
            // must be refused, and a delete with another ETag too.
            OneShot again(bytes);
            const auto second = client_.send("PUT", file(artifact.name), {{"If-None-Match", "*"}, {"Content-Type", "text/plain"}}, &again);
            out["conditionalCreate"] = second.status == 412;
            if (second.status != 412)
                warn("webdav_conditional_create_ignored", "The server doesn't refuse a second upload to the same name (If-None-Match). put.gd checks names first, but two uploads of the same name at the same moment could replace each other.");
            if (receipt.concurrencyToken.isEmpty())
                warn("webdav_no_etag", "The server reports no ETag for uploaded files, so Delete in History is unavailable for this destination.");
        }
        if (!error && nextcloud() && link.isEmpty())
            error = {"public_url_unreachable", "The upload worked, but Nextcloud made no share link (" + out["shareStatus"].toString("no answer") + "). Test again.", {}};
        if (!link.isEmpty()) out["link"] = redactShareTokens(link);
        bool reachable = false;
        if (!fetchUrl.isEmpty()) {
            const auto fetched = anonymous_.fetch(QUrl(fetchUrl, QUrl::StrictMode));
            reachable = fetched.status == 200 && fetched.body == bytes;
            out["linkStatus"] = fetched.status;
            if (!reachable && !error) {
                const bool page = fetched.status == 200 && fetched.header("content-type").startsWith("text/html");
                if (page) error = {"public_url_unreachable", nextcloud() ? "The upload worked, but the share link opened a web page (likely a sign-in page) instead of the file. Check the server's sharing settings."
                                                                         : "The upload worked, but the link opened a web page (likely a sign-in page) instead of the file. Set Link address to where these files are served publicly.", {{"httpStatus", fetched.status}}};
                else if (fetched.status == 0) error = {"public_url_unreachable", "The upload worked, but the link didn't open: " + fetched.detail + ".", {{"httpStatus", 0}}};
                else error = {"public_url_unreachable", QString("The upload worked, but the link didn't open (HTTP %1). ").arg(fetched.status) +
                                                         (nextcloud() ? QStringLiteral("Check the server's sharing settings.") : QStringLiteral("Set Link address to where these files are served publicly, or allow public reading on the server.")),
                              {{"httpStatus", fetched.status}}};
            }
        }
        out["publicReachable"] = reachable;
        if (!nextcloud() && !receipt.concurrencyToken.isEmpty()) {
            const auto wrong = client_.send("DELETE", file(artifact.name), {{"If-Match", "\"putgd-test-mismatch\""}});
            out["conditionalDelete"] = wrong.status == 412;
            if (wrong.status != 412) {
                warn("webdav_conditional_delete_ignored", "The server ignores If-Match on delete: it deleted the test file although its ETag didn't match. A file replaced on the server after an upload could then be deleted from History.");
                out["cleanupStatus"] = wrong.ok() ? "removed" : "unknown";
                return out;
            }
        }
        request_["receipt"] = recorded.toJson();
        request_["progress"] = QJsonObject{};
        Error cleanup;
        bool cleanupUncertain = false;
        if (recorded.concurrencyToken.isEmpty()) {
            // No ETag: the test removes its own file without a precondition.
            const auto r = client_.send("DELETE", file(artifact.name), {});
            out["cleanupStatus"] = r.ok() || r.status == 404 ? "removed" : "unknown";
            checkpoint({{"schemaVersion", 1}, {"phase", "done"}, {"target", recorded.toJson()}});
        } else {
            const auto removed = remove(cleanup, cleanupUncertain);
            out["cleanupStatus"] = removed["status"];
            out["cleanupProgress"] = removed["progress"];
        }
        if (!error && cleanup) error = cleanup;
        return out;
    }
};
}
}

int main(int argc, char** argv) {
    return putgd::dav::stdio.run(argc, argv, "putgd-webdav-worker", putgd::buildManifestJson(), putgd::useHostCaBundle,
        [](QJsonObject& request, putgd::Error& error, bool& uncertain) {
            putgd::dav::Worker worker(request);
            request = {};
            return worker.run(error, uncertain);
        });
}
