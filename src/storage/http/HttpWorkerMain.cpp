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
#include "storage/worker/WorkerStdio.h"
#include "core/CaBundle.h"
#include "core/StorageProvider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <atomic>
#include <memory>
namespace putgd::http {
namespace {
constexpr qint64 maximumLine = 1024 * 1024;
constexpr qsizetype maximumReplyBytes = 4 * 1024 * 1024;
constexpr int maximumRedirects = 5;
worker::Stdio stdio{maximumLine, "http", "request"};
auto& cancelled = stdio.cancelled;
Error checkpoint(const QJsonObject& journal) { return stdio.checkpoint(journal); }
Error stoppedError() { return {"cancelled", "Upload cancelled", {}}; }

// The request body: bytes before the file, the file, bytes after it; or nothing at all,
// for a request that has no body. It can be read exactly once. Qt Network silently sends
// a request again when the server closes the connection before replying; a body it
// cannot rewind turns that into an error, so no upload is ever repeated behind put.gd's
// back. That is why a request without a body is sent with an empty one.
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
    // Never zero. Newer Qt drops an upload device of size zero from a request whose
    // method expects no body, and with it the one thing that keeps the request from
    // being sent again. What is sent is the Content-Length set on the request: for an
    // empty body that is 0, and Qt then reads nothing from here.
    qint64 size() const override { return qMax<qint64>(total(), 1); }
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
    bool cut = false;          // a status line arrived, but not the whole reply
    qint64 received = 0;       // body bytes that arrived, counted past the ones kept
    QString detail;            // bounded transport description, never request data
    QString blocked;           // why the address was not fetched at all; nothing was sent
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
    // Whether a reply may send this client to the machine it runs on: only when the
    // destination's own request URL is there.
    void setOrigin(const QUrl& origin) { origin_ = origin; }
    // Where a host leads: its spelling can say "this machine", and so can what it
    // resolves to ("ip6-localhost", the machine's own name, a public name that answers
    // 127.0.0.1). Looked up once per host, within the connect timeout. Qt keeps a lookup
    // for a minute, so the request that follows normally goes to the addresses judged
    // here; a resolver that changes its answer in between is not excluded by this.
    enum class Where { Elsewhere, Here, Unresolved };
    Where where(const QString& host) {
        if (const auto known = where_.constFind(host); known != where_.cend()) return *known;
        QStringList resolved;
        if (QHostAddress literal; !literal.setAddress(host) && !loopbackAddress(host)) {
            QEventLoop loop;
            QHostInfo info;
            bool done = false;
            const int lookup = QHostInfo::lookupHost(host, &loop, [&](const QHostInfo& result) { info = result; done = true; loop.quit(); });
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            deadline.start(connectMs_);
            if (!done) loop.exec();
            if (!done) { QHostInfo::abortHostLookup(lookup); return Where::Unresolved; }
            if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) return where_[host] = Where::Unresolved;
            for (const auto& address : info.addresses()) resolved.append(address.toString());
        }
        QStringList own;
        for (const auto& address : QNetworkInterface::allAddresses()) own.append(address.toString());
        return where_[host] = leadsToThisMachine(host, resolved, own) ? Where::Here : Where::Elsewhere;
    }
    // Why an address that a reply named is not fetched, or empty. It follows the
    // destination's transport rule (plain HTTP only where the destination allows it), it
    // carries no user name, and it leads to this machine only when the destination's own
    // request URL does. `unresolved`: its host could not be looked up, which may pass.
    QString fetchProblem(const QUrl& url, bool allowInsecure, bool* unresolved = nullptr) {
        if (unresolved) *unresolved = false;
        if (!url.isValid() || url.host().isEmpty() || !QStringList{"http", "https"}.contains(url.scheme())) return QStringLiteral("it is not an address put.gd can open");
        if (!transportAllowed(url, allowInsecure)) return QStringLiteral("it uses plain HTTP, which this destination does not allow");
        if (const auto problem = returnedAddressProblem(url, origin_); !problem.isEmpty()) return problem;
        const auto leads = where(url.host());
        if (leads == Where::Unresolved) { if (unresolved) *unresolved = true; return QStringLiteral("its host could not be looked up"); }
        if (leads == Where::Here && where(origin_.host()) != Where::Here) return QStringLiteral("it points at this computer");
        return {};
    }
    // One request, no redirects followed. `sent` reports body bytes the transport has read.
    Exchange send(const QByteArray& verb, const QUrl& url, const Headers& headers, Body* body = nullptr, const std::function<void(qint64)>& sent = {}) {
        // A request with an empty body. Qt writes its headers as soon as the connection is
        // up and says nothing about it: no "request sent", no upload progress. Over TLS
        // the handshake tells that the connection is up. Over plain HTTP nothing does
        // until the reply, and a server that cannot be reached looks like one that is slow
        // to answer. So that the destination's connect timeout still holds there, the
        // server is first asked for a connection of its own, within that timeout, and
        // only when it gave one is the request made at all: nothing is under way while
        // the answer is awaited, so "could not connect" means that nothing was sent.
        // Through a proxy there is no server to ask (the proxy takes the request, and
        // may refuse a bare connection): such a request is not checked, has no connect
        // deadline, and counts as sent once it is stopped or times out.
        const bool empty = body && body->total() == 0;
        const bool blind = empty && url.scheme() == "http";
        const auto proxies = blind ? QNetworkProxyFactory::proxyForQuery(QNetworkProxyQuery(url)) : QList<QNetworkProxy>{};
        const bool unseen = blind && !proxies.isEmpty() && proxies.first().type() != QNetworkProxy::NoProxy;
        Exchange out;
        out.url = url;
        if (blind && !unseen) {
            QTcpSocket probe;
            probe.setProxy(QNetworkProxy::NoProxy);
            QEventLoop wait;
            QTimer tick;
            QElapsedTimer since;
            since.start();
            QObject::connect(&probe, &QTcpSocket::connected, &wait, &QEventLoop::quit);
            QObject::connect(&probe, &QTcpSocket::errorOccurred, &wait, &QEventLoop::quit);
            QObject::connect(&tick, &QTimer::timeout, &wait, [&] {
                if (cancelled.load()) out.stopped = true;
                else if (since.elapsed() > connectMs_) out.deadline = true;
                if (out.stopped || out.deadline) wait.quit();
            });
            tick.start(50);
            probe.connectToHost(url.host(), quint16(url.port(80)));
            if (probe.state() != QAbstractSocket::ConnectedState && probe.error() == QAbstractSocket::UnknownSocketError) wait.exec();
            tick.stop();
            if (probe.state() != QAbstractSocket::ConnectedState) {
                // Nothing was sent: the request does not exist yet.
                out.detail = out.stopped ? QStringLiteral("cancelled") : out.deadline ? QStringLiteral("could not connect within the connect timeout")
                           : probe.error() == QAbstractSocket::ConnectionRefusedError ? QStringLiteral("connection refused")
                           : probe.error() == QAbstractSocket::HostNotFoundError ? QStringLiteral("host not found")
                           : probe.error() == QAbstractSocket::NetworkError ? QStringLiteral("the network is unavailable")
                           : probe.error() == QAbstractSocket::SocketTimeoutError ? QStringLiteral("timed out") : QStringLiteral("network error %1").arg(int(probe.error()));
                probe.abort();
                return out;
            }
            probe.abort();
            // The server takes connections: from here on only the request deadline counts.
            out.connected = true;
        }
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        // HTTP/1.1: one request on one connection, so "was it sent" has a plain answer.
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setRawHeader("User-Agent", QByteArray("put.gd/") + buildVersion());
        // Replies are asked for as they are. Qt would otherwise ask for a compressed
        // reply, unpack it and take its Content-Length away, and the bytes that arrived
        // could no longer be held against the length the server declared.
        request.setRawHeader("Accept-Encoding", "identity");
        for (const auto& [name, value] : headers) request.setRawHeader(name, value);
        QNetworkReply* reply = nullptr;
        if (body) {
            request.setHeader(QNetworkRequest::ContentLengthHeader, body->total());
            request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
            reply = nam_.sendCustomRequest(request, verb, body);
        } else reply = nam_.sendCustomRequest(request, verb);
        QEventLoop loop;
        bool requestSent = false;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::requestSent, &loop, [&] { requestSent = true; out.connected = true; ++out.sends; });
        QObject::connect(reply, &QNetworkReply::encrypted, &loop, [&] { out.connected = true; });
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] { out.connected = true; });
        QObject::connect(reply, &QNetworkReply::uploadProgress, &loop, [&](qint64 bytes, qint64) {
            if (bytes > 0) out.connected = true;
            if (sent) sent(bytes);
        });
        const auto take = [&](const QByteArray& chunk) {
            out.received += chunk.size();
            if (out.body.size() < maximumReplyBytes) out.body.append(chunk.left(maximumReplyBytes - out.body.size()));
        };
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] { take(reply->readAll()); });
        QTimer poll;
        poll.setInterval(50);
        QElapsedTimer clock;
        clock.start();
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (out.stopped || out.deadline) return;
            if (cancelled.load()) { out.stopped = true; reply->abort(); }
            else if (clock.elapsed() > requestMs_ || (!out.connected && !unseen && clock.elapsed() > connectMs_)) { out.deadline = true; reply->abort(); }
        });
        poll.start();
        if (!reply->isFinished()) loop.exec();
        poll.stop();
        out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        take(reply->readAll());
        out.headers = reply->rawHeaderPairs();
        // Whether the server may have the whole request. With a body: every byte of it was
        // handed to the transport. Without one Qt says when it was sent. With an empty one
        // it does not, so the request counts as sent when a reply came, when the connection
        // closed under it or Qt wanted to send it again, and when it was stopped or timed
        // out after the connection was up (the TLS handshake, or the connection check
        // above) or could not be seen (a proxy): an outcome is reported as unknown sooner
        // than as "nothing was stored".
        const auto ended = reply->error();
        if (unseen && (out.stopped || out.deadline)) out.connected = true;
        if (empty) out.reached = out.status != 0 || ended == QNetworkReply::RemoteHostClosedError || ended == QNetworkReply::ContentReSendError || ((out.stopped || out.deadline) && out.connected);
        else out.reached = body ? body->consumed() : requestSent;
        if (out.status == 0 || out.stopped || out.deadline) {
            // An aborted reply has no usable status even when headers had arrived.
            if (out.stopped || out.deadline) out.status = 0;
            out.detail = out.stopped ? QStringLiteral("cancelled") : out.deadline ? (out.connected ? QStringLiteral("no reply within the request timeout") : QStringLiteral("could not connect within the connect timeout"))
                                                                                  : transportReason(reply->error());
        } else {
            // Whether the whole reply arrived. First by what arrived: a body shorter than
            // the length its own headers declared is not all of it, whatever the transport
            // has or has not reported by now. What Qt reports for a connection that ends
            // under a reply depends on where its own bookkeeping stood when it noticed.
            // Still catching up on the request it had just written, it takes the close for
            // a dead connection, tries to send the request again, and reports that as
            // ContentReSendError, after it has handed over the status line, the headers
            // and the bytes that did arrive (#1393). So, second: for a status that is not
            // itself an error, any error at all means the reply did not end as a reply
            // should. That is also the only word there is on a chunked reply, whose last
            // chunk Qt does not show; an HTTP error status has an error code of its own.
            const auto failure = reply->error();
            const bool ofStatus = out.status >= 400 && int(failure) > 200 && failure != QNetworkReply::ContentReSendError && failure != QNetworkReply::ProtocolUnknownError &&
                                  failure != QNetworkReply::ProtocolFailure;
            if (const auto shortfall = incompleteReply(out.status, verb, out.headers, out.received); !shortfall.isEmpty()) {
                out.cut = true;
                out.detail = shortfall;
            } else if (failure != QNetworkReply::NoError && !ofStatus) {
                out.cut = true;
                out.detail = failure == QNetworkReply::ContentReSendError ? QStringLiteral("the connection closed before the reply was complete") : transportReason(failure);
            }
        }
        reply->deleteLater();
        return out;
    }
    // A GET that follows redirects and sends nothing but the address: the returned link
    // and the deletion URL. Every address on the way is one a reply named and is judged
    // by fetchProblem. `stay`: the address carries a stored value, which goes to its own
    // host or nowhere, so no redirect to another host is followed either: the server
    // could have written the value into the address it redirects to.
    Exchange get(QUrl url, bool allowInsecure, QString* refused = nullptr, bool stay = false) {
        Exchange out;
        out.url = url;
        bool unresolved = false;
        if (const auto problem = fetchProblem(url, allowInsecure, &unresolved); !problem.isEmpty()) {
            if (unresolved) out.detail = QStringLiteral("host not found");
            else out.blocked = problem;
            return out;
        }
        const auto first = url;
        for (int hop = 0; hop <= maximumRedirects; ++hop) {
            out = send("GET", url, {});
            if (!QList<int>{301, 302, 303, 307, 308}.contains(out.status)) return out;
            const auto target = url.resolved(QUrl(out.header("location")));
            if (stay && target.isValid() && !sameHost(first, target)) {
                if (refused) *refused = QStringLiteral("it redirects to another host, where put.gd does not send this destination's stored value");
                return out;
            }
            if (!target.isValid() || (url.scheme() == "https" && target.scheme() != "https") || !fetchProblem(target, allowInsecure).isEmpty()) {
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
    QUrl origin_;
    QHash<QString, Where> where_;
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
        origin_ = requestOrigin(destination_.settings);
        client_.setOrigin(origin_);
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
    QUrl origin_;   // scheme, host and port of the request URL: the only place stored values go
    QJsonObject settings() const { return destination_.settings; }
    bool allowInsecure() const { return settings()["allowInsecureHttp"].toBool(); }

    void progress(const QString& phase, qint64 sent, qint64 total) { stdio.progress(phase, sent, total); }
    // Server text made fit to show: one line, no credential of this destination in any
    // spelling, at most `maximum` characters. In that order. A value echoed around a
    // control character reads as the value once the character is dropped, so the line is
    // made first; and the text is cut last, so a value is never cut in half and shown.
    // Only the start of a long text is cleaned, and enough of it that the place where it
    // was cut is out of sight.
    QString shown(const QString& text, int maximum = 300) const {
        for (qsizetype window = 64 * 1024;; window *= 4) {
            const auto cleaned = scrubStoredValues(singleLine(text.left(window)), secrets_);
            if (window >= text.size() || cleaned.size() > maximum) return boundedMessage(cleaned, maximum);
        }
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
    // What the server said about a refusal, where the destination names it: ": text", or ".".
    // The coding of a reply's body when the server compressed it although it was asked not
    // to, or empty. The worker does not unpack: such a body is bytes, not text to read.
    static QString compressed(const Headers& headers) {
        for (const auto& [name, value] : headers)
            if (name.compare("content-encoding", Qt::CaseInsensitive) == 0)
                if (const auto coding = QString::fromLatin1(value).trimmed().toLower(); !coding.isEmpty() && coding != "identity") return coding;
        return {};
    }
    QString said(const ResponseValues& values) const {
        const auto text = settings()["errorMessage"].toString().isEmpty() || !compressed(values.headers).isEmpty() ? QString()
                        : shown(renderResponse(settings()["errorMessage"].toString(), values));
        return text.isEmpty() ? QStringLiteral(".") : ": " + text;
    }
    Error refusal(const Exchange& r, const QString& stage, const ResponseValues& values) const {
        QJsonObject details{{"requestStage", stage}, {"httpStatus", r.status}};
        const auto suffix = said(values);
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
            if (configured) {
                // The configured request always goes out through a Body, an empty one when
                // the request has no body of its own (its text travels in the address).
                // Without one, Qt repeats the request by itself when the connection closes
                // before the reply, and a server that had stored the file stores it twice.
                body = plan.body ? std::make_unique<Body>(plan.before, plan.file ? artifact.path : QString(), artifact.bytes, plan.after)
                                 : std::make_unique<Body>(QByteArray(), QString(), 0, QByteArray());
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
            // Asked with plain HTTP let through: the transport rule was applied just above.
            if (const auto problem = client_.fetchProblem(target, true); !problem.isEmpty()) {
                error = {"http_redirect_refused", "The server redirects to an address put.gd does not follow: " + problem + ".", details};
                return r;
            }
            const bool same = sameHost(url, target);
            if (r.status == 307 || r.status == 308) {
                // The server asks for the same request elsewhere: only on its own host.
                if (configured && !same) {
                    error = {"http_redirect_refused", "The server redirected the upload to another host (" + shown(target.host(), 96) +
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
        // No stored value is handed to what reads the reply: the link, its thumbnail and
        // a server's message are recorded and shown. {responseurl} is where a redirect
        // led; with no redirect it would be the request's own address, secret parameters
        // and all, which is never a link.
        ResponseValues values{r.text(), r.headers, answered ? r.url.toString(QUrl::FullyEncoded) : QString(), fileName, nullptr, false};
        static const QString resend = QStringLiteral(" The file may be stored on the server. This uploader cannot check, so Retry sends it again and may leave a second copy.");
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
            error = {"http_outcome_unknown", "The upload was sent, but no reply arrived (" + r.detail + ")." + resend, details};
            return {{"status", "unknown"}};
        }
        if (r.status < 200 || r.status >= 300) {
            error = refusal(r, "upload", values);
            // A gateway that gave up, a server that failed once it had the whole request,
            // or a reply fetched after a redirect does not say what was stored. Only a
            // 4xx, or a failure before the request was complete, is a refusal.
            const bool gaveUp = r.status == 408 || r.status == 502 || r.status == 504;
            const bool failedAfter = r.status >= 500 && r.reached;
            if (gaveUp || failedAfter || answered) {
                uncertain = true;
                error.code = "http_outcome_unknown";
                const auto words = said(values);
                error.message = (answered ? QString("The server redirected the upload, and the address it named answered HTTP %1").arg(r.status)
                                 : gaveUp ? QString("The server gave up before answering (HTTP %1)").arg(r.status)
                                          : QString("The server failed after the upload was sent (HTTP %1)").arg(r.status)) + words + resend;
                return {{"status", "unknown"}};
            }
            return {{"status", "failed"}};
        }
        if (r.cut) {
            // Whatever its first bytes look like, a reply that ended early is not a reply.
            uncertain = true;
            error = {"http_outcome_unknown", QString("The server's reply was cut off before it was complete (HTTP %1; %2).").arg(r.status).arg(r.detail) + resend,
                     {{"requestStage", "response"}, {"httpStatus", r.status}, {"transport", r.detail}}};
            return {{"status", "unknown"}};
        }
        progress("finalizing", artifact.bytes, artifact.bytes);
        const auto base = r.url;
        // One address out of reply text. `why` says when it was one that put.gd does not use.
        const auto address = [&](const QString& text, bool transportRule, QString* why = nullptr) -> QString {
            const auto trimmed = text.trimmed();
            if (trimmed.isEmpty() || trimmed.size() > 4096) return {};
            // A link is one unbroken address. Anything else, such as a JSON reply the
            // URL value did not pick apart, is not a link.
            for (const auto c : trimmed) if (c.isSpace() || c.unicode() < 0x21 || c.unicode() == 0x7f || QStringLiteral("\"<>{}|\\^`").contains(c)) return {};
            auto url = QUrl(trimmed, QUrl::TolerantMode);
            // A path from the server's root is completed with the address that answered.
            if (url.isRelative()) { if (!trimmed.startsWith('/')) return {}; url = base.resolved(url); }
            if (!url.isValid() || url.host().isEmpty() || !QStringList{"http", "https"}.contains(url.scheme())) return {};
            // The reply does not get to send put.gd, or whoever opens the link, somewhere
            // other than it shows, or to a service on this machine.
            if (const auto problem = returnedAddressProblem(url, origin_); !problem.isEmpty()) { if (why) *why = problem; return {}; }
            if (transportRule && !transportAllowed(url, allowInsecure())) return {};
            return url.toString(QUrl::FullyEncoded);
        };
        const auto pattern = settings()["responseUrl"].toString();
        QString why;
        const auto link = address(pattern.isEmpty() ? values.body : renderResponse(pattern, values), false, &why);
        if (link.isEmpty()) {
            uncertain = true;
            // A server that compresses its reply although it was asked not to: the reply
            // arrived whole and cannot be read, which is said as what it is.
            const auto coding = compressed(r.headers);
            if (!coding.isEmpty() && why.isEmpty())
                error = {"http_response_unreadable", QString("The server accepted the upload (HTTP %1), but it compressed its reply (%2) although put.gd asks for replies as they are, and put.gd does not unpack them. "
                         "The file may be stored on the server; Retry sends it again. Turn off compression for this address on the server, or put the link in a reply header.").arg(r.status).arg(shown(coding, 32)),
                         {{"requestStage", "response"}, {"httpStatus", r.status}, {"replyBytes", double(r.body.size())}, {"contentEncoding", shown(coding, 32)}}};
            else
                error = {"http_response_unreadable", QString("The server accepted the upload (HTTP %1), but ").arg(r.status) +
                         (why.isEmpty() ? QStringLiteral("its reply has no link where this destination looks for one.") : "the link in its reply cannot be used: " + why + ".") +
                         " The file may be stored on the server. Check the URL value under Response, then Retry; that sends the file again.",
                         {{"requestStage", "response"}, {"httpStatus", r.status}, {"replyBytes", double(r.body.size())}, {"contentType", shown(r.header("content-type"), 96)}}};
            QJsonObject out{{"status", "unknown"}};
            if (request_["includeReply"].toBool() && coding.isEmpty()) out["replyExcerpt"] = shown(values.body, 600);
            return out;
        }
        // A server can repeat what it was sent inside the link it hands back. Such a
        // link is not recorded, copied or shown: it would carry the credential wherever
        // it goes. Sending the file again ends the same way, and the message says so.
        if (const auto key = storedValueIn(link, secrets_); !key.isEmpty()) {
            uncertain = true;
            const bool row = !key.startsWith(extraSecretsList() + '/') && key != dataSecretKey();
            error = {"http_link_holds_secret", QString("The server accepted the upload (HTTP %1), but the link it returned contains a credential of this destination (%2). "
                     "put.gd does not record or copy such a link. The file is probably stored on the server, and Retry would send it again with the same result. "
                     "If that value is not a credential, %3; otherwise change what the server puts in its links.").arg(r.status).arg(secretLabel(key))
                     .arg(row ? QStringLiteral("clear Secret on its row") : key == dataSecretKey() ? QStringLiteral("take it out of the request body") : QStringLiteral("write it into the address itself and remove it from Other secret values")),
                     {{"requestStage", "response"}, {"httpStatus", r.status}, {"storedValue", secretLabel(key)}}};
            return {{"status", "unknown"}};
        }
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.directUrl = link;
        receipt.publicationId = publication;
        receipt.concurrencyToken = reference["nonce"].toString();
        receipt.objectReference = {{"schemaVersion", 1}, {"nonce", reference["nonce"]}, {"url", link}, {"sha256", artifact.sha256}, {"bytes", double(artifact.bytes)}};
        // A thumbnail that repeats a stored value is left out.
        if (const auto thumbnail = address(renderResponse(settings()["thumbnailUrl"].toString(), values), false); !thumbnail.isEmpty() && storedValueIn(thumbnail, secrets_).isEmpty())
            receipt.objectReference["thumbnailUrl"] = thumbnail;
        // Why a deletion URL that came back was not kept is recorded in its place, so
        // History can say that instead of "none came back".
        const auto deletion = deletionRecord(values, address);
        if (!deletion.recorded.isEmpty()) receipt.objectReference["deletionUrl"] = deletion.recorded;
        else if (!deletion.leftOut.isEmpty()) receipt.objectReference["deletionUrlLeftOut"] = deletion.leftOut;
        return {{"status", "uploaded"}, {"receipt", receipt.toJson()}};
    }
    // The deletion URL as the receipt keeps it: an address with no credential in it, or a
    // template that names each one ("{secret:key}", filled in when the deletion is sent).
    // Not kept (`leftOut` says why) when it breaks the transport rule or is no address
    // ("unusable"), when a credential would have to be written out ("stored_value"), or
    // when one would be sent to another host than the request URL's ("other_host"): the
    // upload stands and deletion is unavailable for it.
    struct Deletion { QString recorded, leftOut; };
    template <typename Address>
    Deletion deletionRecord(const ResponseValues& values, const Address& address) const {
        const auto pattern = settings()["deletionUrl"].toString();
        if (pattern.isEmpty()) return {};
        // Judged as it would be sent.
        auto filledValues = values;
        filledValues.secrets = &secrets_;
        const auto rendered = renderResponse(pattern, filledValues).trimmed();
        if (rendered.isEmpty()) return {};
        // What the reply itself gave, with the stored values it names rendered as nothing.
        const auto fromReply = renderResponse(pattern, values).trimmed();
        const auto filled = address(rendered, true, nullptr);
        if (filled.isEmpty()) {
            // Text that is no address at all ("null", say) is no deletion URL either.
            const QUrl shape(rendered, QUrl::TolerantMode);
            const bool anAddress = rendered.size() <= 4096 && (rendered.startsWith('/') || (shape.isValid() && !shape.host().isEmpty() && QStringList{"http", "https"}.contains(shape.scheme())));
            return {{}, anAddress ? QStringLiteral("unusable") : QString()};
        }
        QString recorded;
        if (!secretReferences(pattern).isEmpty()) {
            // The template names its stored values. What it reads from the reply must not
            // repeat a credential beside them.
            if (!storedValueIn(fromReply, secrets_).isEmpty()) return {{}, QStringLiteral("stored_value")};
            auto symbolic = values;
            symbolic.keepSecrets = true;
            recorded = renderResponse(pattern, symbolic).trimmed();
        } else if (storedValueIn(filled, secrets_).isEmpty()) return {filled, {}};
        // The reply wrote a credential into the address: it is recorded by name where it
        // stands exactly as stored, and not at all otherwise.
        else recorded = nameStoredValues(filled, secrets_);
        if (recorded.isEmpty()) return {{}, QStringLiteral("stored_value")};
        // A stored value goes to the request's own host or nowhere.
        if (!sameHost(origin_, QUrl(filled))) return {{}, QStringLiteral("other_host")};
        return {recorded, {}};
    }
    // One GET of the deletion URL the upload returned, after its intent is journaled.
    QJsonObject remove(Error& error, bool& uncertain) {
        const auto expected = StorageReceipt::fromJson(request_["receipt"].toObject());
        auto progress = request_["progress"].toObject();
        const auto target = expected.toJson();
        if (progress.contains("target") && progress["target"].toObject() != target) return {{"status", "changed"}, {"progress", progress}};
        progress["schemaVersion"] = 1;
        progress["target"] = target;
        const auto previousPhase = progress.value("phase");
        const auto phase = previousPhase.toString();
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
        const bool named = !secretReferences(recorded).isEmpty();
        const QUrl url(named ? renderResponse(recorded, stored) : recorded, QUrl::StrictMode);
        if (recorded.isEmpty()) {
            error = {"http_delete_unsupported", "This upload came back without a deletion URL, so put.gd cannot delete it from the server.", {}};
            return {{"status", "unsupported_safe_removal"}, {"progress", progress}};
        }
        // The recorded address is one a reply named: it is judged again before anything
        // is sent, and a name that leads to this machine is found by looking it up.
        bool unresolved = false;
        const auto unusable = client_.fetchProblem(url, allowInsecure(), &unresolved);
        if (unresolved) {
            error = {"http_connect_failed", "The server could not be reached (host not found). Nothing was deleted.", {{"requestStage", "delete"}}};
            return {{"status", "failed"}, {"progress", progress}};
        }
        if (!unusable.isEmpty()) {
            error = {"http_delete_unsupported", "This upload's deletion URL cannot be used: " + unusable + ". put.gd cannot delete this upload from the server.", {}};
            return {{"status", "unsupported_safe_removal"}, {"progress", progress}};
        }
        // A stored value goes to the destination's own host or nowhere, whatever a record says.
        if (named && !sameHost(origin_, url)) {
            error = {"http_delete_unsupported", "This upload's deletion URL is on another host than its destination. put.gd does not send a stored value there, so it cannot delete this upload from the server.", {}};
            return {{"status", "unsupported_safe_removal"}, {"progress", progress}};
        }
        if (cancelled.load()) { error = stoppedError(); return {{"status", "failed"}, {"progress", progress}}; }
        progress["phase"] = "delete_intent";
        if (auto e = checkpoint(progress)) { error = e; progress["phase"] = previousPhase; return {{"status", "failed"}, {"progress", progress}}; }
        const auto finish = [&](const QString& status) {
            progress["phase"] = "done";
            if (auto e = checkpoint(progress)) { error = e; return QJsonObject{{"status", "unknown"}, {"progress", progress}}; }
            return QJsonObject{{"status", status}, {"progress", progress}};
        };
        // What the link serves now, to tell afterwards whether the file really went. The
        // link is fetched under the same rules as any address a reply named; one that
        // may not be fetched is simply not compared.
        const QUrl link(expected.directUrl, QUrl::StrictMode);
        const auto before = client_.get(link, allowInsecure());
        if (cancelled.load()) { error = stoppedError(); return {{"status", "failed"}, {"progress", progress}}; }
        QString refused;
        auto r = client_.get(url, allowInsecure(), &refused, named);
        QJsonObject details{{"requestStage", "delete"}, {"httpStatus", r.status}};
        if (!refused.isEmpty()) {
            // The server received the request and answered with a redirect that is not
            // followed: whether it deleted the file is not known. Retry asks again.
            uncertain = true;
            error = {"http_outcome_unknown", "The server answered the deletion request, but " + refused + ". It may have deleted the file; Retry asks again.", details};
            return {{"status", "unknown"}, {"progress", progress}};
        }
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
            // Two whole replies are the same file when their bytes are. A reply that was
            // cut short, before or after, is held against the other as far as it came:
            // a link that still answers with the bytes it had is not a deleted file, and
            // a check that was cut short proves nothing either way.
            if (before.status >= 200 && before.status < 300) {
                const auto after = client_.get(link, allowInsecure());
                const bool partial = before.cut || after.cut;
                const auto common = qMin(before.body.size(), after.body.size());
                const bool same = partial ? before.body.left(common) == after.body.left(common) : after.body == before.body;
                if (after.status >= 200 && after.status < 300 && same) {
                    // With nothing to compare (a check cut off before any of the file came)
                    // the link still answers, and that is all that is known.
                    error = partial && common == 0
                        ? Error{"http_delete_unconfirmed", QString("The server answered the deletion request (HTTP %1), and the link still answers, but the check of the link was cut off before any of the file came, "
                                "so it shows nothing either way. Nothing is confirmed deleted; try again later or delete it on the server.").arg(r.status), details}
                        : Error{"http_delete_unconfirmed", QString("The server answered the deletion request (HTTP %1), but the link still serves the file%2. Nothing is confirmed deleted. "
                                "This server may want the deletion confirmed on its own page, or may serve a cached copy for a while; try again later or delete it on the server.").arg(r.status)
                                .arg(partial ? QStringLiteral(", as far as the check of the link got before its reply was cut off") : QString()), details};
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
        const auto leftOut = receipt.objectReference["deletionUrlLeftOut"].toString();
        if (auto e = checkpoint({{"schemaVersion", 1}, {"phase", "uploaded"}, {"receipt", receipt.toJson()}})) { error = e; return out; }
        if (!leftOut.isEmpty()) out["deletionLeftOut"] = leftOut;
        // The link is opened as the destination's own rule allows: plain HTTP only where
        // the destination allows it, and this machine only when the destination is here.
        QString refused;
        const auto fetched = client_.get(QUrl(receipt.directUrl, QUrl::StrictMode), allowInsecure(), &refused);
        out["linkStatus"] = fetched.status;
        // A reply that was cut short is not the link opening.
        out["publicReachable"] = fetched.status >= 200 && fetched.status < 300 && !fetched.cut;
        out["linkReturnsFile"] = !fetched.cut && fetched.body == bytes;
        Error cleanup;
        if (receipt.objectReference.contains("deletionUrl")) {
            request_["receipt"] = receipt.toJson();
            request_["progress"] = QJsonObject{};
            bool cleanupUncertain = false;
            const auto removed = remove(cleanup, cleanupUncertain);
            out["cleanupStatus"] = removed["status"];
        } else out["cleanupStatus"] = "unsupported";
        if (cleanup) error = cleanup;
        else if (!fetched.blocked.isEmpty())
            error = {"public_url_unreachable", "The upload worked, but put.gd did not open the returned link: " + fetched.blocked + ". " +
                     (fetched.blocked.contains("plain HTTP") ? QStringLiteral("Turn on Allow plain HTTP for this destination, or have the server return an https link.") : QStringLiteral("Check the URL value under Response.")),
                     {{"httpStatus", 0}}};
        else if (fetched.cut && fetched.status >= 200 && fetched.status < 300)
            error = {"public_url_unreachable", QString("The upload worked, but the returned link's reply was cut off (HTTP %1; %2). Test again.").arg(fetched.status).arg(fetched.detail), {{"httpStatus", fetched.status}}};
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
    return putgd::http::stdio.run(argc, argv, "putgd-http-worker", putgd::buildManifestJson(), putgd::useHostCaBundle,
        [](QJsonObject& request, putgd::Error& error, bool& uncertain) {
            putgd::http::Worker worker(request);
            request = {};
            return worker.run(error, uncertain);
        });
}
