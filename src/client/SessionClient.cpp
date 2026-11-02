#include "core/ByteSize.h"
#include "SessionClient.h"
#include <QLocalSocket>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <utility>

namespace putgd {
namespace {
using SessionProtocol::maximumRequest;
using SessionProtocol::maximumResponse;
using SessionProtocol::invalidResponse;
constexpr qsizetype maximumConcurrent = 64;
}

struct SessionClient::Pending : QObject {
    explicit Pending(QObject* parent) : QObject(parent) {}

    void stopTransport() {
        // A receiver may be this client or its child. Do not let its destruction
        // call into SessionClient after the derived object's members are gone.
        // QPointer still guards the asynchronous delivery after transport stops.
        if (receiver) QObject::disconnect(receiver, nullptr, this, nullptr);
        if (deadline) deadline->stop();
        if (socket) {
            QObject::disconnect(socket, nullptr, this, nullptr);
            socket->abort();
        }
        outgoing.clear();
        incoming.clear();
    }

    QPointer<QObject> receiver;
    Reply reply;
    QString wireId;
    QByteArray outgoing;
    QByteArray incoming;
    QLocalSocket* socket = nullptr;
    QTimer* deadline = nullptr;
    bool submitted = false;
    bool complete = false;
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

void SessionClient::receive(RequestId id) {
    auto* pending = pending_.value(id);
    if (!pending || pending->complete || !pending->socket || pending->socket->bytesAvailable() == 0) return;
    const auto previousSize = pending->incoming.size();
    pending->incoming += pending->socket->read(maximumResponse + 1 - previousSize);
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
