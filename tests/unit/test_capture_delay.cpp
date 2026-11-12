// Delayed capture, Capture monitor and Repeat last region in the operation
// service (#1363), against a fake desktop adapter: what is prepared when, what a
// cancelled or locked countdown leaves behind, and how a repeat is recorded.
#include "fixtures/U2Fixtures.h"
#include "fixtures/Waits.h"
#include <QSignalSpy>
using namespace putgd;using namespace putgd::test;
namespace {
// Records the order of countdown, lock query, preparation and acquisition.
class DelaySource final:public SourceAdapter {
public:
 QStringList events;QList<int> preparedDelays;bool locked=false,deferCountdown=false,deferLock=false,remembered=false;
 std::function<void(Error)> pendingCountdown;std::function<void(bool)> pendingLock;int cancels=0;
 quint64 clipboardRevision() const override {return 0;}
 QJsonObject capabilities() const override {return {};}
 void countdown(int seconds,std::function<void(Error)> reply) override {
  events<<QStringLiteral("countdown:%1").arg(seconds);
  if(deferCountdown)pendingCountdown=std::move(reply);else reply({});
 }
 void sessionLocked(std::function<void(bool)> reply) override {events<<"locked?";if(deferLock)pendingLock=std::move(reply);else reply(locked);}
 PreparedAcquisition prepareAcquisition(const QString& action) override {
  events<<"prepare:"+action;preparedDelays<<captureDelay_;
  Error error;if(action=="repeat"&&!remembered)error=noLastRegion();
  return [this,action,error](const ResourceLimits&,std::function<void(PayloadResult)> reply){
   events<<"acquire:"+action;
   if(error){reply({{},error});return;}
   ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=QImage(8,6,QImage::Format_ARGB32_Premultiplied);payload.image.fill(Qt::red);
   if(action=="region")remembered=true;
   reply({payload,{}});
  };
 }
 void acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> reply) override {prepareAcquisition(action)(limits,std::move(reply));}
 void cancelAcquisition() override {++cancels;if(auto reply=std::exchange(pendingCountdown,{}))reply({"cancelled","Capture countdown cancelled",{}});}
 void copy(const CopyPayload&,std::function<void(Error)> reply) override {reply({});}
};
struct Fixture {
 QTemporaryDir directory;ProviderRegistry registry;SettingsService settings{directory.filePath("config.json")};DelaySource source;
 std::unique_ptr<OperationService> operations;
 explicit Fixture(const QString& regionMode={}) {
  auto document=configuration(directory.path(),true);
  if(!regionMode.isEmpty()){auto preferences=document["preferences"].toObject();preferences["regionSelectionMode"]=regionMode;document["preferences"]=preferences;}
  QVERIFY(settings.save(document,{}));
  operations=std::make_unique<OperationService>(directory.filePath("data"),settings,registry,source);
  PUTGD_TRY_VERIFY(operations->ready());
 }
 ~Fixture(){operations->shutdown();}
 // Invokes and waits for the operation to reach a terminal state.
 QJsonObject run(const QString& action,const QJsonObject& options={},Error* refused=nullptr) {
  QString id;Error error;int replies=0;
  operations->invoke(action,[&](Error e,QJsonObject accepted){error=e;id=accepted["operationId"].toString();++replies;},options);
  if(!waitFor([&]{return replies==1;})){if(refused)*refused={"timeout","no reply",{}};return {};}
  if(refused)*refused=error;
  if(error)return {};
  return finished(id);
 }
 QJsonObject finished(const QString& id) {
  QJsonObject operation;
  const bool done=waitFor([&]{
   bool answered=false;operations->inspect(id,[&](Error,QJsonObject value){operation=value["operation"].toObject().isEmpty()?value:value["operation"].toObject();answered=true;});
   if(!waitFor([&]{return answered;}))return false;
   return !Operation::activeState(operation["state"].toString());
  });
  return done?operation:QJsonObject{};
 }
};
}
class CaptureDelayTest final:public QObject {
 Q_OBJECT
private slots:
 void delayedWindowCountsDownThenFreezesItsTarget() {
  Fixture f;const auto operation=f.run("window",{{"delaySeconds",5}});
  QCOMPARE(operation["state"].toString(),QString("completed"));
  // Nothing is prepared before the countdown and the lock check have passed.
  QCOMPARE(f.source.events,QStringList({"countdown:5","locked?","prepare:window","acquire:window"}));
  QCOMPARE(f.source.preparedDelays,QList<int>{0});
 }
 void delayedMonitorAndDesktopUseTheSamePath_data() {
  QTest::addColumn<QString>("action");
  QTest::newRow("monitor")<<"monitor";QTest::newRow("desktop")<<"desktop";
 }
 void delayedMonitorAndDesktopUseTheSamePath() {
  QFETCH(QString,action);Fixture f;const auto operation=f.run(action,{{"delaySeconds",3}});
  QCOMPARE(operation["state"].toString(),QString("completed"));QCOMPARE(operation["action"].toString(),action);
  QCOMPARE(f.source.events,QStringList({"countdown:3","locked?","prepare:"+action,"acquire:"+action}));
 }
 void liveRegionHandsItsDelayToTheAdapter() {
  // Area first, then the countdown: the adapter runs it between selection and pixels.
  Fixture f;const auto operation=f.run("region",{{"delaySeconds",10}});
  QCOMPARE(operation["state"].toString(),QString("completed"));
  QCOMPARE(f.source.events,QStringList({"prepare:region","acquire:region"}));
  QCOMPARE(f.source.preparedDelays,QList<int>{10});
  // The delay belongs to that one request.
  f.source.events.clear();f.source.preparedDelays.clear();f.run("region");
  QCOMPARE(f.source.preparedDelays,QList<int>{0});
 }
 void screenshotFirstRegionCountsDownBeforeTheFreeze() {
  Fixture f("screenshot_first");const auto operation=f.run("region",{{"delaySeconds",3}});
  QCOMPARE(operation["state"].toString(),QString("completed"));
  QCOMPARE(f.source.events,QStringList({"countdown:3","locked?","prepare:region","acquire:region"}));
  QCOMPARE(f.source.preparedDelays,QList<int>{0});
 }
 void lockedScreenAtExpiryCapturesNothing() {
  Fixture f;f.source.locked=true;const auto operation=f.run("desktop",{{"delaySeconds",3}});
  QCOMPARE(operation["state"].toString(),QString("failed"));
  QCOMPARE(operation["error"].toObject()["code"].toString(),QString("session_locked"));
  QCOMPARE(f.source.events,QStringList({"countdown:3","locked?"}));
  QVERIFY(operation["items"].toArray().isEmpty());
  // The interactive reservation is released: the next capture is accepted.
  f.source.locked=false;f.source.events.clear();
  QCOMPARE(f.run("desktop")["state"].toString(),QString("completed"));
 }
 void cancellingACountdownCapturesNothing() {
  Fixture f;f.source.deferCountdown=true;
  QString id;int replies=0;f.operations->invoke("monitor",[&](Error,QJsonObject accepted){id=accepted["operationId"].toString();++replies;},{{"delaySeconds",10}});
  PUTGD_TRY_COMPARE(replies,1);PUTGD_TRY_VERIFY(bool(f.source.pendingCountdown));
  // A second interactive capture waits its turn while the countdown runs.
  Error busy;f.run("region",{},&busy);QCOMPARE(busy.code,QString("busy_selecting"));
  int cancelled=0;f.operations->cancel(id,[&](Error,QJsonObject){++cancelled;});PUTGD_TRY_COMPARE(cancelled,1);
  const auto operation=f.finished(id);
  QCOMPARE(operation["state"].toString(),QString("cancelled"));
  QCOMPARE(f.source.events,QStringList({"countdown:10"}));
  QVERIFY(operation["items"].toArray().isEmpty());
  f.source.deferCountdown=false;f.source.events.clear();
  QCOMPARE(f.run("monitor")["state"].toString(),QString("completed"));
 }
 // A delayed capture reserves the selection before any adapter holds it. Cancelled
 // in the gap before its countdown starts, it must give the reservation back.
 void cancellingBeforeTheCountdownStartsFreesTheSelection() {
  Fixture f;f.source.deferCountdown=true;
  QString id;int replies=0,cancelled=0;
  f.operations->invoke("desktop",[&](Error,QJsonObject accepted){id=accepted["operationId"].toString();++replies;if(!id.isEmpty())f.operations->cancel(id,[&](Error,QJsonObject){++cancelled;});},{{"delaySeconds",5}});
  PUTGD_TRY_COMPARE(replies,1);PUTGD_TRY_COMPARE(cancelled,1);
  const auto operation=f.finished(id);
  QCOMPARE(operation["state"].toString(),QString("cancelled"));
  // The countdown never started and nothing was prepared.
  QVERIFY(f.source.events.isEmpty());QVERIFY(!f.source.pendingCountdown);
  f.source.deferCountdown=false;
  Error busy;const auto next=f.run("monitor",{},&busy);
  QCOMPARE(busy.code,QString());QCOMPARE(next["state"].toString(),QString("completed"));
 }
 // The countdown is over and the lock is being asked about: nothing is left for a
 // cancel to stop, and still no target may be frozen and no pixel read.
 void cancellingDuringTheLockQueryReadsNothing() {
  Fixture f;f.source.deferLock=true;
  QString id;int replies=0,cancelled=0;
  f.operations->invoke("monitor",[&](Error,QJsonObject accepted){id=accepted["operationId"].toString();++replies;},{{"delaySeconds",3}});
  PUTGD_TRY_COMPARE(replies,1);PUTGD_TRY_VERIFY(bool(f.source.pendingLock));
  f.operations->cancel(id,[&](Error,QJsonObject){++cancelled;});PUTGD_TRY_COMPARE(cancelled,1);
  std::exchange(f.source.pendingLock,{})(false);
  const auto operation=f.finished(id);
  QCOMPARE(operation["state"].toString(),QString("cancelled"));
  QCOMPARE(f.source.events,QStringList({"countdown:3","locked?"}));
  QVERIFY(operation["items"].toArray().isEmpty());
  f.source.deferLock=false;f.source.events.clear();
  QCOMPARE(f.run("monitor")["state"].toString(),QString("completed"));
 }
 void repeatIsARegionCaptureOfTheRememberedRectangle() {
  Fixture f;
  // Nothing to repeat yet: refused with the adapter's message, nothing stored.
  auto operation=f.run("repeat");
  QCOMPARE(operation["state"].toString(),QString("failed"));
  QCOMPARE(operation["error"].toObject()["code"].toString(),QString("repeat_unavailable"));
  QCOMPARE(f.run("region")["state"].toString(),QString("completed"));
  f.source.events.clear();operation=f.run("repeat");
  QCOMPARE(operation["state"].toString(),QString("completed"));
  // Recorded, routed and named as the region capture it repeats.
  QCOMPARE(operation["action"].toString(),QString("region"));
  QCOMPARE(f.source.events,QStringList({"prepare:repeat","acquire:repeat"}));
  f.source.events.clear();operation=f.run("repeat",{{"delaySeconds",3}});
  QCOMPARE(operation["state"].toString(),QString("completed"));
  QCOMPARE(f.source.events,QStringList({"countdown:3","locked?","prepare:repeat","acquire:repeat"}));
 }
 void delayIsValidatedBeforeAnythingIsAccepted_data() {
  QTest::addColumn<QString>("action");QTest::addColumn<QJsonValue>("delay");
  QTest::newRow("four seconds")<<"region"<<QJsonValue(4);
  QTest::newRow("negative")<<"desktop"<<QJsonValue(-3);
  QTest::newRow("fraction")<<"window"<<QJsonValue(3.5);
  QTest::newRow("text")<<"monitor"<<QJsonValue("3");
  QTest::newRow("clipboard")<<"clipboard"<<QJsonValue(3);
  QTest::newRow("edit fraction")<<"edit"<<QJsonValue(3.5);
  QTest::newRow("record")<<"record"<<QJsonValue(3);
 }
 void delayIsValidatedBeforeAnythingIsAccepted() {
  QFETCH(QString,action);QFETCH(QJsonValue,delay);Fixture f;Error refused;
  f.run(action,{{"delaySeconds",delay}},&refused);
  QCOMPARE(refused.code,QString("invalid_request"));QVERIFY(f.source.events.isEmpty());
 }
 void anEditWaitsOnlyWhenItCapturesTheScreen() {
  // Capture region & edit takes the delay as a region does: the area first, then
  // the adapter's countdown. The clipboard and a file read no screen.
  Fixture f;QString id;Error error;int replies=0;
  f.operations->invoke("edit",[&](Error e,QJsonObject accepted){error=e;id=accepted["operationId"].toString();++replies;},{{"delaySeconds",5}});
  QVERIFY(waitFor([&]{return replies==1;}));QVERIFY2(!error,qPrintable(error.message));
  QVERIFY(waitFor([&]{return f.source.events.contains("acquire:region");}));
  QCOMPARE(f.source.preparedDelays,QList<int>{5});
  int cancelled=0;f.operations->cancel(id,[&](Error,QJsonObject){++cancelled;});QVERIFY(waitFor([&]{return cancelled==1;}));
  for(const auto& options:{QJsonObject{{"source","clipboard"},{"delaySeconds",3}},QJsonObject{{"source","file"},{"path","/synthetic/picture.png"},{"delaySeconds",3}}}){
   Fixture refusing;Error refused;refusing.run("edit",options,&refused);
   QCOMPARE(refused.code,QString("invalid_request"));QVERIFY(refusing.source.events.isEmpty());
  }
 }
 void zeroDelayIsImmediate() {
  Fixture f;QCOMPARE(f.run("desktop",{{"delaySeconds",0}})["state"].toString(),QString("completed"));
  QCOMPARE(f.source.events,QStringList({"prepare:desktop","acquire:desktop"}));
 }
 void anAdapterWithoutACountdownRefusesTheDelay() {
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  Source source;ClipboardPayload payload;payload.kind="text";payload.bytes="synthetic";source.payload=payload;
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  QString id;int replies=0;operations.invoke("desktop",[&](Error,QJsonObject accepted){id=accepted["operationId"].toString();++replies;},{{"delaySeconds",3}});
  PUTGD_TRY_COMPARE(replies,1);
  QJsonObject operation;PUTGD_TRY_VERIFY([&]{bool answered=false;operations.inspect(id,[&](Error,QJsonObject value){operation=value["operation"].toObject().isEmpty()?value:value["operation"].toObject();answered=true;});return waitFor([&]{return answered;})&&!Operation::activeState(operation["state"].toString());}());
  QCOMPARE(operation["error"].toObject()["code"].toString(),QString("delay_unavailable"));QCOMPARE(source.reads,0);
  operations.shutdown();
 }
};
QTEST_GUILESS_MAIN(CaptureDelayTest)
#include "test_capture_delay.moc"
