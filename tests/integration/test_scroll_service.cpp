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
  QCOMPARE(cancelResult["state"].toString(),QString("cancelled"));QVERIFY(!service.active());QCOMPARE(finishes,1);QCOMPARE(ended.stateName(),QString("cancelled"));QVERIFY(ended.cancelled);QVERIFY(ended.error.code.isEmpty());QCOMPARE(admissions,0);
  QVERIFY(!QFileInfo::exists(backend->directory));
 }
 // #1515: who turns the page and what the bar says reach an agent; auto-scroll
 // turns on and off on request (pause and resume are its names from #1514);
 // a pause by the mouse says so; Done keeps the image (whole as far as it was
 // scrolled: not truncated) and Cancel discards it (no admission, no files).
 void drivenCaptureEnds_data(){QTest::addColumn<QString>("end");QTest::newRow("done keeps")<<QString("stop");QTest::newRow("cancel discards")<<QString("cancel");}
 void drivenCaptureEnds(){
  QFETCH(QString,end);QTemporaryDir dir("/tmp/sp-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
  ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="driving";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};
  Operation ended;int admissions=0;host.finish=[&](Operation op){ended=op;};host.admit=[&](Operation,ClipboardPayload,auto){++admissions;};
  ScrollCaptureService service(dir.filePath("staging"),host);auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();service.setBackend(std::move(fake));
  QString id;service.command("prepare",{{"rect",QJsonArray{0,0,64,64}},{"driver","putgd"}},[&](Error e,QJsonObject r){QVERIFY(!e);id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  QCOMPARE(backend->preparedParams["driver"].toString(),QString("putgd"));QCOMPARE(backend->preparedParams["fromTop"].toBool(true),false);
  bool refused=false;service.command("prepare",{{"driver","nobody"}},[&](Error e,QJsonObject){refused=e.code=="invalid_request";});PUTGD_TRY_VERIFY(refused);
  bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY(!e);started=true;});PUTGD_TRY_VERIFY(started);
  QJsonObject view;const auto inspect=[&]{view={};service.command("inspect",{{"id",id}},[&](Error e,QJsonObject r){QVERIFY(!e);view=r;});return !view.isEmpty();};
  QVERIFY(inspect());QCOMPARE(view["driver"].toString(),QString("putgd"));QCOMPARE(view["paused"].toBool(true),false);
  backend->sink({{"id",id},{"event","progress"},{"state","paused"},{"driver","user"},{"paused",true},{"pausedBy","pointer"},{"height",1640},{"placed",31},{"skipped",2},{"steps",3},{"pixelsPerNotch",53}});
  PUTGD_TRY_VERIFY(inspect()&&view["paused"].toBool());QCOMPARE(view["pausedBy"].toString(),QString("pointer"));QCOMPARE(view["driver"].toString(),QString("user"));QCOMPARE(view["state"].toString(),QString("capturing"));
  QCOMPARE(view["activity"].toString(),QString("paused"));QCOMPARE(view["height"].toInt(),1640);QCOMPARE(view["placed"].toInt(),31);QCOMPARE(view["skipped"].toInt(),2);
  for(const auto& [command,on]:{std::pair<QString,bool>{"autoScroll",true},{"autoScroll",false},{"resume",true},{"pause",false}}){
   bool answered=false;auto params=QJsonObject{{"id",id}};if(command=="autoScroll")params["on"]=on;
   service.command(command,params,[&,on=on](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(e.code));QCOMPARE(r["driver"].toString(),QString(on?"putgd":"user"));QVERIFY(!r.contains("pausedBy"));answered=true;});PUTGD_TRY_VERIFY(answered);
  }
  QCOMPARE(backend->commands.mid(backend->commands.indexOf("start")+1),(QStringList{"autoScroll","autoScroll","resume","pause"}));
  refused=false;service.command("autoScroll",{{"id",id}},[&](Error e,QJsonObject){refused=e.code=="invalid_request";});PUTGD_TRY_VERIFY(refused);
  bool done=false;Error error;service.command(end,{{"id",id}},[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));
  QVERIFY(!service.active());QVERIFY(inspect());QVERIFY(!view.contains("paused")&&!view.contains("pausedBy")&&!view.contains("activity"));
  if(end=="stop"){QCOMPARE(view["state"].toString(),QString("done"));QCOMPARE(view["endReason"].toString(),QString("done"));QCOMPARE(view["truncated"].toBool(true),false);QCOMPARE(view["scrolledBy"].toString(),QString("user"));PUTGD_TRY_VERIFY(admissions==1);}
  else{QCOMPARE(view["state"].toString(),QString("cancelled"));QVERIFY(ended.cancelled);PUTGD_COMPARE_STEADY(admissions,0,200);QVERIFY(!QFileInfo::exists(backend->directory));}
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
 // What each end reason promises (#1515), checked before an image is admitted:
 // end_of_content only for auto-scroll at the end with no frame skipped and the
 // top proved where asked; lost_track only for a capture that ended lost;
 // truncated exactly for height_cap, lost_track and top_not_reached. A helper
 // that breaks a promise is refused; a kept partial image is marked and noted.
 void endReasons_data(){
  QTest::addColumn<QString>("reason");QTest::addColumn<bool>("missed");QTest::addColumn<QJsonObject>("overrides");QTest::addColumn<bool>("admitted");QTest::addColumn<QString>("note");
  const QJsonObject none;
  QTest::newRow("done")<<"done"<<false<<none<<true<<"";
  QTest::newRow("done, auto-scroll at the end after skipped frames")<<"done"<<false<<QJsonObject{{"reachedEnd",true},{"framesSkipped",3},{"scrolledBy","putgd"}}<<true<<"";
  QTest::newRow("end of content")<<"end_of_content"<<false<<QJsonObject{{"scrolledBy","putgd"}}<<true<<"";
  QTest::newRow("end of content with a skipped frame")<<"end_of_content"<<false<<QJsonObject{{"framesSkipped",1}}<<false<<"";
  QTest::newRow("end of content, end not reached")<<"end_of_content"<<false<<QJsonObject{{"reachedEnd",false}}<<false<<"";
  QTest::newRow("end of content, top unproved")<<"end_of_content"<<true<<none<<false<<"";
  QTest::newRow("top not reached")<<"top_not_reached"<<true<<none<<true<<"Could not reach the top of the page. The image starts partway down.";
  QTest::newRow("top not reached, flag missing")<<"top_not_reached"<<false<<none<<false<<"";
  QTest::newRow("lost track")<<"lost_track"<<false<<none<<true<<"Finished while put.gd had lost its place. The image ends where it last found the page.";
  QTest::newRow("lost track, not lost")<<"lost_track"<<false<<QJsonObject{{"endedLost",false}}<<false<<"";
  QTest::newRow("height cap")<<"height_cap"<<false<<none<<true<<"Stopped at 30,000 px. The page continues below.";
  QTest::newRow("done marked truncated")<<"done"<<false<<QJsonObject{{"truncated",true}}<<false<<"";
  QTest::newRow("an end reason of the old loop")<<"stopped"<<false<<none<<false<<"";
  QTest::newRow("no frame placed")<<"done"<<false<<QJsonObject{{"framesPlaced",0}}<<false<<"";
 }
 void endReasons(){
  QFETCH(QString,reason);QFETCH(bool,missed);QFETCH(QJsonObject,overrides);QFETCH(bool,admitted);QFETCH(QString,note);
  QTemporaryDir dir("/tmp/ps-XXXXXX");ProviderRegistry registry;SettingsService settings(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(settings.save(doc,{}));Source source;OperationService operations(dir.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);
  auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();backend->endReason=reason;backend->topMissed=missed;backend->overrides=overrides;operations.setScrollBackend(std::move(fake));QVERIFY(waitFor([&]{return operations.ready();}));
  QVERIFY(dir.filePath("runtime/session.sock").toUtf8().size()<=107);QVERIFY(server.listen(dir.filePath("runtime")));
  auto call=[&](QString method,QJsonObject p=QJsonObject{}){QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),method,QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact))});if(!waitFor([&]{return client.state()==QProcess::NotRunning;}))qFatal("CLI timed out");const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();const auto e=response["error"].toObject();return qMakePair(Error{e["code"].toString(),e["message"].toString(),e["details"].toObject()},response["result"].toObject());};
  auto prepared=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});QVERIFY2(!prepared.first,qPrintable(prepared.first.message));const auto id=prepared.second["id"].toString();QVERIFY(!id.isEmpty());
  // The settings' defaults: the capture waits for the user, and auto-scroll does not go to the top first.
  QCOMPARE(backend->preparedParams["driver"].toString(),QString("user"));QCOMPARE(backend->preparedParams["fromTop"].toBool(true),false);
  QVERIFY(!call("scroll.start",{{"id",id}}).first);QSignalSpy warning(&operations.scroll(),&ScrollCaptureService::warning);
  const auto stopped=call("scroll.stop",{{"id",id}});const auto result=call("scroll.inspect",{{"id",id}}).second;
  if(!admitted){QCOMPARE(stopped.first.code.isEmpty()?result["error"].toObject()["code"].toString():stopped.first.code,QString("scroll_output_invalid"));QCOMPARE(result["state"].toString(),QString("failed"));QCOMPARE(warning.size(),0);HistoryStore store(operations.databasePath());QVERIFY(store.operation(id).items.isEmpty());return;}
  QVERIFY2(!stopped.first,qPrintable(stopped.first.message));QCOMPARE(result["state"].toString(),QString("done"));QCOMPARE(result["endReason"].toString(),reason);QCOMPARE(stopped.second["endReason"].toString(),reason);
  QCOMPARE(result["truncated"].toBool(),reason=="height_cap"||reason=="lost_track"||reason=="top_not_reached");
  for(const auto* key:{"scrolledBy","framesPlaced","framesSkipped","endedLost","reachedEnd"})QVERIFY2(result.contains(key)&&result[key]==result["result"].toObject()[key],key);
  QCOMPARE(result.contains("topReached"),missed);QCOMPARE(result["result"].toObject().contains("topReached"),missed);
  QCOMPARE(warning.size(),note.isEmpty()?0:1);
  HistoryStore store(operations.databasePath());QVERIFY(waitFor([&]{return !store.operation(id).items.isEmpty();}));
  if(!note.isEmpty()){QCOMPARE(warning[0][2].toString(),reason);QCOMPARE(warning[0][1].toString(),note);QCOMPARE(result["note"].toString(),note);QCOMPARE(store.operation(id).note,note);}
  else QVERIFY(store.operation(id).note.isEmpty());
 }
};
QTEST_GUILESS_MAIN(ScrollTest)
#include "test_scroll_service.moc"
