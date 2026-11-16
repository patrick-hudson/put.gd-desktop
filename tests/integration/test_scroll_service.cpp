#include "fixtures/U2Fixtures.h"
#include "fixtures/FakeScrollBackend.h"
#include "fixtures/Waits.h"
#include "app/LocalServer.h"
#include <QSignalSpy>
#include <QProcess>
#include <QJsonDocument>
#include <utility>
using namespace putgd;using namespace putgd::test;
namespace {
// The Shell on GNOME, as far as a scroll capture uses it (#1531: its pointer watch).
struct FakeShell final:RecordingShell {
 bool capable=true,watching=false;int watches=0,unwatches=0,hides=0;std::function<void(QJsonObject)> sink;
 bool selectsRegions()const override{return true;}
 void selectRegion(Reply reply)override{reply({},{{"rect",QJsonArray{0,0,64,64}}});}
 void cancelSelection()override{}
 void showFrame(const QJsonObject&,Reply reply)override{reply({},{{"bar",QJsonObject{{"placed",true},{"outside",true},{"rect",QJsonArray{0,70,64,20}}}}});}
 void hideFrame()override{++hides;}
 bool watchesPointer()const override{return capable;}
 void watchPointer(std::function<void(QJsonObject)> s)override{sink=std::move(s);watching=true;++watches;}
 void unwatchPointer()override{if(watching)++unwatches;watching=false;sink={};}
};
}
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
  // A toggle the helper refuses leaves the capture running (#1520's review: a refused resume ended it and discarded its image).
  backend->refuseToggle=true;
  for(const auto& command:{QString("autoScroll"),QString("resume"),QString("pause")}){
   Error refusal;bool answered=false;auto params=QJsonObject{{"id",id}};if(command=="autoScroll")params["on"]=true;
   service.command(command,params,[&](Error e,QJsonObject){refusal=e;answered=true;});PUTGD_TRY_VERIFY(answered);
   QCOMPARE(refusal.code,QString("scroll_state"));QVERIFY(service.active());QVERIFY(inspect());QCOMPARE(view["state"].toString(),QString("capturing"));QVERIFY(QFileInfo::exists(backend->directory));
  }
  backend->refuseToggle=false;
  // A helper that sends more statistics than it should: only the known ones, as numbers, reach the view.
  backend->extraStats={{"steps",7},{"pixelsPerNotch",53.0},{"lowConfidenceSteps",0},{"elapsedMs",1234.0},{"rowsAtDone",12.0},{"id","forged"},{"bogus",1},{"frames","many"}};
  backend->overrides={{"hiddenCells",96},{"hiddenRows","x"}};
  bool done=false;Error error;service.command(end,{{"id",id}},[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));
  QVERIFY(!service.active());QVERIFY(inspect());QVERIFY(!view.contains("paused")&&!view.contains("pausedBy")&&!view.contains("activity"));
  if(end=="stop"){QCOMPARE(view["state"].toString(),QString("done"));QCOMPARE(view["endReason"].toString(),QString("done"));QCOMPARE(view["truncated"].toBool(true),false);QCOMPARE(view["scrolledBy"].toString(),QString("user"));PUTGD_TRY_VERIFY(admissions==1);
   QCOMPARE(view["id"].toString(),id);QCOMPARE(view["steps"].toInt(),7);QCOMPARE(view["rowsAtDone"].toInt(),12);QVERIFY(!view.contains("bogus"));QVERIFY(!view.contains("frames"));QCOMPARE(view["hiddenCells"].toInt(),96);QVERIFY(!view.contains("hiddenRows"));}
  else{QCOMPARE(view["state"].toString(),QString("cancelled"));QVERIFY(ended.cancelled);PUTGD_COMPARE_STEADY(admissions,0,200);QVERIFY(!QFileInfo::exists(backend->directory));}
 }
 // A toggle sent while the capture stops (Done on its way) is refused as the
 // state's, scroll_state, like the helper's own; the stop goes on and keeps the image.
 void toggleWhileStoppingIsState(){
  QTemporaryDir dir("/tmp/st-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
  ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="stopping";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};
  int admissions=0;host.finish=[](Operation){};host.admit=[&](Operation,ClipboardPayload,auto){++admissions;};
  ScrollCaptureService service(dir.filePath("staging"),host);auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();service.setBackend(std::move(fake));
  QString id;service.command("prepare",{{"rect",QJsonArray{0,0,64,64}},{"driver","putgd"}},[&](Error e,QJsonObject r){QVERIFY(!e);id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY(!e);started=true;});PUTGD_TRY_VERIFY(started);
  backend->holdCommands={"stop"};
  bool stopped=false;Error stopError;service.command("stop",{{"id",id}},[&](Error e,QJsonObject){stopError=e;stopped=true;});QVERIFY(backend->held.contains("stop"));
  for(const auto& command:{QString("autoScroll"),QString("pause"),QString("resume")}){
   Error refusal;bool answered=false;auto params=QJsonObject{{"id",id}};if(command=="autoScroll")params["on"]=false;
   service.command(command,params,[&](Error e,QJsonObject){refusal=e;answered=true;});PUTGD_TRY_VERIFY(answered);
   QCOMPARE(refusal.code,QString("scroll_state"));QVERIFY(service.active());
  }
  backend->held.take("stop")({},backend->finished());PUTGD_TRY_VERIFY(stopped);QVERIFY2(!stopError,qPrintable(stopError.code));PUTGD_TRY_VERIFY(admissions==1);
 }
 // A capture left alone until the idle bound with nothing ever scrolled ends
 // like Cancel (#1536's review): no image admitted, cancelled, the Shell's ring
 // and pointer watch gone, no staging files; scroll.inspect and the operation say why.
 void forgottenCaptureEndsLikeCancel(){
  QTemporaryDir dir("/tmp/sf-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
  ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="forgotten";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};
  Operation ended;int finishes=0,admissions=0;host.finish=[&](Operation op){ended=op;++finishes;};host.admit=[&](Operation,ClipboardPayload,auto){++admissions;};
  ScrollCaptureService service(dir.filePath("staging"),host);auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();service.setBackend(std::move(fake));
  FakeShell shell;service.setShell(&shell);
  QString id;service.command("prepare",{{"rect",QJsonArray{0,0,64,64}}},[&](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(e.code));id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.code));started=true;});PUTGD_TRY_VERIFY(started);
  backend->sink({{"id",id},{"event","pointerWatch"},{"on",true}});PUTGD_TRY_VERIFY(shell.watching);
  const int hidesBefore=shell.hides;
  backend->sink({{"id",id},{"event","cancelled"},{"reason","idle"}});
  PUTGD_TRY_VERIFY(!service.active());QCOMPARE(finishes,1);QVERIFY(ended.cancelled);QCOMPARE(ended.stateName(),QString("cancelled"));QVERIFY(ended.error.code.isEmpty());
  const QString note("Nothing was scrolled for 10 minutes, so the capture ended without an image.");
  QCOMPARE(ended.note,note);QVERIFY(ended.items.isEmpty());
  QJsonObject view;service.command("inspect",{{"id",id}},[&](Error e,QJsonObject r){QVERIFY(!e);view=r;});PUTGD_TRY_VERIFY(!view.isEmpty());
  QCOMPARE(view["state"].toString(),QString("cancelled"));QCOMPARE(view["note"].toString(),note);QVERIFY(!view.contains("activity")&&!view.contains("paused"));
  QVERIFY(shell.hides>hidesBefore);QVERIFY(!shell.watching);QVERIFY(!QFileInfo::exists(backend->directory));PUTGD_COMPARE_STEADY(admissions,0,200);
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
 // A hand-back the helper reports before its reply to start arrives (replies
 // and events may come from any thread) is kept: the reply does not clear
 // paused while pausedBy stays (CodeRabbit on #1528).
 void aPauseBeforeTheStartReplyStays(){
  QTemporaryDir dir("/tmp/sq-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
  ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="early";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};
  host.finish=[](Operation){};host.admit=[](Operation,ClipboardPayload,auto){};
  ScrollCaptureService service(dir.filePath("staging"),host);auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();service.setBackend(std::move(fake));
  QString id;service.command("prepare",{{"rect",QJsonArray{0,0,64,64}},{"driver","putgd"}},[&](Error e,QJsonObject r){QVERIFY(!e);id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  backend->holdCommands={"start"};bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY(!e);started=true;});PUTGD_TRY_VERIFY(backend->held.contains("start"));
  backend->sink({{"id",id},{"event","progress"},{"state","paused"},{"driver","user"},{"paused",true},{"pausedBy","pointer"},{"height",400},{"placed",3},{"skipped",0},{"steps",1},{"pixelsPerNotch",0}});
  backend->held.take("start")({},{{"state","capturing"}});PUTGD_TRY_VERIFY(started);
  QJsonObject view;service.command("inspect",{{"id",id}},[&](Error e,QJsonObject r){QVERIFY(!e);view=r;});PUTGD_TRY_VERIFY(!view.isEmpty());
  QCOMPARE(view["state"].toString(),QString("capturing"));QVERIFY(view["paused"].toBool());QCOMPARE(view["pausedBy"].toString(),QString("pointer"));
  bool cancelled=false;service.command("cancel",{{"id",id}},[&](Error,QJsonObject){cancelled=true;});PUTGD_TRY_VERIFY(cancelled);
 }
 // GNOME (#1531): an extension that can watch the pointer is announced to the
 // helper; its watch is on exactly while the helper asks, every position goes
 // to the helper as `pointer`, and the end of the capture gives it up. An older
 // extension is not asked.
 void theShellWatchesThePointerWhileAsked(){
  for(const bool capable:{true,false}){
   QTemporaryDir dir("/tmp/sw-XXXXXX");SettingsService settings(dir.filePath("config.json"));QVERIFY(settings.save(configuration(dir.path(),true),{}));
   ScrollCaptureService::Host host;host.settings=[&]{return settings.snapshot();};host.accept=[&](Operation& op){op.id="watching";op.settings=settings.snapshot();return Error{};};host.persist=[](const Operation&,auto done){done({});};host.observe=[](const Operation&){};
   host.finish=[](Operation){};host.admit=[](Operation,ClipboardPayload,auto){};
   ScrollCaptureService service(dir.filePath("staging"),host);auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();service.setBackend(std::move(fake));
   FakeShell shell;shell.capable=capable;service.setShell(&shell);
   QString id;service.command("prepare",{{"rect",QJsonArray{0,0,64,64}},{"driver","putgd"}},[&](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(e.code));id=r["id"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
   QCOMPARE(backend->preparedParams.value("shellPointer").toBool(false),capable);
   bool started=false;service.command("start",{{"id",id}},[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.code));started=true;});PUTGD_TRY_VERIFY(started);
   backend->sink({{"id",id},{"event","pointerWatch"},{"on",true}});
   PUTGD_TRY_VERIFY(shell.watching==capable);
   if(!capable){QCOMPARE(shell.watches,0);bool cancelled=false;service.command("cancel",{{"id",id}},[&](Error,QJsonObject){cancelled=true;});PUTGD_TRY_VERIFY(cancelled);continue;}
   shell.sink({{"at",QJsonArray{10,20}},{"t",123.0}});
   PUTGD_TRY_VERIFY(backend->pointers.size()==1);QCOMPARE(backend->pointers[0]["at"].toArray(),(QJsonArray{10,20}));QCOMPARE(backend->pointers[0]["t"].toDouble(),123.0);
   backend->sink({{"id",id},{"event","pointerWatch"},{"on",false}});
   PUTGD_TRY_VERIFY(!shell.watching);QCOMPARE(shell.unwatches,1);
   // On again, then the capture ends: given up.
   backend->sink({{"id",id},{"event","pointerWatch"},{"on",true}});PUTGD_TRY_VERIFY(shell.watching);
   bool done=false;service.command("stop",{{"id",id}},[&](Error,QJsonObject){done=true;});PUTGD_TRY_VERIFY(done);
   PUTGD_TRY_VERIFY(!shell.watching);QCOMPARE(shell.watches,2);
  }
 }
 void endReasons_data(){
  QTest::addColumn<QString>("reason");QTest::addColumn<bool>("missed");QTest::addColumn<QJsonObject>("overrides");QTest::addColumn<bool>("admitted");QTest::addColumn<QString>("note");
  const QJsonObject none;
  QTest::newRow("done")<<"done"<<false<<none<<true<<"";
  QTest::newRow("done, auto-scroll at the end after skipped frames")<<"done"<<false<<QJsonObject{{"reachedEnd",true},{"framesSkipped",3},{"scrolledBy","putgd"}}<<true<<"";
  // A blank stretch taller than the region: the end not claimed, and the person told (#1528 round 4).
  QTest::newRow("done, auto-scroll at a blank stretch")<<"done"<<false<<QJsonObject{{"reachedEnd",false},{"scrolledBy","putgd"},{"note","auto-scroll stopped where the page showed nothing: a blank stretch taller than the region looks like the end of the page; if the page goes on, scroll past it yourself"}}<<true<<"Auto-scroll stopped where the page showed nothing. If the page goes on, scroll past the blank part yourself.";
  QTest::newRow("end of content")<<"end_of_content"<<false<<QJsonObject{{"scrolledBy","putgd"}}<<true<<"";
  QTest::newRow("end of content with a skipped frame")<<"end_of_content"<<false<<QJsonObject{{"framesSkipped",1}}<<false<<"";
  QTest::newRow("end of content, end not reached")<<"end_of_content"<<false<<QJsonObject{{"reachedEnd",false}}<<false<<"";
  QTest::newRow("end of content, top unproved")<<"end_of_content"<<true<<none<<false<<"";
  QTest::newRow("top not reached")<<"top_not_reached"<<true<<none<<true<<"Could not reach the top of the page. The image starts partway down.";
  QTest::newRow("top not reached, flag missing")<<"top_not_reached"<<false<<none<<false<<"";
  // The end of the page with the top unproved: the same promise as end_of_content (CodeRabbit on #1528).
  QTest::newRow("top not reached with a skipped frame")<<"top_not_reached"<<true<<QJsonObject{{"framesSkipped",1}}<<false<<"";
  QTest::newRow("top not reached, ended lost")<<"top_not_reached"<<true<<QJsonObject{{"endedLost",true}}<<false<<"";
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
