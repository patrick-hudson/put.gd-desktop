#pragma once
#include <QObject>
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>
namespace putgd {
enum class LocalSessionState { Connected, Absent, Unavailable };
// The process listening on a session socket, from the kernel's credentials for
// the connection (#490). start is its start time since boot, which tells it
// apart from a later process given the same PID; 0 where it cannot be read.
struct LocalPeer { bool connected=false; qint64 pid=0; uint uid=0; quint64 start=0; };
quint64 processStartTime(qint64 pid);
// A connected peer owns the session even when its activation reply fails.
// response contains one complete frame only; errorCode never contains peer data.
// refused: admit turned the peer down, and nothing was sent to it.
struct LocalActivation { bool connected=false; QByteArray response; QString errorCode; LocalPeer peer; bool refused=false; };
LocalSessionState probeLocalSession(const QString& path,int timeoutMs);
LocalActivation activateLocalSession(const QString& path,const QByteArray& request,const std::function<bool(const LocalPeer&)>& admit={});
// One request frame sent whole, one complete reply frame (ending in '\n', at
// most maximumResponse bytes) read back, all within deadlineMs. The resident's
// own --upload client (#1105) uses this synchronous exchange, so the resident
// links no QtNetwork. sent: the whole request reached the socket, so a reply
// that never came leaves the request's outcome unknown. errorCode is one of
// activation_timeout, transport_failed, response_too_large, invalid_response.
struct LocalExchange { bool connected=false; bool sent=false; QByteArray response; QString errorCode; };
LocalExchange exchangeLocalSession(const QString& path,const QByteArray& request,int deadlineMs,qsizetype maximumResponse);
class LocalIpcConnection final : public QObject {
 Q_OBJECT
public:
 ~LocalIpcConnection() override;
 void setReadBufferSize(qsizetype size);
 QByteArray readAll();
 // Stop request delivery while still observing a peer disconnect.
 void stopReading();
 void sendAndClose(QByteArray bytes);
 void disconnectFromServer();
signals:
 void readyRead();
 void disconnected();
private:
 friend class LocalIpcServer;
 struct State;
 explicit LocalIpcConnection(qintptr handle,QObject* parent);
 std::unique_ptr<State> state_;
};
class LocalIpcServer final : public QObject {
 Q_OBJECT
public:
 explicit LocalIpcServer(QObject* parent=nullptr);
 ~LocalIpcServer() override;
 bool listen(const QString& path);
 QString fullServerName() const;
 LocalIpcConnection* nextPendingConnection();
 static bool removeServer(const QString& path);
signals:
 void newConnection();
private:
 struct State;
 std::unique_ptr<State> state_;
};
}
