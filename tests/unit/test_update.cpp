#include "core/Update.h"
#include <QJsonDocument>
#include <QTest>
using namespace putgd;
// Update feed contract v1 and the version order (#293).
class UpdateTest:public QObject {
 Q_OBJECT
 static QJsonObject manifest(const QString& version,const QJsonValue& dirty=false){
  return {{"version",version},{"baseVersion","0.1.1"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",dirty},{"sourceProvenance","git"}};
 }
 static QJsonObject asset(const QString& tag,const QString& name){
  return {{"name",name},{"url","https://github.com/patrick-hudson/put.gd-desktop/releases/download/"+tag+"/"+name},{"size",123},{"sha256",QString(64,'a')}};
 }
 static QByteArray feed(const QString& channel,const QJsonValue& release){
  return QJsonDocument(QJsonObject{{"schema",1},{"channel",channel},{"generatedAt","2026-10-20T18:32:24Z"},{"release",release}}).toJson();
 }
 static QJsonObject release(const QString& version){
  const auto tag="v"+version;
  return {{"tag",tag},{"version",version},{"publishedAt","2026-10-20T05:55:50Z"},{"notesUrl","https://example.com/elsewhere"},
   {"assets",QJsonObject{{"debian13",asset(tag,"putgd-"+version+"-debian13-x86_64.deb")},{"appimage",asset(tag,"putgd-"+version+"-x86_64.AppImage")}}}};
 }
private slots:
 void buildsFollowTheirChannelOnlyWhenAPackageJobMadeThem_data(){
  QTest::addColumn<QString>("version");QTest::addColumn<QString>("target");QTest::addColumn<bool>("built");
  QTest::addColumn<QString>("channel");QTest::addColumn<bool>("supported");QTest::addColumn<QString>("reason");
  QTest::newRow("nightly deb")<<"0.1.1-nightly.588"<<"debian13"<<true<<"nightly"<<true<<"";
  QTest::newRow("stable appimage")<<"0.1.1"<<"appimage"<<true<<"stable"<<true<<"";
  QTest::newRow("candidate follows stable")<<"0.1.1-rc.2"<<"fedora44"<<true<<"stable"<<true<<"";
  QTest::newRow("flatpak updates itself")<<"0.1.1"<<"flatpak"<<true<<"stable"<<false<<"flatpak";
  QTest::newRow("turned off at build")<<"0.1.1"<<"ubuntu24.04"<<false<<"stable"<<false<<"disabled_at_build";
  QTest::newRow("no package job")<<"0.1.1-nightly.588"<<""<<true<<"nightly"<<false<<"no_package";
  QTest::newRow("unknown package")<<"0.1.1"<<"arch"<<true<<"stable"<<false<<"no_package";
  QTest::newRow("development")<<"0.1.1-dev.430+g528fe444b0cb"<<"debian13"<<true<<""<<false<<"development_build";
 }
 void buildsFollowTheirChannelOnlyWhenAPackageJobMadeThem(){
  QFETCH(QString,version);QFETCH(QString,target);QFETCH(bool,built);QFETCH(QString,channel);QFETCH(bool,supported);QFETCH(QString,reason);
  const auto result=update::build(manifest(version),target,built);
  QCOMPARE(result.channel,channel);QCOMPARE(result.supported,supported);QCOMPARE(result.reason,reason);QCOMPARE(result.version,version);
  QCOMPARE(result.toJson()["supported"].toBool(),supported);
 }
 void ordersNightlyBeforeCandidateBeforeStable(){
  QCOMPARE(update::compareVersions("0.1.1-nightly.590","0.1.1-nightly.588"),std::optional(1));
  QCOMPARE(update::compareVersions("0.1.1-nightly.588","0.1.1-nightly.588"),std::optional(0));
  QCOMPARE(update::compareVersions("0.1.1-nightly.9999","0.1.1-rc.1"),std::optional(-1));
  QCOMPARE(update::compareVersions("0.1.1-rc.3","0.1.1"),std::optional(-1));
  QCOMPARE(update::compareVersions("0.1.1","0.1.2-nightly.1"),std::optional(-1));
  QCOMPARE(update::compareVersions("0.10.0","0.9.9"),std::optional(1));
  QCOMPARE(update::compareVersions("0.1.1-dev.5+gabc","0.1.1"),std::nullopt);
  QCOMPARE(update::compareVersions("v0.1.1","0.1.1"),std::nullopt);
 }
 // #373: calendar versions YY.MDD.N[-nightly] sort by date, then commit count,
 // and every one sorts after every pre-#373 0.Y.Z version.
 void ordersCalendarVersionsByDateAfterEveryLegacyVersion(){
  QCOMPARE(update::compareVersions("26.1022.942-nightly","0.1.1-nightly.925"),std::optional(1));
  QCOMPARE(update::compareVersions("26.1022.942-nightly","0.1.1"),std::optional(1));
  QCOMPARE(update::compareVersions("0.1.1-rc.1","26.1022.942-nightly"),std::optional(-1));
  QCOMPARE(update::compareVersions("26.1022.943-nightly","26.1022.942-nightly"),std::optional(1));
  // MDD is a number: January 5 (105) is before October 22 (1022).
  QCOMPARE(update::compareVersions("26.105.990-nightly","26.1022.942-nightly"),std::optional(-1));
  QCOMPARE(update::compareVersions("27.105.1000-nightly","26.1231.5000"),std::optional(1));
  // On one YY.MDD.N the nightly sorts before the stable release.
  QCOMPARE(update::compareVersions("26.1101.960-nightly","26.1101.960"),std::optional(-1));
  QCOMPARE(update::compareVersions("26.1101.960","26.1101.960"),std::optional(0));
  // A later nightly is newer than an earlier stable release, so stable is usually an older switch (#344).
  QCOMPARE(update::compareVersions("26.1105.951-nightly","26.1101.960"),std::optional(1));
  QCOMPARE(update::compareVersions("26.1101.961-nightly","26.1101.960"),std::optional(1));
 }
 void rejectsMalformedCalendarVersions_data(){
  QTest::addColumn<QString>("version");
  QTest::newRow("leading zero month")<<"26.0105.990";
  QTest::newRow("leading zero day part")<<"26.10.990";
  QTest::newRow("leading zero count")<<"26.105.0990";
  QTest::newRow("zero count")<<"26.105.0";
  QTest::newRow("leading zero year")<<"026.105.990";
  QTest::newRow("month 13")<<"26.1301.990";
  QTest::newRow("day 32")<<"26.132.990";
  QTest::newRow("no leap day")<<"26.229.990";
  QTest::newRow("before the scheme")<<"25.1231.990";
  QTest::newRow("candidate")<<"26.1101.960-rc";
  QTest::newRow("numbered candidate")<<"26.1101.960-rc.1";
  QTest::newRow("numbered nightly")<<"26.1022.942-nightly.942";
  QTest::newRow("development")<<"26.1022.942-dev+g528fe444b0cb";
  QTest::newRow("tag prefix")<<"v26.1022.942";
  QTest::newRow("pre-#373 major other than 0")<<"1.10.0";
 }
 void rejectsMalformedCalendarVersions(){
  QFETCH(QString,version);
  QCOMPARE(update::compareVersions(version,"0.1.1"),std::nullopt);
  QCOMPARE(update::compareVersions("0.1.1",version),std::nullopt);
 }
 void acceptsRealCalendarDates(){
  QCOMPARE(update::compareVersions("28.229.990","28.228.990"),std::optional(1));
  QCOMPARE(update::compareVersions("26.1231.990","26.101.990"),std::optional(1));
 }
 // #373: a nightly from before calendar versions sees the first calendar nightly as an update.
 void legacyNightlyIsOfferedTheFirstCalendarNightly(){
  const auto build=update::build(manifest("0.1.1-nightly.925"),"appimage",true);Error error;
  const auto result=update::evaluate(feed("nightly",release("26.1022.942-nightly")),build,"nightly",error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["updateAvailable"].toBool(),true);QCOMPARE(result["tag"].toString(),QString("v26.1022.942-nightly"));
  QCOMPARE(result["asset"].toObject()["name"].toString(),QString("putgd-26.1022.942-nightly-x86_64.AppImage"));
  QCOMPARE(update::releaseFileUrl("v26.1022.942-nightly","SHA256SUMS",error),QUrl("https://github.com/patrick-hudson/put.gd-desktop/releases/download/v26.1022.942-nightly/SHA256SUMS"));QVERIFY(!error);
  const auto calendar=update::build(manifest("26.1022.942-nightly"),"appimage",true);
  QCOMPARE(calendar.channel,QString("nightly"));QCOMPARE(calendar.kind,QString("nightly"));
  QCOMPARE(update::build(manifest("26.1101.960"),"appimage",true).channel,QString("stable"));
  // A stable release committed before this nightly is older; a later one is an update.
  QCOMPARE(update::evaluate(feed("stable",release("26.1020.900")),calendar,"stable",error)["updateAvailable"].toBool(),false);QVERIFY(!error);
  QCOMPARE(update::evaluate(feed("stable",release("26.1101.960")),calendar,"stable",error)["updateAvailable"].toBool(),true);QVERIFY(!error);
  QCOMPARE(update::evaluate(feed("nightly",release("26.1022.941-nightly")),calendar,"nightly",error)["updateAvailable"].toBool(),false);QVERIFY(!error);
 }
 void feedAddressIsFixedUnlessALoopbackOrHttpsOverride(){
  qunsetenv("PUTGD_UPDATE_FEED_URL");Error error;
  QCOMPARE(update::feedUrl("nightly",error),QUrl("https://app.put.gd/update/v1/nightly.json"));QVERIFY(!error);
  QCOMPARE(update::feedUrl("stable",error),QUrl("https://app.put.gd/update/v1/stable.json"));QVERIFY(!error);
  update::feedUrl("beta",error);QCOMPARE(error.code,QString("invalid_request"));
  for(const auto& [base,accepted]:std::initializer_list<std::pair<const char*,bool>>{
       {"http://127.0.0.1:8080/feeds",true},{"http://localhost:1/",true},{"https://feeds.example.test/v1/",true},
       {"http://example.com/",false},{"https://example.com/?id=1",false},{"https://user@example.com/",false},{"file:///tmp/",false}}){
   qputenv("PUTGD_UPDATE_FEED_URL",base);error={};const auto url=update::feedUrl("nightly",error);
   QVERIFY2(accepted==!error,base);if(accepted)QVERIFY(url.path().endsWith("/nightly.json"));
  }
  qunsetenv("PUTGD_UPDATE_FEED_URL");
 }
 void newerNightlyOffersTheProjectsOwnLinks(){
  const auto build=update::build(manifest("0.1.1-nightly.588"),"debian13",true);Error error;
  const auto result=update::evaluate(feed("nightly",release("0.1.1-nightly.590")),build,"nightly",error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["updateAvailable"].toBool(),true);QCOMPARE(result["latest"].toString(),QString("0.1.1-nightly.590"));QCOMPARE(result["current"].toString(),QString("0.1.1-nightly.588"));
  // The feed's own notesUrl is ignored: links are built from the tag.
  QCOMPARE(result["notesUrl"].toString(),QString("https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.590"));
  QCOMPARE(result["downloadUrl"].toString(),QString("https://github.com/patrick-hudson/put.gd-desktop/releases/download/v0.1.1-nightly.590/putgd-0.1.1-nightly.590-debian13-x86_64.deb"));
  QCOMPARE(result["publishedAt"].toString(),QString("2026-10-20T05:55:50Z"));
  // What an install fetches (#293 phase 3): this build's own asset, with the feed's size and hash.
  QCOMPARE(result["asset"].toObject(),(QJsonObject{{"name","putgd-0.1.1-nightly.590-debian13-x86_64.deb"},{"size",123},{"sha256",QString(64,'a')}}));
 }
 void releaseFilesComeFromTheReleaseOrTheMirror(){
  for(const char* name:{"PUTGD_UPDATE_RELEASE_URL","PUTGD_UPDATE_MIRROR_URL"})qunsetenv(name);
  Error error;
  QCOMPARE(update::releaseFileUrl("v0.1.1-nightly.801","SHA256SUMS",error),QUrl("https://github.com/patrick-hudson/put.gd-desktop/releases/download/v0.1.1-nightly.801/SHA256SUMS"));QVERIFY(!error);
  QCOMPARE(update::mirrorFileUrl("v0.1.1-nightly.801","putgd-0.1.1-nightly.801-x86_64.AppImage.zsync",error),
           QUrl("https://pkg.put.gd/releases/v0.1.1-nightly.801/putgd-0.1.1-nightly.801-x86_64.AppImage.zsync"));QVERIFY(!error);
  // Only release tags and plain file names.
  for(const auto& [tag,name]:std::initializer_list<std::pair<const char*,const char*>>{{"v0.1.1-nightly.801","../SHA256SUMS"},{"0.1.1","SHA256SUMS"},{"v0.1.1/x","a"},{"v0.1.1",""}}){
   error={};update::releaseFileUrl(tag,name,error);QVERIFY2(error,name);
  }
  qputenv("PUTGD_UPDATE_MIRROR_URL","http://127.0.0.1:9/lab/releases");error={};
  QCOMPARE(update::mirrorFileUrl("v0.1.1","SHA256SUMS",error),QUrl("http://127.0.0.1:9/lab/releases/v0.1.1/SHA256SUMS"));QVERIFY(!error);
  qputenv("PUTGD_UPDATE_RELEASE_URL","http://example.com/releases");error={};
  update::releaseFileUrl("v0.1.1","SHA256SUMS",error);QCOMPARE(error.code,QString("update_download_failed"));
  for(const char* name:{"PUTGD_UPDATE_RELEASE_URL","PUTGD_UPDATE_MIRROR_URL"})qunsetenv(name);
 }
 void sameOrOlderVersionIsUpToDate(){
  const auto build=update::build(manifest("0.1.1-nightly.590"),"appimage",true);Error error;
  QCOMPARE(update::evaluate(feed("nightly",release("0.1.1-nightly.590")),build,"nightly",error)["updateAvailable"].toBool(),false);QVERIFY(!error);
  QCOMPARE(update::evaluate(feed("nightly",release("0.1.1-nightly.500")),build,"nightly",error)["updateAvailable"].toBool(),false);QVERIFY(!error);
  // A nightly build watching stable sees an older stable as no update.
  QCOMPARE(update::evaluate(feed("stable",release("0.1.0")),build,"stable",error)["updateAvailable"].toBool(),false);QVERIFY(!error);
  QCOMPARE(update::evaluate(feed("stable",release("0.1.1")),build,"stable",error)["updateAvailable"].toBool(),true);QVERIFY(!error);
 }
 void channelWithoutAReleaseIsNull(){
  const auto build=update::build(manifest("0.1.1-nightly.588"),"fedora44",true);Error error;
  const auto result=update::evaluate(feed("stable",QJsonValue::Null),build,"stable",error);
  QVERIFY(!error);QVERIFY(result["latest"].isNull());QVERIFY(result["downloadUrl"].isNull());QCOMPARE(result["updateAvailable"].toBool(),false);
 }
 void foreignOrMissingAssetFallsBackToThePermalink(){
  const auto build=update::build(manifest("0.1.1-nightly.588"),"appimage",true);Error error;
  auto entry=release("0.1.1-nightly.590");auto assets=entry["assets"].toObject();
  auto appimage=assets["appimage"].toObject();appimage["url"]="https://evil.example/putgd.AppImage";assets["appimage"]=appimage;entry["assets"]=assets;
  QCOMPARE(update::evaluate(feed("nightly",entry),build,"nightly",error)["downloadUrl"].toString(),QString("https://app.put.gd/dl/nightly/appimage"));QVERIFY(!error);
  // Nothing to install from a foreign link.
  QVERIFY(update::evaluate(feed("nightly",entry),build,"nightly",error)["asset"].isNull());
  const auto ubuntu=update::build(manifest("0.1.1-nightly.588"),"ubuntu26.04",true);
  QCOMPARE(update::evaluate(feed("nightly",release("0.1.1-nightly.590")),ubuntu,"nightly",error)["downloadUrl"].toString(),QString("https://app.put.gd/dl/nightly/ubuntu26.04"));QVERIFY(!error);
 }
 // #344: a switch's new image must follow the other channel's permalink and nothing else.
 void aSwitchMovesOnlyTheChannelOfThePermalink(){
  QCOMPARE(update::switchedUpdateInformation("zsync|https://app.put.gd/dl/nightly/appimage.zsync","nightly","stable"),QString("zsync|https://app.put.gd/dl/stable/appimage.zsync"));
  QCOMPARE(update::switchedUpdateInformation("zsync|https://app.put.gd/dl/stable/appimage.zsync","stable","nightly"),QString("zsync|https://app.put.gd/dl/nightly/appimage.zsync"));
  // The lab's own origin keeps its prefix.
  QCOMPARE(update::switchedUpdateInformation("zsync|http://127.0.0.1:8344/dl/nightly/appimage.zsync","nightly","stable"),QString("zsync|http://127.0.0.1:8344/dl/stable/appimage.zsync"));
  // Not this channel's permalink, the same channel, no update information or an unknown channel: no switch.
  QVERIFY(update::switchedUpdateInformation("zsync|https://app.put.gd/dl/stable/appimage.zsync","nightly","stable").isEmpty());
  QVERIFY(update::switchedUpdateInformation("zsync|https://app.put.gd/dl/nightly/appimage.zsync","nightly","nightly").isEmpty());
  QVERIFY(update::switchedUpdateInformation("","nightly","stable").isEmpty());
  QVERIFY(update::switchedUpdateInformation("zsync|/dl/nightly/appimage.zsync","nightly","stable").isEmpty());
  QVERIFY(update::switchedUpdateInformation("gh-releases-zsync|patrick-hudson|put.gd-desktop|latest|putgd-*.zsync","nightly","stable").isEmpty());
  QVERIFY(update::switchedUpdateInformation("zsync|https://app.put.gd/dl/nightly/appimage.zsync","nightly","beta").isEmpty());
 }
 // #344: which way a switch goes. Before the first stable release, nightly →
 // stable is newer; once nightlies move to the next patch, it is older.
 void switchDirectionFollowsTheVersionOrder(){
  QCOMPARE(update::compareVersions("0.1.1","0.1.1-nightly.900"),std::optional(1));
  QCOMPARE(update::compareVersions("0.1.1","0.1.2-nightly.5"),std::optional(-1));
  QCOMPARE(update::compareVersions("0.1.2-nightly.5","0.1.1"),std::optional(1));
  QCOMPARE(update::compareVersions("0.1.1-nightly.901","0.1.1"),std::optional(-1));
 }
 void malformedFeedsAreRejected_data(){
  QTest::addColumn<QByteArray>("body");QTest::addColumn<QString>("channel");
  QTest::newRow("not json")<<QByteArray("<html>")<<"nightly";
  QTest::newRow("unknown schema")<<QJsonDocument(QJsonObject{{"schema",2},{"channel","nightly"},{"release",QJsonValue::Null}}).toJson()<<"nightly";
  QTest::newRow("other channel")<<feed("stable",QJsonValue::Null)<<"nightly";
  QTest::newRow("release is a string")<<feed("nightly","0.1.1-nightly.590")<<"nightly";
  QTest::newRow("stable version on nightly")<<feed("nightly",release("0.1.2"))<<"nightly";
  QTest::newRow("nightly version on stable")<<feed("stable",release("0.1.2-nightly.3"))<<"stable";
  QTest::newRow("candidate on stable")<<feed("stable",release("0.1.2-rc.1"))<<"stable";
  QTest::newRow("development version")<<feed("nightly",release("0.1.2-dev.3+gabc"))<<"nightly";
  QTest::newRow("calendar stable on nightly")<<feed("nightly",release("26.1101.960"))<<"nightly";
  QTest::newRow("calendar nightly on stable")<<feed("stable",release("26.1022.942-nightly"))<<"stable";
  QTest::newRow("calendar candidate")<<feed("stable",release("26.1101.960-rc"))<<"stable";
  QTest::newRow("calendar leading zero")<<feed("nightly",release("26.0105.942-nightly"))<<"nightly";
  auto mismatched=release("0.1.1-nightly.590");mismatched["tag"]="v0.1.1-nightly.591";
  QTest::newRow("tag mismatch")<<feed("nightly",mismatched)<<"nightly";
 }
 void malformedFeedsAreRejected(){
  QFETCH(QByteArray,body);QFETCH(QString,channel);
  Error error;update::evaluate(body,update::build(manifest("0.1.1-nightly.588"),"debian13",true),channel,error);
  QCOMPARE(error.code,QString("update_feed_invalid"));
 }
};
QTEST_GUILESS_MAIN(UpdateTest)
#include "test_update.moc"
