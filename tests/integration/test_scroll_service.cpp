#include "fixtures/U2Fixtures.h"
#include "fixtures/FakeScrollBackend.h"
#include "fixtures/Waits.h"
#include "app/LocalServer.h"
#include <QSignalSpy>
#include <QProcess>
#include <QJsonDocument>
#include <utility>
using namespace putgd;using namespace putgd::test;
class ScrollTest:public QObject {
 Q_OBJECT
private slots:
 // Stop and Cancel sent a millisecond apart (acceptance of 2026-11-11): once a
 // Cancel is on its way it decides. An image the Stop finished, or a finished
 // event, is never admitted (copied or uploaded) for a capture the user cancelled.
 void cancelInFlightDecides_data(){
  QTest::addColumn<QString>("order");
  QTest::newRow("stop finishes its image while the cancel is on its way")<<QString("stop-cancel-finished");
  QTest::newRow("the finished event arrives while the cancel is on its way")<<QString("event");
 }
 void cancelInFlightDecides(){
  QFETCH(QString,order);QTemporaryDir dir("/tmp/sc-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
  ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="deciding";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};
  Operation ended;int finishes=0,admissions=0;host.finish=[&](Operation op){ended=op;++finishes;};host.admit=[&](Operation,ClipboardPayload,auto){++admissions;};
  ScrollCaptureService service(dir.filePath("staging"),host);auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();service.setBackend(std::move(fake));
  QString id;service.command("prepare",{{"rect",QJsonArray{0,0,64,64}}},[&](Error e,QJsonObject r){QVERIFY(!e);id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY(!e);started=true;});PUTGD_TRY_VERIFY(started);
  backend->commands.clear();backend->holdCommands={"stop","cancel"};
  bool stopAnswered=false,cancelAnswered=false;Error stopError;QJsonObject cancelResult;
  const auto stop=[&]{service.command("stop",{{"id",id}},[&](Error e,QJsonObject){stopError=e;stopAnswered=true;});};
  const auto cancel=[&]{service.command("cancel",{{"id",id}},[&](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(e.code));cancelResult=r;cancelAnswered=true;});};
  if(order=="event"){
   cancel();QVERIFY(backend->held.contains("cancel"));auto announced=backend->finished();announced["id"]=id;announced["event"]="finished";backend->sink(announced);
   PUTGD_VERIFY_STEADY(service.active()&&admissions==0,200);
  }else{
   stop();QVERIFY(backend->held.contains("stop"));cancel();QVERIFY(backend->held.contains("cancel"));
   backend->held.take("stop")({},backend->finished());
   PUTGD_TRY_VERIFY(stopAnswered);QCOMPARE(stopError.code,QString("cancelled"));QVERIFY(service.active());QCOMPARE(finishes,0);
  }
  QCOMPARE(admissions,0);QVERIFY(!cancelAnswered);
  backend->held.take("cancel")({},{});PUTGD_TRY_VERIFY(cancelAnswered);
  QCOMPARE(cancelResult["state"].toString(),QString("cancelled"));QVERIFY(!service.active());QCOMPARE(finishes,1);QCOMPARE(ended.state,QString("cancelled"));QVERIFY(ended.cancelled);QVERIFY(ended.error.code.isEmpty());QCOMPARE(admissions,0);
  QVERIFY(!QFileInfo::exists(backend->directory));
 }
 // A PNG that does not match the helper's descriptor (other bytes, other
 // dimensions) is never admitted; and the helper is given the pixel bound.
 void corruptOutputFails_data(){QTest::addColumn<QString>("bad");QTest::newRow("hash")<<QString("hash");QTest::newRow("ihdr")<<QString("ihdr");}
 void corruptOutputFails(){
 QFETCH(QString,bad);QTemporaryDir dir("/tmp/ps-XXXXXX");ProviderRegistry registry;SettingsService settings(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(settings.save(doc,{}));Source source;OperationService operations(dir.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);
 auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();backend->corruption=bad;operations.setScrollBackend(std::move(fake));QVERIFY(waitFor([&]{return operations.ready();}));
 QVERIFY(dir.filePath("runtime/session.sock").toUtf8().size()<=107);QVERIFY(server.listen(dir.filePath("runtime")));
 auto call=[&](QString method,QJsonObject p=QJsonObject{}){QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),method,QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact))});if(!waitFor([&]{return client.state()==QProcess::NotRunning;}))qFatal("CLI timed out");const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();const auto e=response["error"].toObject();return qMakePair(Error{e["code"].toString(),e["message"].toString(),e["details"].toObject()},response["result"].toObject());};
 auto prepared=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});QVERIFY2(!prepared.first,qPrintable(prepared.first.message));const auto id=prepared.second["id"].toString();QVERIFY(!id.isEmpty());QVERIFY(backend->preparedParams.contains("maxPixels"));
 QVERIFY(!call("scroll.start",{{"id",id}}).first);call("scroll.stop",{{"id",id}});
 QCOMPARE(call("scroll.inspect",{{"id",id}}).second["state"].toString(),QString("failed"));HistoryStore store(operations.databasePath());QVERIFY(store.operation(id).items.isEmpty());
 }
 // The scroll-to-top step could not prove the top (final pass of 2026-11-11:
 // an image that began at line 78 of the page was admitted as end_of_content).
 // The image is kept, routed and marked; it is never called complete, and a
 // helper that calls it complete is refused.
 void topNotReached_data(){
  QTest::addColumn<QString>("reason");QTest::addColumn<bool>("missed");QTest::addColumn<bool>("admitted");QTest::addColumn<QString>("warned");
  QTest::newRow("top not reached")<<"top_not_reached"<<true<<true<<"top_not_reached";
  QTest::newRow("top not reached, flag missing")<<"top_not_reached"<<false<<false<<"";
  QTest::newRow("end of content, top unproved")<<"end_of_content"<<true<<false<<"";
 }
 void topNotReached(){
  QFETCH(QString,reason);QFETCH(bool,missed);QFETCH(bool,admitted);QFETCH(QString,warned);
  QTemporaryDir dir("/tmp/ps-XXXXXX");ProviderRegistry registry;SettingsService settings(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(settings.save(doc,{}));Source source;OperationService operations(dir.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);
  auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();backend->endReason=reason;backend->topMissed=missed;operations.setScrollBackend(std::move(fake));QVERIFY(waitFor([&]{return operations.ready();}));
  QVERIFY(dir.filePath("runtime/session.sock").toUtf8().size()<=107);QVERIFY(server.listen(dir.filePath("runtime")));
  auto call=[&](QString method,QJsonObject p=QJsonObject{}){QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),method,QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact))});if(!waitFor([&]{return client.state()==QProcess::NotRunning;}))qFatal("CLI timed out");const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();const auto e=response["error"].toObject();return qMakePair(Error{e["code"].toString(),e["message"].toString(),e["details"].toObject()},response["result"].toObject());};
  auto prepared=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});QVERIFY2(!prepared.first,qPrintable(prepared.first.message));const auto id=prepared.second["id"].toString();QVERIFY(!id.isEmpty());
  QVERIFY(backend->preparedParams["fromTop"].toBool());
  QVERIFY(!call("scroll.start",{{"id",id}}).first);QSignalSpy warning(&operations.scroll(),&ScrollCaptureService::warning);
  const auto stopped=call("scroll.stop",{{"id",id}});const auto result=call("scroll.inspect",{{"id",id}}).second;
  if(!admitted){QCOMPARE(stopped.first.code.isEmpty()?result["error"].toObject()["code"].toString():stopped.first.code,QString("scroll_output_invalid"));QCOMPARE(result["state"].toString(),QString("failed"));QCOMPARE(warning.size(),0);HistoryStore store(operations.databasePath());QVERIFY(store.operation(id).items.isEmpty());return;}
  QVERIFY2(!stopped.first,qPrintable(stopped.first.message));QCOMPARE(result["state"].toString(),QString("done"));QCOMPARE(result["endReason"].toString(),reason);QCOMPARE(stopped.second["endReason"].toString(),reason);
  // Only end_of_content is the whole page; the mark travels with every other end, at the top level and in the result.
  QCOMPARE(result["truncated"].toBool(),reason!="end_of_content");QCOMPARE(result.contains("topReached"),missed);QCOMPARE(result["result"].toObject().contains("topReached"),missed);
  if(missed){QCOMPARE(result["topReached"].toBool(true),false);QCOMPARE(result["result"].toObject()["topReached"].toBool(true),false);}
  QCOMPARE(warning.size(),warned.isEmpty()?0:1);
  HistoryStore store(operations.databasePath());QVERIFY(waitFor([&]{return !store.operation(id).items.isEmpty();}));
  if(!warned.isEmpty())QCOMPARE(warning[0][2].toString(),warned);
  if(reason=="top_not_reached"){const QString note("Could not reach the top of the page. The image starts partway down.");QCOMPARE(warning[0][1].toString(),note);QCOMPARE(result["note"].toString(),note);QCOMPARE(store.operation(id).note,note);}
  else if(warned.isEmpty())QVERIFY(store.operation(id).note.isEmpty());
 }
};
QTEST_GUILESS_MAIN(ScrollTest)
#include "test_scroll_service.moc"
