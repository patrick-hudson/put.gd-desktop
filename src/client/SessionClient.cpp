#include "core/ByteSize.h"
#include "SessionClient.h"
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <utility>
#ifdef Q_OS_LINUX
#include <QFile>
#include <QSocketNotifier>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#else
#include <QLocalSocket>
#endif

namespace putgd {
namespace {
using SessionProtocol::maximumRequest;
using SessionProtocol::maximumResponse;
using SessionProtocol::invalidResponse;
constexpr qsizetype maximumConcurrent = 64;
#ifdef Q_OS_LINUX
// One activation of the read notifier drains at most this much before it
// yields to the event loop; the notifier is level-triggered, so the rest
// follows on the next turn.
constexpr qsizetype maximumReadPerActivation = 1024 * 1024;
// The resident's listen backlog is full: connect(2) reports EAGAIN on a Unix
// stream socket without queuing, so the attempt repeats shortly, within the
// request's deadline.
constexpr int connectRetryMilliseconds = 20;
#endif
}

// On Linux the transport is a native Unix stream socket on Qt Core socket
// notifications, as the resident's LocalIpc already is: the interface helper
// then links no QtNetwork, which on distro Qt pulls in libproxy, curl and a
// TLS stack (#1161). Other platforms keep QLocalSocket.
struct SessionClient::Pending : QObject {
    explicit Pending(QObject* parent) : QObject(parent) {}
#ifdef Q_OS_LINUX
    ~Pending() override { closeSocket(); }
#endif

    void stopTransport() {
        // A receiver may be this client or its child. Do not let its destruction
        // call into SessionClient after the derived object's members are gone.
        // QPointer still guards the asynchronous delivery after transport stops.
        if (receiver) QObject::disconnect(receiver, nullptr, this, nullptr);
        if (deadline) deadline->stop();
#ifdef Q_OS_LINUX
        if (retry) retry->stop();
        closeSocket();
#else
        if (socket) {
            QObject::disconnect(socket, nullptr, this, nullptr);
            socket->abort();
        }
#endif
        outgoing.clear();
        incoming.clear();
    }

    QPointer<QObject> receiver;
    Reply reply;
    QString wireId;
    QByteArray outgoing;
    QByteArray incoming;
    QTimer* deadline = nullptr;
    bool submitted = false;
    bool complete = false;
#ifdef Q_OS_LINUX
    void closeSocket() {
        if (readable) readable->setEnabled(false);
        if (writable) writable->setEnabled(false);
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    int fd = -1;
    qsizetype sent = 0;
    QSocketNotifier* readable = nullptr;
    QSocketNotifier* writable = nullptr;
    QTimer* retry = nullptr;
#else
    QLocalSocket* socket = nullptr;
#endif
};

SessionClient::SessionClient(QString socketPath, QObject* parent)
    : QObject(parent), socketPath_(std::move(socketPath)) {}

SessionClient::~SessionClient() {
    const auto ids = pending_.keys();
    for (const auto id : ids) abandon(id);
}

SessionClient::RequestId SessionClient::request(const QString& method, const QJsonObject& params,
                                               QObject* receiver, Reply reply) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(receiver && receiver->thread() == thread());
    Q_ASSERT(reply);
    if (!receiver || receiver->thread() != thread() || !reply) return 0;

    const bool busy = pending_.size() >= maximumConcurrent;
    const auto id = ++nextRequestId_;
    auto* pending = new Pending(this);
    pending->receiver = receiver;
    pending->reply = std::move(reply);
    pending_.insert(id, pending);
    connect(receiver, &QObject::destroyed, pending, [this, id] { abandon(id); });
    if (busy) {
        finish(id, {"client_busy", "Too many requests are awaiting a resident response", {}});
        return id;
    }

    pending->wireId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending->outgoing = SessionProtocol::encodeRequest(pending->wireId, socketPath_, method, params);
    if (pending->outgoing.size() > maximumRequest) {
        finish(id, {"request_too_large", ByteSize::labels("Request exceeds 1 MiB"), {}});
        return id;
    }

    pending->deadline = new QTimer(pending);
    pending->deadline->setSingleShot(true);
    pending->deadline->setTimerType(Qt::PreciseTimer);
    connect(pending->deadline, &QTimer::timeout, pending,
            [this, id] { transportFailed(id, true); });
    pending->deadline->start(deadlineMs(method));
    QTimer::singleShot(0, pending, [this, id] { start(id); });
    return id;
}

int SessionClient::deadlineMs(const QString& method) { return SessionProtocol::deadlineMs(method); }

#ifdef Q_OS_LINUX
void SessionClient::start(RequestId id) {
    auto* pending = pending_.value(id);
    if (!pending || pending->complete) return;
    if (pending->fd < 0) {
        const auto name = QFile::encodeName(socketPath_);
        sockaddr_un address{};
        if (name.isEmpty() || name.contains('\0') || name.size() >= qsizetype(sizeof(address.sun_path))) {
            transportFailed(id, false);
            return;
        }
        pending->fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (pending->fd < 0) {
            transportFailed(id, false);
            return;
        }
    }
    const auto name = QFile::encodeName(socketPath_);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.constData(), size_t(name.size()) + 1);
    int result;
    do {
        result = ::connect(pending->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno == EAGAIN) {
        if (!pending->retry) {
            pending->retry = new QTimer(pending);
            pending->retry->setSingleShot(true);
            connect(pending->retry, &QTimer::timeout, pending, [this, id] { start(id); });
        }
        pending->retry->start(connectRetryMilliseconds);
        return;
    }
    if (result < 0) {
        // Absent, refused or unreachable: nothing reached the resident.
        transportFailed(id, false);
        return;
    }
    pending->readable = new QSocketNotifier(pending->fd, QSocketNotifier::Read, pending);
    pending->writable = new QSocketNotifier(pending->fd, QSocketNotifier::Write, pending);
    pending->writable->setEnabled(false);
    connect(pending->readable, &QSocketNotifier::activated, pending, [this, id] { receive(id); });
    connect(pending->writable, &QSocketNotifier::activated, pending, [this, id] { transmit(id); });
    transmit(id);
}

void SessionClient::transmit(RequestId id) {
    auto* pending = pending_.value(id);
    if (!pending || pending->complete || pending->fd < 0) return;
    while (pending->sent < pending->outgoing.size()) {
        const auto count = ::send(pending->fd, pending->outgoing.constData() + pending->sent,
                                  size_t(pending->outgoing.size() - pending->sent), MSG_NOSIGNAL);
        if (count > 0) {
            // Once handed to the transport, a lost reply cannot establish whether
            // the resident accepted the request. Never replay an uncertain mutation.
            pending->submitted = true;
            pending->sent += count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pending->writable->setEnabled(true);
            return;
        }
        transportFailed(id, false);
        return;
    }
    pending->writable->setEnabled(false);
    pending->outgoing.clear();
}

void SessionClient::receive(RequestId id) {
    auto* pending = pending_.value(id);
    if (!pending || pending->complete || pending->fd < 0) return;
    char chunk[16384];
    qsizetype drained = 0;
    while (drained < maximumReadPerActivation) {
        // Read what has arrived, bounded by the response limit, into the frame
        // buffer: no transient allocation of the limit (#1160).
        const auto room = maximumResponse + 1 - pending->incoming.size();
        const auto count = ::recv(pending->fd, chunk, size_t(std::min<qsizetype>(sizeof(chunk), room)), 0);
        if (count > 0) {
            drained += count;
            const auto previousSize = pending->incoming.size();
            pending->incoming.append(chunk, count);
            if (pending->incoming.size() > maximumResponse) {
                finish(id, invalidResponse(pending->submitted));
                return;
            }
            const auto newline = pending->incoming.indexOf('\n', previousSize);
            if (newline < 0) continue;
            if (newline != pending->incoming.size() - 1) {
                finish(id, invalidResponse(pending->submitted));
                return;
            }
            Error error;
            QJsonObject result;
            if (!SessionProtocol::decodeReply(pending->incoming, pending->wireId, error, result)) {
                finish(id, invalidResponse(pending->submitted));
                return;
            }
            finish(id, std::move(error), std::move(result));
            return;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        // The peer closed (or the socket failed) before a complete frame.
        transportFailed(id, false);
        return;
    }
}
#else
void SessionClient::start(RequestId id) {
    auto* pending = pending_.value(id);
    if (!pending || pending->complete) return;
    auto* socket = new QLocalSocket(pending);
    pending->socket = socket;
    socket->setReadBufferSize(maximumResponse + 1);
    connect(socket, &QLocalSocket::connected, pending, [this, id, pending, socket] {
        const auto written = socket->write(pending->outgoing);
        // Once handed to the transport, a lost reply cannot establish whether the
        // resident accepted the request. Never replay an uncertain mutation.
        pending->submitted = written > 0;
        const bool completeWrite = written == pending->outgoing.size();
        pending->outgoing.clear();
        if (!completeWrite) transportFailed(id, false);
    });
    connect(socket, &QLocalSocket::readyRead, pending, [this, id] { receive(id); });
    connect(socket, &QLocalSocket::errorOccurred, pending,
            [this, id](QLocalSocket::LocalSocketError) {
                receive(id);
                transportFailed(id, false);
            });
    connect(socket, &QLocalSocket::disconnected, pending, [this, id] {
        receive(id);
        transportFailed(id, false);
    });
    socket->connectToServer(socketPath_);
}

void SessionClient::transmit(RequestId) {}

void SessionClient::receive(RequestId id) {
    auto* pending = pending_.value(id);
    if (!pending || pending->complete || !pending->socket || pending->socket->bytesAvailable() == 0) return;
    const auto previousSize = pending->incoming.size();
    const auto remaining = maximumResponse + 1 - previousSize;
    pending->incoming += pending->socket->read(qMin(pending->socket->bytesAvailable(), qint64(remaining)));
    if (pending->incoming.size() > maximumResponse) {
        finish(id, invalidResponse(pending->submitted));
        return;
    }
    const auto newline = pending->incoming.indexOf('\n', previousSize);
    if (newline < 0) return;
    if (newline != pending->incoming.size() - 1) {
        finish(id, invalidResponse(pending->submitted));
        return;
    }

    Error error;
    QJsonObject result;
    if (!SessionProtocol::decodeReply(pending->incoming, pending->wireId, error, result)) {
        finish(id, invalidResponse(pending->submitted));
        return;
    }
    finish(id, std::move(error), std::move(result));
}
#endif

void SessionClient::transportFailed(RequestId id, bool timedOut) {
    const auto* pending = pending_.value(id);
    if (!pending || pending->complete) return;
    Error error = timedOut
        ? Error{"client_timeout", "Resident request exceeded its deadline", {}}
        : Error{"client_unavailable", "Cannot exchange a complete response with the resident", {}};
    if (pending->submitted) error.details["outcomeUnknown"] = true;
    finish(id, std::move(error));
}

void SessionClient::finish(RequestId id, Error error, QJsonObject result) {
    auto* pending = pending_.value(id);
    if (!pending || pending->complete) return;
    pending->complete = true;
    pending->stopTransport();
    // Keep the entry until delivery so abandon() and receiver destruction can
    // suppress even a completed response that is still queued in the event loop.
    QTimer::singleShot(0, pending, [this, id, error = std::move(error), result = std::move(result)]() mutable {
        auto* completed = pending_.take(id);
        if (!completed) return;
        auto receiver = completed->receiver;
        auto reply = std::move(completed->reply);
        completed->deleteLater();
        if (receiver) reply(std::move(error), std::move(result));
    });
}

void SessionClient::abandon(RequestId id) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto* pending = pending_.take(id);
    if (!pending) return;
    pending->complete = true;
    pending->stopTransport();
    pending->reply = {};
    pending->deleteLater();
}

const QString& SessionClient::socketPath() const { return socketPath_; }
}
