#include "storage/s3/S3Provider.h"
#include "core/Settings.h"
#include <QHostAddress>
#include <QUrl>
#include <QtTest>
using namespace putgd;
class IpAddressContract : public QObject {
 Q_OBJECT
private slots:
 void normalizedLiteralCompatibility() {
  CredentialStore credentials;S3Provider provider(credentials);
  auto document=Settings::developmentTemplate();auto base=document["profiles"].toArray().first().toObject()["settings"].toObject();
  QStringList hosts{"example.invalid","localhost","127.0.0.1","127.255.255.254","127.1","2130706433","0x7f000001","0177.0.0.1","0.0.0.0","128.0.0.1","192.0.2.1","[::1]","[::]","[::ffff:127.0.0.1]","[::ffff:7f00:1]","[::127.0.0.1]","[2001:db8::1]","[fe80::1%25lo]","[::1%25lo]","[v1.test]"};
  for(int octet=0;octet<256;++octet)hosts.append(QString("127.%1.4.5").arg(octet));
  for(const auto& host:hosts) {
   const QUrl endpoint("http://"+host);if(!endpoint.isValid()||endpoint.host().isEmpty())continue;
   const QHostAddress original(endpoint.host());
   auto settings=base;settings["addressing"]="path";settings["endpoint"]=endpoint.toString();settings["publicUrlBase"]=QJsonValue::Null;
   QVERIFY2(!provider.validateSettings(settings)==original.isLoopback(),qPrintable(endpoint.host()));
   settings["endpoint"]="https://"+host;settings["addressing"]="virtual";settings["bucket"]="synthetic-bucket";
   QVERIFY2(!provider.validateSettings(settings)==original.isNull(),qPrintable(endpoint.host()));
   settings["addressing"]="path";settings["endpoint"]="https://s3.example.invalid";settings["publicUrlBase"]="http://"+host;
   QVERIFY2(!provider.validateSettings(settings)==original.isLoopback(),qPrintable(endpoint.host()));
  }
 }
};
QTEST_GUILESS_MAIN(IpAddressContract)
#include "test_ip_address.moc"
