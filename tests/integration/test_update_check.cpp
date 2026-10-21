#include "cli/UpdateCheck.h"
#include "core/CaBundle.h"
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSslCertificate>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
using namespace putgd;
// The checker's one request (#293): which bytes leave the computer, and how
// it reads the answer. A loopback server stands in for app.put.gd.
class FeedServer final:public QObject {
 Q_OBJECT
public:
 QByteArray status="200 OK",body;QList<QByteArray> requests;
 FeedServer(){QVERIFY(server_.listen(QHostAddress::LocalHost));connect(&server_,&QTcpServer::newConnection,this,&FeedServer::accept);}
 QByteArray base() const {return "http://127.0.0.1:"+QByteArray::number(server_.serverPort())+"/update/v1";}
private:
 void accept(){
  while(auto* socket=server_.nextPendingConnection()){
   auto request=std::make_shared<QByteArray>();
   connect(socket,&QTcpSocket::readyRead,socket,[this,socket,request]{
    *request+=socket->readAll();if(!request->contains("\r\n\r\n"))return;
    requests.append(*request);
    socket->write("HTTP/1.1 "+status+"\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
    socket->disconnectFromHost();
   });
  }
 }
 QTcpServer server_;
};
class UpdateCheckTest:public QObject {
 Q_OBJECT
 static update::Build build(){
  return update::build(QJsonObject{{"version","0.1.1-nightly.588"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",false},{"sourceProvenance","git"}},"debian13",true);
 }
 static QByteArray feed(const QString& version){
  const auto tag="v"+version;const auto name="putgd-"+version+"-debian13-x86_64.deb";
  return QJsonDocument(QJsonObject{{"schema",1},{"channel","nightly"},{"release",QJsonObject{{"tag",tag},{"version",version},{"publishedAt","2026-10-20T05:55:50Z"},
   {"assets",QJsonObject{{"debian13",QJsonObject{{"name",name},{"url","https://github.com/patrick-hudson/put.gd-desktop/releases/download/"+tag+"/"+name},{"size",1}}}}}}}}).toJson();
 }
private slots:
 void cleanup(){qunsetenv("PUTGD_UPDATE_FEED_URL");}
 void sendsOneIdenticalRequestAndComparesLocally(){
  FeedServer server;server.body=feed("0.1.1-nightly.590");qputenv("PUTGD_UPDATE_FEED_URL",server.base());
  Error error;const auto result=fetchUpdate(build(),"nightly",error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["updateAvailable"].toBool(),true);QCOMPARE(result["latest"].toString(),QString("0.1.1-nightly.590"));QVERIFY(!result["checkedAt"].toString().isEmpty());
  // The child, not the resident, reads the repository file (#293 phase 2).
  QVERIFY(QStringList({"software-updates","download"}).contains(result["action"].toObject()["kind"].toString()));
  QCOMPARE(server.requests.size(),1);
  const auto lines=server.requests.first().split('\n');
  QCOMPARE(lines.first().trimmed(),QByteArray("GET /update/v1/nightly.json HTTP/1.1"));
  QStringList names;
  for(const auto& raw:lines.mid(1)){
   const auto line=raw.trimmed();if(line.isEmpty())continue;
   const auto name=line.left(line.indexOf(':')).toLower();names.append(QString::fromLatin1(name));
   // Qt 6.8 and later send lower-case names.
   if(name=="user-agent")QCOMPARE(line.toLower(),QByteArray("user-agent: put.gd-update-check/1"));
   if(name=="accept-language")QCOMPARE(line.toLower(),QByteArray("accept-language: en"));
  }
  // Nothing beyond these: no cookie, version, locale or identifier. cache-control
  // and pragma are no-cache, from AlwaysNetwork.
  for(const auto& name:names)QVERIFY2(QStringList({"host","user-agent","accept","accept-language","accept-encoding","connection","cache-control","pragma"}).contains(name),qPrintable(name));
  QVERIFY(names.contains("user-agent"));
 }
 void missingFeedIsUnavailable(){
  FeedServer server;server.status="404 Not Found";server.body="{}";qputenv("PUTGD_UPDATE_FEED_URL",server.base());
  Error error;fetchUpdate(build(),"nightly",error);
  QCOMPARE(error.code,QString("update_feed_unavailable"));QCOMPARE(error.details["httpStatus"].toInt(),404);
 }
 void oversizedFeedIsRejected(){
  FeedServer server;server.body=QByteArray(300*1024,' ');qputenv("PUTGD_UPDATE_FEED_URL",server.base());
  Error error;fetchUpdate(build(),"nightly",error);QCOMPARE(error.code,QString("update_feed_invalid"));
 }
 void invalidFeedIsRejected(){
  FeedServer server;server.body=feed("0.1.2");qputenv("PUTGD_UPDATE_FEED_URL",server.base());
  Error error;fetchUpdate(build(),"nightly",error);QCOMPARE(error.code,QString("update_feed_invalid"));
 }
 void plainHttpElsewhereIsRefusedBeforeConnecting(){
  qputenv("PUTGD_UPDATE_FEED_URL","http://example.com/update/v1/");
  Error error;fetchUpdate(build(),"nightly",error);QCOMPARE(error.code,QString("update_feed_unavailable"));
 }
 // #329: the AppImage names the host's bundle; its certificates join Qt's default CAs.
 void hostCaBundleAddsItsCertificates(){
#if QT_CONFIG(ssl)
  const auto original=QSslConfiguration::defaultConfiguration();
  const auto restore=qScopeGuard([&]{QSslConfiguration::setDefaultConfiguration(original);qunsetenv("PUTGD_CA_BUNDLE");});
  const auto path=QFINDTESTDATA("../fixtures/ca-bundle-329.pem");QVERIFY(!path.isEmpty());
  const auto certificate=QSslCertificate::fromPath(path);QCOMPARE(certificate.size(),1);
  qunsetenv("PUTGD_CA_BUNDLE");QVERIFY(!useHostCaBundle());
  qputenv("PUTGD_CA_BUNDLE",QFile::encodeName(path+".absent"));QVERIFY(!useHostCaBundle());
  QVERIFY(!QSslConfiguration::defaultConfiguration().caCertificates().contains(certificate.first()));
  qputenv("PUTGD_CA_BUNDLE",QFile::encodeName(path));QVERIFY(useHostCaBundle());
  QVERIFY(QSslConfiguration::defaultConfiguration().caCertificates().contains(certificate.first()));
  QVERIFY(QSslConfiguration::defaultConfiguration().peerVerifyMode()!=QSslSocket::VerifyNone);
#else
  QSKIP("Qt without TLS");
#endif
 }
};
QTEST_GUILESS_MAIN(UpdateCheckTest)
#include "test_update_check.moc"
