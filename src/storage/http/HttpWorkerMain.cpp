// putgd-http-worker: one custom HTTP uploader request per process (#1361).
//
// The resident writes one request line on stdin: {schemaVersion:1, method, destination,
// secrets:{key:value}, ...}. Header, parameter and argument values marked secret arrive
// only there, never in argv or the environment, and are never echoed. Later stdin lines
// are {"ack":true} or {"ack":false,"error":{...}} for a checkpoint, and {"cancel":true}.
// A closed stdin cancels. The worker writes newline JSON on stdout:
// {"event":"progress",...}, {"event":"checkpoint","journal":{...}} (it then waits for the
// ack), and one final {"ok":true,"result":{...}} or
// {"ok":false,"error":{...},"uncertain":bool}. All HTTP happens here (Qt Network); the
// resident links none of it.
#include "HttpCommon.h"
#include "HttpTemplate.h"
#include "core/BuildInfo.h"
#include "core/CaBundle.h"
#include "core/StorageProvider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
namespace putgd::http {
namespace {
constexpr qint64 maximumLine = 1024 * 1024;
constexpr qsizetype maximumReplyBytes = 4 * 1024 * 1024;
constexpr int maximumRedirects = 5;
std::atomic_bool cancelled{false};
std::mutex controlMutex;
std::condition_variable controlChanged;
std::deque<QJsonObject> acks;
bool parentGone = false;

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
Error checkpoint(const QJsonObject& journal) {
    emitLine({{"event", "checkpoint"}, {"journal", journal}});
    std::unique_lock lock(controlMutex);
    if (!controlChanged.wait_for(lock, std::chrono::minutes(5), [] { return !acks.empty() || parentGone; }))
        return {"http_checkpoint_timeout", "The resident did not save the request journal in time", {}};
    if (acks.empty()) return {"cancelled", "The resident stopped this request", {}};
    const auto ack = acks.front();
    acks.pop_front();
    if (ack["ack"].toBool()) return {};
    const auto e = ack["error"].toObject();
    return {e["code"].toString("checkpoint_failed"), e["message"].toString("The request journal could not be saved"), {}};
}
Error stoppedError() { return {"cancelled", "Upload cancelled", {}}; }

// The request body: bytes before the file, the file, bytes after it. It can be read
// exactly once. Qt Network silently sends a request again when the server closes the
// connection before replying; a body it cannot rewind turns that into an error, so no
// upload is ever repeated behind put.gd's back.
class Body final : public QIODevice {
public:
    Body(QByteArray before, const QString& path, qint64 fileBytes, QByteArray after)
        : before_(std::move(before)), after_(std::move(after)), file_(path), fileBytes_(path.isEmpty() ? 0 : fileBytes) {}
    bool open(OpenMode mode) override {
        if (fileBytes_ > 0 && !file_.open(QIODevice::ReadOnly)) return false;
        return QIODevice::open(mode);
    }
    void close() override { file_.close(); QIODevice::close(); }
    bool isSequential() const override { return true; }
    qint64 total() const { return before_.size() + fileBytes_ + after_.size(); }
    qint64 bytesAvailable() const override { return (total() - read_) + QIODevice::bytesAvailable(); }
    bool atEnd() const override { return read_ >= total() && QIODevice::bytesAvailable() == 0; }
    bool reset() override { return false; }
    qint64 size() const override { return total(); }
    // Every byte has been handed to the transport: the server may have the whole request.
    bool consumed() const { return read_ >= total(); }
    qint64 fileOffset() const { return before_.size(); }
    qint64 fileBytes() const { return fileBytes_; }
protected:
    qint64 readData(char* data, qint64 maximum) override {
        qint64 done = 0;
        while (done < maximum && read_ < total()) {
            qint64 n = 0;
            if (read_ < before_.size()) {
                n = qMin(maximum - done, before_.size() - read_);
                std::memcpy(data + done, before_.constData() + read_, size_t(n));
            } else if (read_ < before_.size() + fileBytes_) {
                n = file_.read(data + done, qMin(maximum - done, before_.size() + fileBytes_ - read_));
                if (n <= 0) { failed_ = true; return done > 0 ? done : -1; }
            } else {
                const auto offset = read_ - before_.size() - fileBytes_;
                n = qMin(maximum - done, after_.size() - offset);
                std::memcpy(data + done, after_.constData() + offset, size_t(n));
            }
            read_ += n;
            done += n;
        }
        if (done == 0) return read_ >= total() ? -1 : 0;
        return done;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QByteArray before_, after_;
    QFile file_;
    qint64 fileBytes_ = 0, read_ = 0;
    bool failed_ = false;
};

using Headers = QList<QPair<QByteArray, QByteArray>>;
struct Exchange {
    int status = 0;            // HTTP status; 0 when no reply arrived
    QByteArray body;           // at most maximumReplyBytes
    Headers headers;
    QUrl url;                  // the address that answered
    int sends = 0;             // times the transport wrote the request; more than one is its own resend
    bool reached = false;      // the complete request was handed to the transport
    bool connected = false;    // the connection was up before the request ended
    bool stopped = false;      // cancelled
    bool deadline = false;     // past the connect or request deadline
    QString detail;            // bounded transport description, never request data
    QString header(const char* name) const {
        for (const auto& [key, value] : headers) if (key.compare(name, Qt::CaseInsensitive) == 0) return QString::fromUtf8(value);
        return {};
    }
    QString text() const { return QString::fromUtf8(body); }
};
QString transportReason(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ConnectionRefusedError: return QStringLiteral("connection refused");
    case QNetworkReply::RemoteHostClosedError: return QStringLiteral("the server closed the connection");
    case QNetworkReply::HostNotFoundError: return QStringLiteral("host not found");
    case QNetworkReply::TimeoutError: return QStringLiteral("timed out");
    case QNetworkReply::SslHandshakeFailedError: return QStringLiteral("the TLS handshake failed; the certificate may not be trusted");
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError: return QStringLiteral("the network is unavailable");
    case QNetworkReply::ContentReSendError: return QStringLiteral("the connection closed before the reply");
    case QNetworkReply::OperationCanceledError: return QStringLiteral("stopped");
    default: return QStringLiteral("network error %1").arg(int(error));
    }
}

class Client {
public:
    Client(int connectMs, int requestMs) : connectMs_(connectMs), requestMs_(requestMs) {}
    // One request, no redirects followed. `sent` reports body bytes the transport has read.
    Exchange send(const QByteArray& verb, const QUrl& url, const Headers& headers, Body* body = nullptr, const std::function<void(qint64)>& sent = {}) {
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        // HTTP/1.1: one request on one connection, so "was it sent" has a plain answer.
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setRawHeader("User-Agent", QByteArray("put.gd/") + buildVersion());
        for (const auto& [name, value] : headers) request.setRawHeader(name, value);
        QNetworkReply* reply = nullptr;
        if (body) {
            request.setHeader(QNetworkRequest::ContentLengthHeader, body->total());
            request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
            reply = nam_.sendCustomRequest(request, verb, body);
        } else reply = nam_.sendCustomRequest(request, verb);
        QEventLoop loop;
        Exchange out;
        out.url = url;
        bool requestSent = false;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::requestSent, &loop, [&] { requestSent = true; out.connected = true; ++out.sends; });
        QObject::connect(reply, &QNetworkReply::encrypted, &loop, [&] { out.connected = true; });
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] { out.connected = true; });
        QObject::connect(reply, &QNetworkReply::uploadProgress, &loop, [&](qint64 bytes, qint64) { if (bytes > 0) out.connected = true; if (sent) sent(bytes); });
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            const auto chunk = reply->readAll();
            if (out.body.size() < maximumReplyBytes) out.body.append(chunk.left(maximumReplyBytes - out.body.size()));
        });
        QTimer poll;
        poll.setInterval(50);
        QElapsedTimer clock;
        clock.start();
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (out.stopped || out.deadline) return;
            if (cancelled.load()) { out.stopped = true; reply->abort(); }
            else if (clock.elapsed() > requestMs_ || (!out.connected && clock.elapsed() > connectMs_)) { out.deadline = true; reply->abort(); }
        });
        poll.start();
        if (!reply->isFinished()) loop.exec();
        poll.stop();
        out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto rest = reply->readAll();
        if (out.body.size() < maximumReplyBytes) out.body.append(rest.left(maximumReplyBytes - out.body.size()));
        out.headers = reply->rawHeaderPairs();
        out.reached = body ? body->consumed() : requestSent;
        if (out.status == 0 || out.stopped || out.deadline) {
            // An aborted reply has no usable status even when headers had arrived.
            if (out.stopped || out.deadline) out.status = 0;
            out.detail = out.stopped ? QStringLiteral("cancelled") : out.deadline ? (out.connected ? QStringLiteral("no reply within the request timeout") : QStringLiteral("could not connect within the connect timeout"))
                                                                                  : transportReason(reply->error());
        }
        reply->deleteLater();
        return out;
    }
    // A GET that follows redirects and sends nothing but the address: the returned link
    // and the deletion URL. Plain HTTP follows the destination's transport rule.
    Exchange get(QUrl url, bool allowInsecure, QString* refused = nullptr) {
        Exchange out;
        for (int hop = 0; hop <= maximumRedirects; ++hop) {
            out = send("GET", url, {});
            if (!QList<int>{301, 302, 303, 307, 308}.contains(out.status)) return out;
            const auto target = url.resolved(QUrl(out.header("location")));
            if (!target.isValid() || !transportAllowed(target, allowInsecure) || (url.scheme() == "https" && target.scheme() != "https")) {
                if (refused) *refused = QStringLiteral("it redirects to an address put.gd does not follow");
                return out;
            }
            url = target;
        }
        if (refused) *refused = QStringLiteral("it redirects too many times");
        return out;
    }
private:
    QNetworkAccessManager nam_;
    int connectMs_, requestMs_;
};

struct Hashes { QByteArray sha256; qint64 bytes = 0; };
bool hashFile(const QString& path, Hashes& out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (true) {
        const auto n = file.read(buffer.data(), buffer.size());
        if (n < 0) return false;
        if (n == 0) break;
        hash.addData(QByteArrayView(buffer.constData(), n));
        out.bytes += n;
        if (cancelled.load()) return false;
    }
    out.sha256 = hash.result().toHex();
    return true;
}
QByteArray formName(const QString& value) {
    auto bytes = value.toUtf8();
    bytes.replace('"', "%22").replace('\r', "%0D").replace('\n', "%0A");
    return bytes;
}
bool controlFree(const QString& text) {
    for (const auto c : text) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return true;
}
// 67 bytes: a valid 1x1 PNG, for an uploader that takes images only.
const QByteArray& syntheticPng() {
    static const QByteArray bytes = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
    return bytes;
}

// A deletion page that asks "are you sure?" with a form posting back to its own server,
// as XBackBone's does. The user has already confirmed in put.gd, so that form is sent
// once. Only a POST form on the same host, with nothing to fill in, whose address ends
// with the deletion URL's own last segment, qualifies.
struct Confirmation { QUrl action; QByteArray body; bool found = false; };
Confirmation confirmationForm(const QString& html, const QUrl& page) {
    static const QRegularExpression form("<form\\b([^>]*)>(.*?)</form>", QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression input("<input\\b([^>]*)>", QRegularExpression::CaseInsensitiveOption);
    const auto attribute = [](const QString& tag, const QString& name) {
        const QRegularExpression pattern("\\b" + name + "\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", QRegularExpression::CaseInsensitiveOption);
        const auto match = pattern.match(tag);
        return match.hasMatch() ? QString(match.captured(1) + match.captured(2) + match.captured(3)).replace("&amp;", "&") : QString();
    };
    const auto last = page.path().section('/', -1);
    if (html.size() > 1024 * 1024 || last.isEmpty()) return {};
    for (auto it = form.globalMatch(html); it.hasNext();) {
        const auto match = it.next();
        if (attribute(match.captured(1), "method").compare("post", Qt::CaseInsensitive) != 0) continue;
        const auto action = page.resolved(QUrl(attribute(match.captured(1), "action")));
        if (!action.isValid() || !sameHost(page, action) || action.path().section('/', -1) != last) continue;
        QByteArray body;
        bool fillable = false;
        for (auto fields = input.globalMatch(match.captured(2)); fields.hasNext();) {
            const auto tag = fields.next().captured(1);
            const auto type = attribute(tag, "type").toLower();
            if (type == "submit" || type == "button") continue;
            if (type != "hidden") { fillable = true; break; }
            if (!body.isEmpty()) body.append('&');
            body += QUrl::toPercentEncoding(attribute(tag, "name")) + '=' + QUrl::toPercentEncoding(attribute(tag, "value"));
        }
        if (fillable || match.captured(2).contains("<select", Qt::CaseInsensitive) || match.captured(2).contains("<textarea", Qt::CaseInsensitive)) continue;
        return {action, body, true};
    }
    return {};
}

class Worker {
public:
    Worker(const QJsonObject& request) : request_(request), destination_(Destination::fromJson(request["destination"].toObject())),
          client_(destination_.settings["connectTimeoutMs"].toInt(10000), destination_.settings["requestTimeoutMs"].toInt(600000)) {
        const auto secrets = request_.take("secrets").toObject();
        for (auto it = secrets.begin(); it != secrets.end(); ++it) secrets_.insert(it.key(), it.value().toString());
    }
    ~Worker() { for (auto& value : secrets_) value.fill(QChar('\0')); }
    QJsonObject run(Error& error, bool& uncertain) {
        const auto method = request_["method"].toString();
        if (destination_.providerId != providerId) { error = {"http_protocol", "Request is not for a custom uploader destination", {}}; return {}; }
        if (auto e = validateSettings(destination_.settings)) { error = e; return {}; }
        if (method == "upload") return upload(error, uncertain);
        if (method == "remove") return remove(error, uncertain);
        if (method == "testDestination") return testDestination(error);
        error = {"http_protocol", "Unsupported worker method", {}};
        return {};
    }
private:
    QJsonObject request_;
    Destination destination_;
    Client client_;
    QHash<QString, QString> secrets_;
    QJsonObject settings() const { return destination_.settings; }
    bool allowInsecure() const { return settings()["allowInsecureHttp"].toBool(); }

    void progress(const QString& phase, qint64 sent, qint64 total) {
        static QElapsedTimer last;
        if (phase == "uploading" && last.isValid() && last.elapsed() < 250 && sent < total) return;
        last.start();
        UploadProgress value;
        value.phase = phase;
        value.bytesTransferred = qBound<qint64>(0, sent, total);
        value.bytesTotal = total;
        auto line = value.toJson();
        line["event"] = "progress";
        emitLine(line);
    }
    // Server text may repeat what it was sent: a stored value never leaves in a message.
    QString scrub(QString text) const {
        for (const auto& value : secrets_) {
            if (value.size() >= 4) text.replace(value, QStringLiteral("<redacted>"));
            for (const auto& part : value.split(' ', Qt::SkipEmptyParts)) if (part.size() >= 8) text.replace(part, QStringLiteral("<redacted>"));
        }
        return text;
    }
    // A row's value: from settings, or from the credential store for a secret row.
    bool rowValue(const QString& list, const QJsonObject& row, const RequestValues& values, QString& out, Error& error) const {
        const auto name = row["name"].toString();
        const auto key = secretKey(list, name);
        if (row["secret"].toBool() && !secrets_.contains(key)) {
            error = {"credential_setup_required", "A stored value of this destination is missing (" + secretLabel(key) + "). Enter it in Settings and store the credentials again.", {}};
            return false;
        }
        QString problem;
        out = renderRequest(row["secret"].toBool() ? secrets_.value(key) : row["value"].toString(), values, &problem);
        if (!problem.isEmpty()) {
            error = {problem.contains("{input}") ? "http_text_only" : "http_value_invalid", "The " + secretLabel(key) + " of this destination cannot be used: " + problem, {}};
            return false;
        }
        return true;
    }
    struct Plan {
        QByteArray verb;
        QUrl url;
        Headers headers;
        QByteArray before, after;   // body bytes around the file
        bool file = false;          // the body carries the file's bytes
        bool body = false;
    };
    bool plan(const Artifact& artifact, const QString& fileName, Plan& out, Error& error) const {
        const auto s = settings();
        const auto kind = artifactKind(artifact.mime, artifact.category);
        if (s["requestUrl"].toString().isEmpty()) { error = {"http_setup_required", "This destination has no request URL yet. Enter it in Settings, or import a .sxcu file.", {}}; return false; }
        if (!acceptsKind(s, kind)) {
            QStringList kinds;
            for (const auto& value : s["accepts"].toArray()) kinds.append(value.toString() == "image" ? "images" : value.toString());
            error = {"http_type_unsupported", "This uploader takes " + kinds.join(" and ") + " only, so it cannot upload this file.", {{"kind", kind}}};
            return false;
        }
        const auto body = s["body"].toString();
        out.file = body == "binary" || (body == "multipart" && !s["fileFormName"].toString().isEmpty());
        out.body = body != "none";
        out.verb = s["requestMethod"].toString().toLatin1();
        RequestValues values;
        values.fileName = fileName;
        values.secrets = &secrets_;
        if (kind == "text" && artifact.bytes <= maximumInputBytes) {
            QFile file(artifact.path);
            if (file.open(QIODevice::ReadOnly)) { values.input = QString::fromUtf8(file.readAll()); values.hasInput = true; }
        }
        if (!out.file && !values.hasInput) {
            error = {"http_text_only", "This uploader sends text in its request ({input}); it cannot upload this file.", {{"kind", kind}}};
            return false;
        }
        // A request that carries the file may still name {input}, as XBackBone's does:
        // for a file that is not text it is empty, as it is in ShareX.
        values.hasInput = true;
        QString problem;
        auto address = values;
        address.encode = RequestValues::Encode::Url;
        const auto rendered = renderRequest(s["requestUrl"].toString(), address, &problem);
        QUrl url(rendered, QUrl::TolerantMode);
        if (!problem.isEmpty() || !url.isValid() || url.host().isEmpty() || !transportAllowed(url, allowInsecure())) {
            error = {"http_settings_invalid", "The request URL of this destination cannot be used" + (problem.isEmpty() ? QString(".") : ": " + problem), {}};
            return false;
        }
        QByteArray query;
        for (const auto& value : s["parameters"].toArray()) {
            QString text;
            if (!rowValue("parameters", value.toObject(), values, text, error)) return false;
            if (!query.isEmpty()) query.append('&');
            query.append(QUrl::toPercentEncoding(value.toObject()["name"].toString()) + '=' + QUrl::toPercentEncoding(text));
        }
        if (!query.isEmpty()) url = QUrl::fromEncoded(url.toEncoded(QUrl::RemoveQuery | QUrl::RemoveFragment) + '?' + query, QUrl::StrictMode);
        if (!url.isValid()) { error = {"http_settings_invalid", "The request URL of this destination cannot be used.", {}}; return false; }
        out.url = url;
        bool contentType = false;
        for (const auto& value : s["headers"].toArray()) {
            QString text;
            if (!rowValue("headers", value.toObject(), values, text, error)) return false;
            const auto name = value.toObject()["name"].toString();
            if (!controlFree(text)) { error = {"http_value_invalid", "The header " + name.left(64) + " has a line break or control character.", {}}; return false; }
            if (name.compare("content-type", Qt::CaseInsensitive) == 0) contentType = true;
            out.headers.append({name.toLatin1(), text.toUtf8()});
        }
        QList<QPair<QString, QString>> arguments;
        if (body == "multipart" || body == "form")
            for (const auto& value : s["arguments"].toArray()) {
                QString text;
                if (!rowValue("arguments", value.toObject(), values, text, error)) return false;
                arguments.append({value.toObject()["name"].toString(), text});
            }
        if (body == "multipart") {
            const QByteArray boundary = "----putgd" + QUuid::createUuid().toString(QUuid::Id128).toLatin1();
            for (const auto& [name, text] : arguments)
                out.before += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + formName(name) + "\"\r\n\r\n" + text.toUtf8() + "\r\n";
            if (out.file) {
                out.before += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + formName(s["fileFormName"].toString()) + "\"; filename=\"" + formName(fileName) +
                              "\"\r\nContent-Type: " + (artifact.mime.isEmpty() || !controlFree(artifact.mime) ? QByteArray("application/octet-stream") : artifact.mime.toUtf8()) + "\r\n\r\n";
                out.after = "\r\n--" + boundary + "--\r\n";
            } else out.before += "--" + boundary + "--\r\n";
            out.headers.append(QPair<QByteArray, QByteArray>("Content-Type", "multipart/form-data; boundary=" + boundary));
        } else if (body == "form") {
            for (const auto& [name, text] : arguments) {
                if (!out.before.isEmpty()) out.before.append('&');
                out.before += QUrl::toPercentEncoding(name) + '=' + QUrl::toPercentEncoding(text);
            }
            out.headers.append(QPair<QByteArray, QByteArray>("Content-Type", "application/x-www-form-urlencoded"));
        } else if (body == "json" || body == "xml") {
            if (s["dataSecret"].toBool() && !secrets_.contains(dataSecretKey())) {
                error = {"credential_setup_required", "The stored request body of this destination is missing. Enter it in Settings and store the credentials again.", {}};
                return false;
            }
            auto data = s["dataSecret"].toBool() ? secrets_.value(dataSecretKey()) : s["data"].toString();
            // The Data text is JSON or XML with braces of its own: only these two names are replaced.
            auto encoded = values;
            encoded.encode = body == "json" ? RequestValues::Encode::Json : RequestValues::Encode::Xml;
            QString ignored;
            data.replace("{input}", renderRequest("{input}", encoded, &ignored), Qt::CaseInsensitive);
            data.replace("{filename}", renderRequest("{filename}", encoded), Qt::CaseInsensitive);
            out.before = data.toUtf8();
            out.headers.append(QPair<QByteArray, QByteArray>("Content-Type", body == "json" ? "application/json" : "application/xml"));
        } else if (body == "binary" && !contentType) {
            out.headers.append(QPair<QByteArray, QByteArray>("Content-Type", artifact.mime.isEmpty() || !controlFree(artifact.mime) ? QByteArray("application/octet-stream") : artifact.mime.toUtf8()));
        }
        return true;
    }
    Error refusal(const Exchange& r, const QString& stage, const ResponseValues& values) const {
        QJsonObject details{{"requestStage", stage}, {"httpStatus", r.status}};
        auto said = settings()["errorMessage"].toString().isEmpty() ? QString() : boundedMessage(scrub(renderResponse(settings()["errorMessage"].toString(), values)));
        const auto suffix = said.isEmpty() ? QString(".") : ": " + said;
        if (r.status == 401 || r.status == 403)
            return {"http_authentication_failed", QString("The server refused this destination's credentials (HTTP %1)%2 Check its header, parameter and argument values, then store them again.").arg(r.status).arg(suffix), details};
        if (r.status == 413) return {"http_too_large", QString("The server says the file is too large (HTTP 413)%1").arg(suffix), details};
        if (r.status == 429) return {"http_rate_limited", QString("The server is limiting requests (HTTP 429)%1 Wait before retrying.").arg(suffix), details};
        if (r.status >= 500) return {"http_server_error", QString("The server failed to handle the upload (HTTP %1)%2").arg(r.status).arg(suffix), details};
        return {"http_request_refused", QString("The server refused the upload (HTTP %1)%2").arg(r.status).arg(suffix), details};
    }
    // The upload request, following redirects by hand: credentials and the file go to
    // the configured host only.
    Exchange sendUpload(const Plan& plan, const Artifact& artifact, bool& answered, Error& error) {
        answered = false;
        QUrl url = plan.url;
        QByteArray verb = plan.verb;
        bool configured = true;     // still the configured request, body included
        bool withValues = true;     // configured headers go to the configured host only
        Exchange r;
        for (int hop = 0;; ++hop) {
            std::unique_ptr<Body> body;
            if (configured && plan.body) {
                body = std::make_unique<Body>(plan.before, plan.file ? artifact.path : QString(), artifact.bytes, plan.after);
                if (!body->open(QIODevice::ReadOnly)) { error = {"artifact_invalid", "Retained artifact is unavailable", {}}; return {}; }
            }
            const bool reports = configured && plan.file;
            const auto sent = [&](qint64 bytes) { progress("uploading", bytes - plan.before.size(), artifact.bytes); };
            r = client_.send(verb, url, withValues ? plan.headers : Headers{}, body.get(), reports ? std::function<void(qint64)>(sent) : std::function<void(qint64)>{});
            if (!QList<int>{301, 302, 303, 307, 308}.contains(r.status)) return r;
            const auto location = r.header("location");
            const auto target = url.resolved(QUrl(location));
            const QJsonObject details{{"requestStage", "redirect"}, {"httpStatus", r.status}};
            if (location.isEmpty() || !target.isValid() || target.host().isEmpty()) { error = {"http_redirect_refused", QString("The server answered with a redirect that names no address (HTTP %1).").arg(r.status), details}; return r; }
            if (hop >= maximumRedirects) { error = {"http_redirect_refused", "The server redirects too many times.", details}; return r; }
            if (!transportAllowed(target, allowInsecure()) || (url.scheme() == "https" && target.scheme() != "https")) {
                error = {"http_redirect_refused", "The server redirects to an unencrypted address; put.gd does not follow it.", details};
                return r;
            }
            const bool same = sameHost(url, target);
            if (r.status == 307 || r.status == 308) {
                // The server asks for the same request elsewhere: only on its own host.
                if (configured && !same) {
                    error = {"http_redirect_refused", "The server redirected the upload to another host (" + boundedMessage(target.host(), 96) +
                             "). put.gd does not send the file or this destination's credentials there. If that address is right, set it as the request URL.", details};
                    return r;
                }
            } else {
                // 301, 302 and 303: the reply to the upload is at another address, fetched
                // with a GET that carries no file.
                if (configured) answered = true;
                configured = false;
                verb = "GET";
            }
            if (!same) withValues = false;
            url = target;
        }
    }
    QJsonObject upload(Error& error, bool& uncertain) {
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        const auto publication = request_["publicationId"].toString();
        const auto reference = request_["objectReference"].toObject();
        const auto fileName = request_["candidate"].toString();
        if (reference["nonce"].toString().isEmpty() || fileName.isEmpty()) { error = {"http_protocol", "The upload request is incomplete", {}}; return {}; }
        progress("preparing", 0, artifact.bytes);
        Hashes whole;
        if (!hashFile(artifact.path, whole)) { error = cancelled.load() ? stoppedError() : Error{"artifact_invalid", "Retained artifact is unavailable", {}}; return {}; }
        if (whole.bytes != artifact.bytes || QString::fromLatin1(whole.sha256) != artifact.sha256) { error = {"artifact_changed", "The retained artifact changed since it was saved", {}}; return {}; }
        Plan request;
        if (!plan(artifact, fileName, request, error)) return {};
        if (cancelled.load()) { error = stoppedError(); return {}; }
        bool answered = false;
        const auto r = sendUpload(request, artifact, answered, error);
        if (error) {
            // A refused redirect after the upload was answered leaves its outcome open.
            uncertain = answered;
            return {{"status", answered ? "unknown" : "failed"}};
        }
        ResponseValues values{r.text(), r.headers, r.url.toString(QUrl::FullyEncoded), fileName, &secrets_, false};
        if (r.status == 0) {
            QJsonObject details{{"requestStage", "upload"}, {"transport", r.detail}};
            if (!r.reached && !answered) {
                // The whole request never left: nothing can be stored.
                if (r.stopped) { error = stoppedError(); return {{"status", "failed"}}; }
                error = {r.connected ? "http_upload_interrupted" : "http_connect_failed",
                         r.connected ? "The connection ended before the upload was sent (" + r.detail + "). Nothing was stored; retry when the server is reachable."
                                     : "The server could not be reached (" + r.detail + "). Check the request URL and the network.", details};
                return {{"status", "failed"}};
            }
            uncertain = true;
            error = {"http_outcome_unknown", "The upload was sent, but no reply arrived (" + r.detail + "). The file may be stored on the server. This uploader cannot check, so Retry sends it again and may leave a second copy.", details};
            return {{"status", "unknown"}};
        }
        if (r.status < 200 || r.status >= 300) {
            error = refusal(r, "upload", values);
            // A gateway that gave up, or a reply fetched after a redirect, does not say what the server stored.
            if (r.status == 408 || r.status == 502 || r.status == 504 || answered) {
                uncertain = true;
                error.code = "http_outcome_unknown";
                error.message = (answered ? QString("The server redirected the upload, and the address it named answered HTTP %1.").arg(r.status)
                                          : QString("The server gave up before answering (HTTP %1).").arg(r.status)) +
                                " The file may be stored on the server. This uploader cannot check, so Retry sends it again and may leave a second copy.";
                return {{"status", "unknown"}};
            }
            return {{"status", "failed"}};
        }
        progress("finalizing", artifact.bytes, artifact.bytes);
        const auto base = r.url;
        const auto address = [&](const QString& text, bool transportRule) -> QString {
            const auto trimmed = text.trimmed();
            if (trimmed.isEmpty() || trimmed.size() > 4096) return {};
            // A link is one unbroken address. Anything else, such as a JSON reply the
            // URL value did not pick apart, is not a link.
            for (const auto c : trimmed) if (c.isSpace() || c.unicode() < 0x21 || c.unicode() == 0x7f || QStringLiteral("\"<>{}|\\^`").contains(c)) return {};
            auto url = QUrl(trimmed, QUrl::TolerantMode);
            // A path from the server's root is completed with the address that answered.
            if (url.isRelative()) { if (!trimmed.startsWith('/')) return {}; url = base.resolved(url); }
            if (!url.isValid() || url.host().isEmpty() || !QStringList{"http", "https"}.contains(url.scheme())) return {};
            if (transportRule && !transportAllowed(url, allowInsecure())) return {};
            return url.toString(QUrl::FullyEncoded);
        };
        const auto pattern = settings()["responseUrl"].toString();
        const auto link = address(pattern.isEmpty() ? values.body : renderResponse(pattern, values), false);
        if (link.isEmpty()) {
            uncertain = true;
            error = {"http_response_unreadable", QString("The server accepted the upload (HTTP %1), but its reply has no link where this destination looks for one. "
                     "The file may be stored on the server. Check the URL value under Response, then Retry; that sends the file again.").arg(r.status),
                     {{"requestStage", "response"}, {"httpStatus", r.status}, {"replyBytes", double(r.body.size())}, {"contentType", boundedMessage(r.header("content-type"), 96)}}};
            QJsonObject out{{"status", "unknown"}};
            if (request_["includeReply"].toBool()) out["replyExcerpt"] = boundedMessage(scrub(values.body), 600);
            return out;
        }
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.directUrl = link;
        receipt.publicationId = publication;
        receipt.concurrencyToken = reference["nonce"].toString();
        receipt.objectReference = {{"schemaVersion", 1}, {"nonce", reference["nonce"]}, {"url", link}, {"sha256", artifact.sha256}, {"bytes", double(artifact.bytes)}};
        if (const auto thumbnail = address(renderResponse(settings()["thumbnailUrl"].toString(), values), false); !thumbnail.isEmpty()) receipt.objectReference["thumbnailUrl"] = thumbnail;
        // A deletion URL that breaks the transport rule is not kept: the upload stands, deletion is unavailable.
        if (const auto deletion = address(renderResponse(settings()["deletionUrl"].toString(), values), true); !deletion.isEmpty()) {
            // A stored value in it, such as a token, is recorded by name only: the
            // receipt keeps {secret:key}, filled in when the deletion is sent.
            if (secretReferences(settings()["deletionUrl"].toString()).isEmpty()) receipt.objectReference["deletionUrl"] = deletion;
            else {
                auto symbolic = values;
                symbolic.secrets = nullptr;
                symbolic.keepSecrets = true;
                receipt.objectReference["deletionUrl"] = renderResponse(settings()["deletionUrl"].toString(), symbolic).trimmed();
            }
        }
        return {{"status", "uploaded"}, {"receipt", receipt.toJson()}};
    }
    // One GET of the deletion URL the upload returned, after its intent is journaled.
    QJsonObject remove(Error& error, bool& uncertain) {
        const auto expected = StorageReceipt::fromJson(request_["receipt"].toObject());
        auto progress = request_["progress"].toObject();
        const auto target = expected.toJson();
        if (progress.contains("target") && progress["target"].toObject() != target) return {{"status", "changed"}, {"progress", progress}};
        progress["schemaVersion"] = 1;
        progress["target"] = target;
        const auto phase = progress["phase"].toString();
        if (phase == "done") return {{"status", "removed"}, {"progress", progress}};
        // The recorded deletion URL may name stored values; they are filled in only here.
        const auto recorded = expected.objectReference["deletionUrl"].toString();
        ResponseValues stored;
        stored.secrets = &secrets_;
        for (const auto& key : secretReferences(recorded))
            if (!secrets_.contains(key)) {
                error = {"credential_setup_required", "Deleting this upload needs a stored value of its destination (" + secretLabel(key) + "). Unlock or store the destination's credentials, then try again.", {}};
                return {{"status", "failed"}, {"progress", progress}};
            }
        const QUrl url(secretReferences(recorded).isEmpty() ? recorded : renderResponse(recorded, stored), QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() || !transportAllowed(url, allowInsecure())) {
            error = {"http_delete_unsupported", "This upload came back without a deletion URL, so put.gd cannot delete it from the server.", {}};
            return {{"status", "unsupported_safe_removal"}, {"progress", progress}};
        }
        if (cancelled.load()) { error = stoppedError(); return {{"status", "failed"}, {"progress", progress}}; }
        progress["phase"] = "delete_intent";
        if (auto e = checkpoint(progress)) { error = e; progress["phase"] = phase; return {{"status", "failed"}, {"progress", progress}}; }
        const auto finish = [&](const QString& status) {
            progress["phase"] = "done";
            if (auto e = checkpoint(progress)) { error = e; return QJsonObject{{"status", "unknown"}, {"progress", progress}}; }
            return QJsonObject{{"status", status}, {"progress", progress}};
        };
        // What the link serves now, to tell afterwards whether the file really went.
        const QUrl link(expected.directUrl, QUrl::StrictMode);
        const auto before = link.isValid() && !link.host().isEmpty() ? client_.get(link, true) : Exchange{};
        if (cancelled.load()) { error = stoppedError(); return {{"status", "failed"}, {"progress", progress}}; }
        QString refused;
        auto r = client_.get(url, allowInsecure(), &refused);
        QJsonObject details{{"requestStage", "delete"}, {"httpStatus", r.status}};
        if (!refused.isEmpty()) { error = {"http_delete_refused", "The deletion URL could not be used: " + refused + ".", details}; return {{"status", "failed"}, {"progress", progress}}; }
        if (r.status >= 200 && r.status < 300) {
            if (const auto confirm = confirmationForm(r.text(), r.url); confirm.found && transportAllowed(confirm.action, allowInsecure())) {
                Body body(confirm.body, QString(), 0, {});
                body.open(QIODevice::ReadOnly);
                r = client_.send("POST", confirm.action, {{QByteArray("Content-Type"), QByteArray("application/x-www-form-urlencoded")}}, &body);
                details["confirmed"] = true;
                details["httpStatus"] = r.status;
            }
        }
        // The answer to a confirmation is often a redirect back to the site.
        if (r.status >= 200 && r.status < 400) {
            // A server can answer politely and delete nothing. If the link served the
            // file before and serves the very same bytes now, it is still there.
            if (before.status >= 200 && before.status < 300) {
                const auto after = client_.get(link, true);
                if (after.status >= 200 && after.status < 300 && after.body == before.body) {
                    error = {"http_delete_unconfirmed", QString("The server answered the deletion request (HTTP %1), but the link still serves the file. Nothing is confirmed deleted. "
                             "This server may want the deletion confirmed on its own page, or may serve a cached copy for a while; try again later or delete it on the server.").arg(r.status), details};
                    return {{"status", "failed"}, {"progress", progress}};
                }
            }
            return finish("removed");
        }
        // Gone. It was this deletion when an earlier attempt's reply was lost, or when
        // the server closed the connection on this request and the transport, which
        // repeats a request that has no body, then found nothing left to delete.
        if (r.status == 404 || r.status == 410) return finish(phase == "delete_intent" || r.sends > 1 ? "removed" : "missing");
        if (r.status == 0) {
            details["transport"] = r.detail;
            uncertain = r.reached;
            error = {r.reached ? "http_outcome_unknown" : "http_connect_failed",
                     r.reached ? "The deletion request was sent, but no reply arrived (" + r.detail + "). Retry asks the server again."
                               : "The server could not be reached (" + r.detail + "). Nothing was deleted.", details};
            return {{"status", r.reached ? "unknown" : "failed"}, {"progress", progress}};
        }
        error = {"http_delete_refused", QString("The server refused the deletion (HTTP %1).").arg(r.status), details};
        return {{"status", "failed"}, {"progress", progress}};
    }
    // A synthetic file through the production upload path, the returned link fetched, and
    // the file deleted when the reply carried a deletion URL.
    QJsonObject testDestination(Error& error) {
        const auto synthetic = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const bool image = !acceptsKind(settings(), "text");
        const auto bytes = image ? syntheticPng() : QByteArray("put.gd connection test " + synthetic.toUtf8() + "\n");
        QJsonObject out{{"kind", image ? "image" : "text"}};
        QTemporaryDir dir;
        const auto path = dir.filePath(image ? "synthetic.png" : "synthetic.txt");
        QFile file(path);
        if (!dir.isValid() || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { error = {"synthetic_artifact_failed", "Could not create the private synthetic file", {}}; return out; }
        file.close();
        Artifact artifact;
        artifact.id = "synthetic-" + synthetic;
        artifact.path = path;
        artifact.name = "putgd-test-" + synthetic + (image ? ".png" : ".txt");
        artifact.mime = image ? "image/png" : "text/plain";
        artifact.category = image ? "images" : "text";
        artifact.bytes = bytes.size();
        artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        request_["candidate"] = artifact.name;
        request_["artifact"] = artifact.toJson();
        request_["publicationId"] = synthetic;
        request_["objectReference"] = QJsonObject{{"schemaVersion", 1}, {"nonce", synthetic}};
        request_["includeReply"] = true;
        if (auto e = checkpoint({{"schemaVersion", 1}, {"phase", "upload_intent"}, {"name", artifact.name}})) { error = e; return out; }
        bool uncertain = false;
        const auto uploaded = upload(error, uncertain);
        out["uploadStatus"] = uploaded["status"].toString(error ? "failed" : "unknown");
        if (uploaded.contains("replyExcerpt")) out["replyExcerpt"] = uploaded["replyExcerpt"];
        if (error || uploaded["status"] != "uploaded") return out;
        const auto receipt = StorageReceipt::fromJson(uploaded["receipt"].toObject());
        out["link"] = receipt.directUrl;
        out["deletionOffered"] = receipt.objectReference.contains("deletionUrl");
        if (auto e = checkpoint({{"schemaVersion", 1}, {"phase", "uploaded"}, {"receipt", receipt.toJson()}})) { error = e; return out; }
        QString refused;
        const auto fetched = client_.get(QUrl(receipt.directUrl, QUrl::StrictMode), true, &refused);
        out["linkStatus"] = fetched.status;
        out["publicReachable"] = fetched.status >= 200 && fetched.status < 300;
        out["linkReturnsFile"] = fetched.body == bytes;
        Error cleanup;
        if (receipt.objectReference.contains("deletionUrl")) {
            request_["receipt"] = receipt.toJson();
            request_["progress"] = QJsonObject{};
            bool cleanupUncertain = false;
            const auto removed = remove(cleanup, cleanupUncertain);
            out["cleanupStatus"] = removed["status"];
        } else out["cleanupStatus"] = "unsupported";
        if (cleanup) error = cleanup;
        else if (!out["publicReachable"].toBool())
            error = {"public_url_unreachable", fetched.status ? QString("The upload worked, but the returned link did not open (HTTP %1). Check the URL value under Response.").arg(fetched.status)
                                                              : "The upload worked, but the returned link did not open (" + (refused.isEmpty() ? fetched.detail : refused) + "). Check the URL value under Response.",
                     {{"httpStatus", fetched.status}}};
        return out;
    }
};
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version-json") == 0) { std::printf("%s\n", putgd::buildManifestJson()); return 0; }
    if (argc != 2 || std::strcmp(argv[1], "--stdio") != 0) return 2;
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("putgd-http-worker");
    putgd::useHostCaBundle();
    bool eof = false;
    auto line = putgd::http::readLine(eof);
    auto request = QJsonDocument::fromJson(line).object();
    line.fill('\0');
    if (request["schemaVersion"].toInt() != 1 || !request["method"].isString()) {
        putgd::http::emitLine({{"ok", false}, {"error", QJsonObject{{"code", "http_protocol"}, {"message", "Invalid worker request"}}}, {"uncertain", false}});
        return 0;
    }
    std::thread control(putgd::http::readControl);
    control.detach();
    putgd::Error error;
    bool uncertain = false;
    QJsonObject result;
    {
        putgd::http::Worker worker(request);
        request = {};
        result = worker.run(error, uncertain);
    }
    if (error)
        putgd::http::emitLine({{"ok", false}, {"error", QJsonObject{{"code", error.code}, {"message", error.message}, {"details", error.details}}},
                               {"uncertain", uncertain}, {"result", result}});
    else putgd::http::emitLine({{"ok", true}, {"result", result}});
    std::_Exit(0);
}
