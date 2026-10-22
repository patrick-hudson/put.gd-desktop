#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QDir>
#include <QSignalSpy>
#include <QTimer>
#include <vector>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include "fixtures/Waits.h"
#endif
using namespace putgd;
using namespace putgd::test;
class LocalIpcContract : public QObject {
 Q_OBJECT
 static QByteArray request(const QString& path) {
  return QJsonDocument(QJsonObject{{"schemaVersion",1},{"requestId","ipc-fixture"},{"session",path},{"method","capabilities"},{"params",QJsonObject{}}}).toJson(QJsonDocument::Compact)+'\n';
 }
 static QByteArray receive(QLocalSocket& socket) {
  QByteArray bytes;QElapsedTimer elapsed;elapsed.start();
  while(elapsed.elapsed()<putgd::test::scaled(3000)&&!bytes.contains('\n')){QCoreApplication::processEvents();bytes+=socket.readAll();putgd::test::settle(1);}
  return bytes;
 }
private slots:

 void slowPeerReceivesCompleteReply() {
  QTemporaryDir d;LocalIpcServer server;QVERIFY(server.listen(d.filePath("session.sock")));
  QByteArray payload(2*1024*1024,'x');payload[12345]='y';payload.back()='z';
  int notifications=0;
  connect(&server,&LocalIpcServer::newConnection,this,[&]{
   auto* socket=server.nextPendingConnection();socket->setReadBufferSize(4096);
   connect(socket,&LocalIpcConnection::readyRead,this,[&,socket]{
    ++notifications;socket->readAll();socket->stopReading();
    QTimer::singleShot(50,socket,[socket,payload]{socket->sendAndClose(payload);});
   });
  });
  QLocalSocket peer;peer.setReadBufferSize(1024);peer.connectToServer(server.fullServerName());QVERIFY(peer.waitForConnected(1000));peer.write("request");QVERIFY(peer.waitForBytesWritten(1000));PUTGD_TRY_COMPARE(notifications,1);
  peer.write("ignored trailing bytes");QVERIFY(peer.waitForBytesWritten(1000));putgd::test::settle(100);PUTGD_TRY_COMPARE(notifications,1);PUTGD_TRY_VERIFY(peer.bytesAvailable()<=1024);
  QByteArray reply;QElapsedTimer elapsed;elapsed.start();
  while(elapsed.elapsed()<putgd::test::scaled(5000)&&reply.size()<payload.size()){QCoreApplication::processEvents();reply+=peer.readAll();putgd::test::settle(1);}
  QCOMPARE(reply,payload);
 }
 void handledRequestObservesDisconnectBeforeReply() {
  QTemporaryDir d;LocalIpcServer server;QVERIFY(server.listen(d.filePath("session.sock")));
  LocalIpcConnection* accepted=nullptr;int notifications=0;
  connect(&server,&LocalIpcServer::newConnection,this,[&]{
   accepted=server.nextPendingConnection();
   connect(accepted,&LocalIpcConnection::readyRead,this,[&]{
    ++notifications;QCOMPARE(accepted->readAll(),QByteArray("request"));accepted->stopReading();
   });
  });
  QLocalSocket peer;peer.connectToServer(server.fullServerName());QVERIFY(peer.waitForConnected(1000));
  peer.write("request");QVERIFY(peer.waitForBytesWritten(1000));PUTGD_TRY_COMPARE(notifications,1);
  QSignalSpy disconnected(accepted,&LocalIpcConnection::disconnected);
  // A response may be waiting on asynchronous work when the CLI exits.
  peer.write(QByteArray(128*1024,'x'));QVERIFY(peer.waitForBytesWritten(1000));
  putgd::test::settle(10);peer.abort();
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(disconnected.count(),1,1000);
  QCOMPARE(notifications,1);
  accepted->sendAndClose("late reply\n");
  QCoreApplication::processEvents();QCOMPARE(disconnected.count(),1);
 }
 void disconnectedPeerDoesNotInterruptOtherRequests() {
  QTemporaryDir d;LocalIpcServer server;QVERIFY(server.listen(d.filePath("session.sock")));int replies=0;
  connect(&server,&LocalIpcServer::newConnection,this,[&]{auto* socket=server.nextPendingConnection();
   connect(socket,&LocalIpcConnection::readyRead,this,[&,socket]{socket->readAll();socket->stopReading();QTimer::singleShot(30,socket,[&,socket]{socket->sendAndClose("reply\n");++replies;});});});
  QLocalSocket gone;gone.connectToServer(server.fullServerName());QVERIFY(gone.waitForConnected(1000));gone.write("first");QVERIFY(gone.waitForBytesWritten(1000));putgd::test::settle(5);gone.abort();PUTGD_TRY_COMPARE(replies,1);
  QLocalSocket healthy;healthy.connectToServer(server.fullServerName());QVERIFY(healthy.waitForConnected(1000));healthy.write("second");QVERIFY(healthy.waitForBytesWritten(1000));QCOMPARE(receive(healthy),QByteArray("reply\n"));
 }
 void incompleteRequestExpires() {
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));
  QLocalSocket peer;peer.connectToServer(server.socketPath());QVERIFY(peer.waitForConnected(1000));peer.write("{partial");QVERIFY(peer.waitForBytesWritten(1000));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(peer.state(),QLocalSocket::UnconnectedState,12000);
  QLocalSocket healthy;healthy.connectToServer(server.socketPath());QVERIFY(healthy.waitForConnected(1000));healthy.write(request(server.socketPath()));QVERIFY(healthy.waitForBytesWritten(1000));QVERIFY(QJsonDocument::fromJson(receive(healthy)).object()["ok"].toBool());
 }
 void clientLimitPreservesAcceptedRequests() {
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));
  std::vector<std::unique_ptr<QLocalSocket>> peers;
  for(int i=0;i<65;++i){auto peer=std::make_unique<QLocalSocket>();peer->connectToServer(server.socketPath());QVERIFY(peer->waitForConnected(1000));QCoreApplication::processEvents();peers.push_back(std::move(peer));}
  PUTGD_TRY_COMPARE(peers.back()->state(),QLocalSocket::UnconnectedState);
  peers.front()->write(request(server.socketPath()));QVERIFY(peers.front()->waitForBytesWritten(1000));QVERIFY(QJsonDocument::fromJson(receive(*peers.front())).object()["ok"].toBool());
 }
 void fragmentedRequestAndDuplicateFrames() {
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));
  const auto bytes=request(server.socketPath());QLocalSocket client;client.connectToServer(server.socketPath());QVERIFY(client.waitForConnected(1000));
  client.write(bytes.left(bytes.size()/2));QVERIFY(client.waitForBytesWritten(1000));PUTGD_VERIFY_STEADY(client.bytesAvailable()==0,20);
  client.write(bytes.mid(bytes.size()/2));QVERIFY(client.waitForBytesWritten(1000));const auto response=QJsonDocument::fromJson(receive(client)).object();QVERIFY(response["ok"].toBool());QCOMPARE(response["requestId"].toString(),QString("ipc-fixture"));
  QLocalSocket duplicate;duplicate.connectToServer(server.socketPath());QVERIFY(duplicate.waitForConnected(1000));duplicate.write(bytes+bytes);QVERIFY(duplicate.waitForBytesWritten(1000));QCOMPARE(QJsonDocument::fromJson(receive(duplicate)).object()["error"].toObject()["code"].toString(),QString("invalid_request"));
 }
 void endpointOwnershipAndLiveResident() {
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));
  const auto path=server.socketPath();LocalServer second(settings,operations,history);QCOMPARE(second.listen(d.filePath("runtime")).error.code,QString("session_running"));
  QLocalSocket client;client.connectToServer(path);QVERIFY(client.waitForConnected(1000));client.write(request(path));QVERIFY(client.waitForBytesWritten(1000));QVERIFY(QJsonDocument::fromJson(receive(client)).object()["ok"].toBool());
  QVERIFY(!(QFile::permissions(path)&(QFile::ReadOther|QFile::WriteOther|QFile::ReadGroup|QFile::WriteGroup)));
  const auto other=d.filePath("not-a-socket");QVERIFY(QDir().mkpath(other));QFile file(other+"/session.sock");QVERIFY(file.open(QIODevice::WriteOnly));file.write("preserve");file.close();LocalServer blocked(settings,operations,history);QVERIFY(blocked.listen(other).error);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("preserve"));
 }
#ifdef Q_OS_LINUX
 void halfClosedRequestReceivesReply() {
  QTemporaryDir d;LocalIpcServer server;QVERIFY(server.listen(d.filePath("session.sock")));
  connect(&server,&LocalIpcServer::newConnection,this,[&]{auto* peer=server.nextPendingConnection();connect(peer,&LocalIpcConnection::readyRead,this,[peer]{if(peer->readAll()=="request")peer->sendAndClose("reply\n");});});
  QLocalSocket peer;peer.connectToServer(server.fullServerName());QVERIFY(peer.waitForConnected(1000));peer.write("request");QVERIFY(peer.waitForBytesWritten(1000));QCOMPARE(::shutdown(int(peer.socketDescriptor()),SHUT_WR),0);
  QCOMPARE(receive(peer),QByteArray("reply\n"));
 }
 void teardownPreservesReplacementEndpoint() {
  QTemporaryDir d;const auto path=d.filePath("session.sock");
  {LocalIpcServer server;QVERIFY(server.listen(path));QVERIFY(QFile::remove(path));QFile replacement(path);QVERIFY(replacement.open(QIODevice::WriteOnly));replacement.write("preserve");}
  QFile replacement(path);QVERIFY(replacement.open(QIODevice::ReadOnly));QCOMPARE(replacement.readAll(),QByteArray("preserve"));
 }
 void recoversOwnedStaleSocket() {
  QTemporaryDir d;const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime));const auto path=QFile::encodeName(runtime+"/session.sock");sockaddr_un address{};address.sun_family=AF_UNIX;QVERIFY(path.size()<qsizetype(sizeof(address.sun_path)));std::memcpy(address.sun_path,path.constData(),path.size()+1);
  const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);QVERIFY(fd>=0);const int result=::bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));::close(fd);QCOMPARE(result,0);
  ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(runtime));
  QLocalSocket client;client.connectToServer(server.socketPath());QVERIFY(client.waitForConnected(1000));client.write(request(server.socketPath()));QVERIFY(client.waitForBytesWritten(1000));QVERIFY(QJsonDocument::fromJson(receive(client)).object()["ok"].toBool());
 }
#endif
};
QTEST_GUILESS_MAIN(LocalIpcContract)
#include "test_local_ipc.moc"
