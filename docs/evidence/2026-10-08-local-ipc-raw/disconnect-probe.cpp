#include "LocalIpc.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>
class AbortProbe : public QObject {
 Q_OBJECT
private slots:
 void originalQtObservesAbortBeforeReply() {
  QTemporaryDir d;QLocalServer server;QVERIFY(server.listen(d.filePath("session")));
  QLocalSocket peer;peer.connectToServer(server.fullServerName());QVERIFY(peer.waitForConnected(1000));
  QTRY_VERIFY(server.hasPendingConnections());auto* accepted=server.nextPendingConnection();
  peer.write("request");QVERIFY(peer.waitForBytesWritten(1000));QTRY_VERIFY(accepted->bytesAvailable()>0);QCOMPARE(accepted->readAll(),QByteArray("request"));
  QSignalSpy disconnected(accepted,&QLocalSocket::disconnected);peer.abort();
  QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(),1,1000);
 }
 void nativeObservesAbortBeforeReply() {
  QTemporaryDir d;putgd::LocalIpcServer server;QVERIFY(server.listen(d.filePath("session")));
  putgd::LocalIpcConnection* accepted=nullptr;bool handled=false;
  connect(&server,&putgd::LocalIpcServer::newConnection,this,[&]{accepted=server.nextPendingConnection();connect(accepted,&putgd::LocalIpcConnection::readyRead,this,[&]{QCOMPARE(accepted->readAll(),QByteArray("request"));accepted->stopReading();handled=true;});});
  QLocalSocket peer;peer.connectToServer(server.fullServerName());QVERIFY(peer.waitForConnected(1000));peer.write("request");QVERIFY(peer.waitForBytesWritten(1000));QTRY_VERIFY(handled);
  QSignalSpy disconnected(accepted,&putgd::LocalIpcConnection::disconnected);peer.abort();
  QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(),1,1000);
 }
};
QTEST_GUILESS_MAIN(AbortProbe)
#include "probe.moc"
