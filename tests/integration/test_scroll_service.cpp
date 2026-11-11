#include "fixtures/U2Fixtures.h"
#include "fixtures/FakeScrollBackend.h"
#include "fixtures/Waits.h"
#include "app/LocalServer.h"
#include <QSignalSpy>
#include <QProcess>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <utility>
using namespace putgd;using namespace putgd::test;
struct ScrollShellFixture final:RecordingShell {
 Reply selection;int cancels=0;
 // holdFrames: the Shell is still placing the bar; `placed` answers when the test says so.
 bool holdFrames=false;Reply placed;
 bool selectsRegions()const override{return true;}
 void selectRegion(Reply reply)override{selection=std::move(reply);}
 void cancelSelection()override{++cancels;auto reply=std::exchange(selection,{});if(reply)reply({"cancelled","Selection cancelled",{}},{});}
 void showFrame(const QJsonObject& frame,Reply reply)override{if(holdFrames&&frame["state"]=="ready"&&!placed){placed=std::move(reply);return;}reply({},{});}
 void hideFrame()override{}
};
class ScrollTest:public QObject {
 Q_OBJECT
private slots:
 void cancelPendingCommand_data(){QTest::addColumn<QString>("command");QTest::newRow("start")<<QString("start");QTest::newRow("stop")<<QString("stop");}
 void cancelPendingCommand(){
  QFETCH(QString,command);QTemporaryDir dir("/tmp/sc-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
  ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="pending";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};host.finish=[](Operation){};
  ScrollCaptureService service(dir.filePath("staging"),host);auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();service.setBackend(std::move(fake));
  QString id;service.command("prepare",{{"rect",QJsonArray{0,0,64,64}}},[&](Error e,QJsonObject r){QVERIFY(!e);id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  if(command=="stop"){bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY(!e);started=true;});PUTGD_TRY_VERIFY(started);}
  backend->holdCommand=command;bool pendingAnswered=false;service.command(command,{{"id",id}},[&](Error,QJsonObject){pendingAnswered=true;});QVERIFY(bool(backend->heldCommand));
  bool cancelled=false;service.command("cancel",{{"id",id}},[&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["state"].toString(),QString("cancelled"));cancelled=true;});PUTGD_TRY_VERIFY(cancelled);QVERIFY(!service.active());
  std::exchange(backend->heldCommand,{})({},{{"state","capturing"}});PUTGD_TRY_VERIFY(pendingAnswered);QVERIFY(!service.active());
 }
 void helperReleasedBeforeAdmission(){
  QTemporaryDir dir("/tmp/sc-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
  ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="admission";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};host.finish=[](Operation){};
  auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();bool admitted=false;std::function<void()> release;
  host.admit=[&](Operation,ClipboardPayload payload,auto released){QCOMPARE(backend->discarded,1);QVERIFY(!payload.bytes.isEmpty());release=std::move(released);admitted=true;};
  ScrollCaptureService service(dir.filePath("staging"),host);service.setBackend(std::move(fake));QString id;
  service.command("prepare",{{"rect",QJsonArray{0,0,64,64}}},[&](Error e,QJsonObject r){QVERIFY(!e);id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY(!e);started=true;});PUTGD_TRY_VERIFY(started);
  service.command("stop",{{"id",id}},[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_VERIFY(admitted);QVERIFY(!service.active());release();
 }
 void lifecycle_data(){QTest::addColumn<QString>("bad");for(const auto& s:QStringList{"","hash","size","ihdr"})QTest::newRow(qPrintable(s.isEmpty()?"valid":s))<<s;}
 void lifecycle(){
 QFETCH(QString,bad);QTemporaryDir dir("/tmp/ps-XXXXXX");ProviderRegistry registry;SettingsService settings(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(settings.save(doc,{}));Source source;OperationService operations(dir.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);
 auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();backend->corruption=bad;backend->heightCap=true;bool acceptedBeforeFrames=false;backend->onPrepare=[&](const QString& id){HistoryStore store(operations.databasePath());acceptedBeforeFrames=store.operation(id).id==id;};operations.setScrollBackend(std::move(fake));QVERIFY(waitFor([&]{return operations.ready();}));
 QVERIFY(dir.filePath("runtime/session.sock").toUtf8().size()<=107);QVERIFY(server.listen(dir.filePath("runtime")));
 auto call=[&](QString method,QJsonObject p=QJsonObject{}){QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),method,QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact))});if(!waitFor([&]{return client.state()==QProcess::NotRunning;}))qFatal("CLI timed out");const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();const auto e=response["error"].toObject();return qMakePair(Error{e["code"].toString(),e["message"].toString(),e["details"].toObject()},response["result"].toObject());};
 QVERIFY(call("scroll.capabilities").second["available"].toBool());QVERIFY(call("scroll.prepare",{{"maxHeight",1}}).first);
 auto prepared=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});QVERIFY2(!prepared.first,qPrintable(prepared.first.message));const auto id=prepared.second["id"].toString();QVERIFY(!id.isEmpty());QVERIFY(acceptedBeforeFrames);QVERIFY(backend->probes>=2);QVERIFY(backend->preparedParams.contains("maxPixels"));QVERIFY(!backend->preparedParams.contains("imagePixels"));QVERIFY(!backend->preparedParams.contains("imageBytes"));
 QCOMPARE(call("action.invoke",{{"action","region"}}).first.code,QString("scroll_active"));QCOMPARE(call("recording.prepare",{{"target",QJsonObject{{"kind","region"}}},{"format","mp4"}}).first.code,QString("scroll_active"));
 QVERIFY(!call("scroll.start",{{"id",id}}).first);QSignalSpy warning(&operations.scroll(),&ScrollCaptureService::warning);call("scroll.stop",{{"id",id}});auto result=call("scroll.inspect",{{"id",id}}).second;
 QCOMPARE(result["state"].toString(),bad.isEmpty()?QString("done"):QString("failed"));if(bad.isEmpty()){QCOMPARE(warning.size(),1);QVERIFY(result["note"].toString().contains("30,000"));QCOMPARE(result["endReason"].toString(),QString("height_cap"));QVERIFY(result["truncated"].toBool());HistoryStore store(operations.databasePath());QVERIFY(waitFor([&]{return !store.operation(id).items.isEmpty();}));QCOMPARE(store.operation(id).note,result["note"].toString());}
 backend->stopEmpty=true;
 auto empty=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});const auto emptyId=empty.second["id"].toString();QVERIFY(!emptyId.isEmpty());QVERIFY(!call("scroll.start",{{"id",emptyId}}).first);call("scroll.stop",{{"id",emptyId}});QCOMPARE(call("scroll.inspect",{{"id",emptyId}}).second["state"].toString(),QString("cancelled"));{HistoryStore store(operations.databasePath());QVERIFY(store.operation(emptyId).items.isEmpty());}
 ScrollShellFixture shell;operations.scroll().setShell(&shell);auto next=call("scroll.prepare");const auto nextId=next.second["id"].toString();QVERIFY(!nextId.isEmpty());QVERIFY(!call("scroll.select",{{"id",nextId},{"rect",QJsonArray{0,0,64,64}}}).first);QCOMPARE(shell.cancels,1);QVERIFY(!call("scroll.cancel",{{"id",nextId}}).first);QCOMPARE(call("scroll.inspect",{{"id",nextId}}).second["state"].toString(),QString("cancelled"));
 backend->commands.clear();auto interactive=call("action.invoke",{{"action","scroll"}});const auto interactiveId=interactive.second["id"].toString();QVERIFY(!interactiveId.isEmpty());QVERIFY(!backend->preparedParams["autoStart"].toBool());QCOMPARE(backend->preparedParams["selection"].toString(),QString("shell"));QVERIFY(bool(shell.selection));auto selected=std::exchange(shell.selection,{});selected({},{{"rect",QJsonArray{0,0,64,64}}});QVERIFY(waitFor([&]{return backend->commands.contains("start");}));QCOMPARE(backend->commands,QStringList({"select","overlay","start"}));QVERIFY(!call("scroll.cancel",{{"id",interactiveId}}).first);
 // GNOME answers `ready` while the Shell still places the bar (real desktop,
 // 2026-11-10: an immediate scroll.start got scroll_busy). The start waits
 // for the placement and then runs; a second one meanwhile is refused, and a
 // session that ends meanwhile ends the waiting start with it.
 backend->commands.clear();backend->started=false;shell.holdFrames=true;
 auto placing=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});const auto placingId=placing.second["id"].toString();QVERIFY(!placingId.isEmpty());QCOMPARE(placing.second["state"].toString(),QString("ready"));QVERIFY(bool(shell.placed));
 QProcess early;early.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"scroll.start",QString::fromUtf8(QJsonDocument(QJsonObject{{"id",placingId}}).toJson(QJsonDocument::Compact))});
 PUTGD_VERIFY_STEADY(early.state()==QProcess::Running&&!backend->started,400);
 QCOMPARE(call("scroll.start",{{"id",placingId}}).first.code,QString("scroll_busy"));
 std::exchange(shell.placed,{})({},{});QVERIFY(waitFor([&]{return early.state()==QProcess::NotRunning;}));
 const auto earlyReply=QJsonDocument::fromJson(early.readAllStandardOutput()).object();QVERIFY2(earlyReply["error"].toObject()["code"].toString().isEmpty(),qPrintable(QJsonDocument(earlyReply).toJson()));QCOMPARE(earlyReply["result"].toObject()["state"].toString(),QString("capturing"));
 QVERIFY(backend->started);QCOMPARE(backend->commands,QStringList({"overlay","start"}));QVERIFY(!call("scroll.cancel",{{"id",placingId}}).first);
 backend->commands.clear();backend->started=false;
 auto ending=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});const auto endingId=ending.second["id"].toString();QVERIFY(!endingId.isEmpty());QVERIFY(bool(shell.placed));
 QProcess waiting;waiting.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"scroll.start",QString::fromUtf8(QJsonDocument(QJsonObject{{"id",endingId}}).toJson(QJsonDocument::Compact))});
 PUTGD_VERIFY_STEADY(waiting.state()==QProcess::Running,300);QVERIFY(!call("scroll.cancel",{{"id",endingId}}).first);QVERIFY(waitFor([&]{return waiting.state()==QProcess::NotRunning;}));
 QCOMPARE(QJsonDocument::fromJson(waiting.readAllStandardOutput()).object()["error"].toObject()["code"].toString(),QString("cancelled"));QVERIFY(!backend->started);
 std::exchange(shell.placed,{})({},{});shell.holdFrames=false;
 // holdStart: whoever cleared the screen for the capture gets its time; start waits, it is not refused.
 operations.scroll().setShell(nullptr);backend->commands.clear();backend->started=false;
 QMetaObject::Connection hold=connect(&operations.scroll(),&ScrollCaptureService::preparing,&operations,[&]{operations.scroll().holdStart(600);});QSignalSpy ended(&operations.scroll(),&ScrollCaptureService::ended);
 auto held=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});const auto heldId=held.second["id"].toString();QVERIFY(!heldId.isEmpty());QElapsedTimer since;since.start();
 QVERIFY(!call("scroll.start",{{"id",heldId}}).first);QVERIFY(backend->started);QVERIFY2(since.elapsed()>=450,qPrintable(QString::number(since.elapsed())));
 QCOMPARE(ended.size(),0);QVERIFY(!call("scroll.cancel",{{"id",heldId}}).first);QCOMPARE(ended.size(),1);disconnect(hold);
 // What the hold waited for has happened: said `fromNow`, a long hold ends that much later, also for a start already waiting.
 backend->started=false;hold=connect(&operations.scroll(),&ScrollCaptureService::preparing,&operations,[&]{operations.scroll().holdStart(4000);});
 auto moved=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});const auto movedId=moved.second["id"].toString();QVERIFY(!movedId.isEmpty());since.restart();
 QTimer::singleShot(250,&operations,[&]{operations.scroll().holdStart(200,true);});
 QVERIFY(!call("scroll.start",{{"id",movedId}}).first);QVERIFY(backend->started);QVERIFY2(since.elapsed()>=400&&since.elapsed()<2500,qPrintable(QString::number(since.elapsed())));
 QVERIFY(!call("scroll.cancel",{{"id",movedId}}).first);disconnect(hold);
 // A start the hold keeps back when its session is cancelled is answered, not left waiting.
 backend->started=false;hold=connect(&operations.scroll(),&ScrollCaptureService::preparing,&operations,[&]{operations.scroll().holdStart(700);});
 auto dropped=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});const auto droppedId=dropped.second["id"].toString();QVERIFY(!droppedId.isEmpty());
 bool answered=false;Error late;operations.scroll().command("start",{{"id",droppedId}},[&](Error e,QJsonObject){late=e;answered=true;});
 QVERIFY(!call("scroll.cancel",{{"id",droppedId}}).first);QVERIFY(waitFor([&]{return answered;}));QCOMPARE(late.code,QString("scroll_not_found"));QVERIFY(!backend->started);disconnect(hold);

 }
 // The scroll-to-top step could not prove the top (final pass of 2026-11-11:
 // an image that began at line 78 of the page was admitted as end_of_content).
 // The image is kept, routed and marked; it is never called complete, and a
 // helper that calls it complete is refused.
 void topNotReached_data(){
  QTest::addColumn<QString>("reason");QTest::addColumn<bool>("missed");QTest::addColumn<bool>("admitted");QTest::addColumn<QString>("warned");
  QTest::newRow("top not reached")<<"top_not_reached"<<true<<true<<"top_not_reached";
  QTest::newRow("stop after an unproved top")<<"stopped"<<true<<true<<"";
  QTest::newRow("height cap after an unproved top")<<"height_cap"<<true<<true<<"height_cap";
  QTest::newRow("top not reached, flag missing")<<"top_not_reached"<<false<<false<<"";
  QTest::newRow("end of content, top unproved")<<"end_of_content"<<true<<false<<"";
  QTest::newRow("end of content")<<"end_of_content"<<false<<true<<"";
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
