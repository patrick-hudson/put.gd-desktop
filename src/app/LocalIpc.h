#pragma once
#include <QObject>
#include <QByteArray>
#include <QString>
#include <memory>
namespace putgd {
enum class LocalSessionState { Connected, Absent, Unavailable };
// A connected peer owns the session even when its activation reply fails.
// response contains one complete frame only; errorCode never contains peer data.
struct LocalActivation { bool connected=false; QByteArray response; QString errorCode; };
LocalSessionState probeLocalSession(const QString& path,int timeoutMs);
LocalActivation activateLocalSession(const QString& path,const QByteArray& request);
// The process listening on a session socket, from the kernel's credentials for
// the connection (#490). start is its start time since boot, which tells it
// apart from a later process given the same PID; 0 where it cannot be read.
struct LocalPeer { bool connected=false; qint64 pid=0; uint uid=0; quint64 start=0; };
LocalPeer localSessionPeer(const QString& path);
quint64 processStartTime(qint64 pid);
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
