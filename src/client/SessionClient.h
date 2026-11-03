#pragma once
#include "SessionProtocol.h"
#include <QHash>
#include <QObject>
#include <functional>

namespace putgd {
class SessionClient : public QObject {
    Q_OBJECT
public:
    using RequestId = quint64;
    using Reply = std::function<void(Error, QJsonObject)>;

    explicit SessionClient(QString socketPath, QObject* parent = nullptr);
    virtual ~SessionClient();
    virtual RequestId request(const QString& method, const QJsonObject& params,
                              QObject* receiver, Reply reply);
    virtual void abandon(RequestId id);
    const QString& socketPath() const;
    // How long a request waits for its reply: 15 s, or longer for the methods
    // the resident answers late.
    static int deadlineMs(const QString& method);

private:
    struct Pending;
    void start(RequestId id);
    void transmit(RequestId id);
    void receive(RequestId id);
    void finish(RequestId id, Error error, QJsonObject result = {});
    void transportFailed(RequestId id, bool timedOut);

    QString socketPath_;
    QHash<RequestId, Pending*> pending_;
    RequestId nextRequestId_ = 0;
};
}
