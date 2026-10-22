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
  QTest::newRow("stable")<<build("0.1.1")<<"stable"<<tag+"v0.1.1";
  QTest::newRow("calendar stable")<<build("26.1101.960")<<"stable"<<tag+"v26.1101.960";
  QTest::newRow("calendar nightly")<<build("26.1022.942-nightly")<<"nightly"<<tag+"v26.1022.942-nightly";
  QTest::newRow("calendar January")<<build("27.105.1500-nightly")<<"nightly"<<tag+"v27.105.1500-nightly";
  QTest::newRow("calendar leap day")<<build("28.229.1500")<<"stable"<<tag+"v28.229.1500";
  QTest::newRow("candidate")<<build("0.1.1-rc.2")<<"candidate"<<tag+"v0.1.1-rc.2";
  QTest::newRow("nightly")<<build("0.1.1-nightly.430")<<"nightly"<<tag+"v0.1.1-nightly.430";
  // Local and CI development builds have no GitHub release, even when their commit is pushed.
  QTest::newRow("clean development")<<build("0.1.1-dev.430+g528fe444b0cb")<<"none"<<"";
  QTest::newRow("supplied archive")<<QJsonObject{{"version","0.1.1-dev.430+g528fe444b0cb"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",QJsonValue::Null},{"sourceProvenance","supplied"}}<<"none"<<"";
  QTest::newRow("dirty development")<<build("0.1.1-dev.430+g528fe444b0cb.dirty",true)<<"none"<<"";
  QTest::newRow("dirty flag without suffix")<<build("0.1.1-dev.430+g528fe444b0cb",true)<<"none"<<"";
  QTest::newRow("dirty release identity")<<build("0.1.1-nightly.430",true)<<"none"<<"";
  QTest::newRow("unknown")<<build("0.1.1-dev.unknown",QJsonValue::Null,"")<<"none"<<"";
  QTest::newRow("leading zero")<<build("0.01.1")<<"none"<<"";
  QTest::newRow("pre-#373 major other than 0")<<build("1.10.0")<<"none"<<"";
  QTest::newRow("calendar leading zero")<<build("26.0105.942")<<"none"<<"";
  QTest::newRow("calendar invalid date")<<build("26.229.942")<<"none"<<"";
  QTest::newRow("calendar candidate")<<build("26.1101.960-rc")<<"none"<<"";
  QTest::newRow("calendar numbered nightly")<<build("26.1022.942-nightly.942")<<"none"<<"";
  QTest::newRow("calendar development")<<build("26.1022.942-dev+g528fe444b0cb")<<"none"<<"";
  QTest::newRow("dirty calendar nightly")<<build("26.1022.942-nightly",true)<<"none"<<"";
  QTest::newRow("development suffix on a release")<<build("0.1.1-nightly.430+g528fe444b0cb")<<"none"<<"";
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
