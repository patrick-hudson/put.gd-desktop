#include "LocalIpc.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>
#include <sys/socket.h>
class HalfCloseProbe : public QObject {
 Q_OBJECT
private slots:
 void originalQtObservesHalfCloseBeforeReply() {
  QTemporaryDir d;QLocalServer server;QVERIFY(server.listen(d.filePath("session")));
  QLocalSocket peer;peer.connectToServer(server.fullServerName());QVERIFY(peer.waitForConnected(1000));
  QTRY_VERIFY(server.hasPendingConnections());auto* accepted=server.nextPendingConnection();
  peer.write("request");QVERIFY(peer.waitForBytesWritten(1000));QTRY_VERIFY(accepted->bytesAvailable()>0);QCOMPARE(accepted->readAll(),QByteArray("request"));
  QSignalSpy disconnected(accepted,&QLocalSocket::disconnected);QCOMPARE(::shutdown(int(peer.socketDescriptor()),SHUT_WR),0);
  QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(),1,1000);
 }
 void nativeObservesHalfCloseBeforeReply() {
  QTemporaryDir d;putgd::LocalIpcServer server;QVERIFY(server.listen(d.filePath("session")));
  putgd::LocalIpcConnection* accepted=nullptr;bool handled=false;
  connect(&server,&putgd::LocalIpcServer::newConnection,this,[&]{accepted=server.nextPendingConnection();connect(accepted,&putgd::LocalIpcConnection::readyRead,this,[&]{QCOMPARE(accepted->readAll(),QByteArray("request"));accepted->stopReading();handled=true;});});
  QLocalSocket peer;peer.connectToServer(server.fullServerName());QVERIFY(peer.waitForConnected(1000));peer.write("request");QVERIFY(peer.waitForBytesWritten(1000));QTRY_VERIFY(handled);
  QSignalSpy disconnected(accepted,&putgd::LocalIpcConnection::disconnected);QCOMPARE(::shutdown(int(peer.socketDescriptor()),SHUT_WR),0);
  QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(),1,1000);
 }
};
QTEST_GUILESS_MAIN(HalfCloseProbe)
#include "probe.moc"
