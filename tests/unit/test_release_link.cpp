#include "core/ReleaseLink.h"
#include <QTest>
using namespace putgd;
class ReleaseLinkTest:public QObject {
 Q_OBJECT
 static QJsonObject build(const QString& version,const QJsonValue& dirty=false,const QString& commit="528fe444b0cb0123456789abcdef0123456789ab"){
  return {{"version",version},{"baseVersion","0.1.1"},{"gitCommit",commit},{"dirty",dirty},{"sourceProvenance","git"}};
 }
private slots:
 void mapsVersionsToTheirGitHubPage_data(){
  QTest::addColumn<QJsonObject>("build");QTest::addColumn<QString>("kind");QTest::addColumn<QString>("url");
  const QString tag="https://github.com/patrick-hudson/put.gd-desktop/releases/tag/";
  const QString commit="https://github.com/patrick-hudson/put.gd-desktop/commit/528fe444b0cb0123456789abcdef0123456789ab";
  QTest::newRow("stable")<<build("0.1.1")<<"stable"<<tag+"v0.1.1";
  QTest::newRow("stable two-digit")<<build("1.10.0")<<"stable"<<tag+"v1.10.0";
  QTest::newRow("candidate")<<build("0.1.1-rc.2")<<"candidate"<<tag+"v0.1.1-rc.2";
  QTest::newRow("nightly")<<build("0.1.1-nightly.430")<<"nightly"<<tag+"v0.1.1-nightly.430";
  QTest::newRow("clean development")<<build("0.1.1-dev.430+g528fe444b0cb")<<"commit"<<commit;
  QTest::newRow("supplied archive")<<QJsonObject{{"version","0.1.1-dev.430+g528fe444b0cb"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",QJsonValue::Null},{"sourceProvenance","supplied"}}<<"commit"<<commit;
  QTest::newRow("dirty development")<<build("0.1.1-dev.430+g528fe444b0cb.dirty",true)<<"none"<<"";
  QTest::newRow("dirty flag without suffix")<<build("0.1.1-dev.430+g528fe444b0cb",true)<<"none"<<"";
  QTest::newRow("commit does not match")<<build("0.1.1-dev.430+g528fe444b0cb",false,"0123456789abcdef0123456789abcdef01234567")<<"none"<<"";
  QTest::newRow("short commit")<<build("0.1.1-dev.430+g528fe444b0cb",false,"528fe444b0cb")<<"none"<<"";
  QTest::newRow("unknown")<<build("0.1.1-dev.unknown",QJsonValue::Null,"")<<"none"<<"";
  QTest::newRow("leading zero")<<build("0.01.1")<<"none"<<"";
  QTest::newRow("candidate zero")<<build("0.1.1-rc.0")<<"none"<<"";
  QTest::newRow("tag prefix")<<build("v0.1.1")<<"none"<<"";
  QTest::newRow("path injection")<<build("0.1.1/../../evil")<<"none"<<"";
  QTest::newRow("empty")<<QJsonObject{}<<"none"<<"";
 }
 void mapsVersionsToTheirGitHubPage(){
  QFETCH(QJsonObject,build);QFETCH(QString,kind);QFETCH(QString,url);
  const auto link=releaseLink(build);
  QCOMPARE(link.version,build["version"].toString());QCOMPARE(link.kind,kind);QCOMPARE(link.url.toString(),url);QCOMPARE(link.url.isEmpty(),url.isEmpty());
  QCOMPARE(link.toJson(),(QJsonObject{{"version",link.version},{"kind",kind},{"url",url}}));
 }
};
QTEST_GUILESS_MAIN(ReleaseLinkTest)
#include "test_release_link.moc"
