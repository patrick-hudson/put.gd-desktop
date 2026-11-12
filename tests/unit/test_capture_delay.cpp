// Delayed capture in the operation service (#1363), against a fake desktop
// adapter: a countdown that ends on a locked screen, or is cancelled while the
// lock is being asked about, prepares no target and reads no pixel.
#include "fixtures/U2Fixtures.h"
#include "fixtures/Waits.h"
#include <QSignalSpy>
using namespace putgd;using namespace putgd::test;
namespace {
// Records the order of countdown, lock query, preparation and acquisition.
class DelaySource final:public SourceAdapter {
public:
 QStringList events;bool locked=false,deferLock=false;
 std::function<void(bool)> pendingLock;
 quint64 clipboardRevision() const override {return 0;}
 QJsonObject capabilities() const override {return {};}
 void countdown(int seconds,std::function<void(Error)> reply) override {events<<QStringLiteral("countdown:%1").arg(seconds);reply({});}
 void sessionLocked(std::function<void(bool)> reply) override {events<<"locked?";if(deferLock)pendingLock=std::move(reply);else reply(locked);}
 PreparedAcquisition prepareAcquisition(const QString& action) override {
  events<<"prepare:"+action;
  return [this,action](const ResourceLimits&,std::function<void(PayloadResult)> reply){
   events<<"acquire:"+action;
   ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=QImage(8,6,QImage::Format_ARGB32_Premultiplied);payload.image.fill(Qt::red);
   reply({payload,{}});
  };
 }
 void acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> reply) override {prepareAcquisition(action)(limits,std::move(reply));}
 void cancelAcquisition() override {}
 void copy(const CopyPayload&,std::function<void(Error)> reply) override {reply({});}
};
struct Fixture {
 QTemporaryDir directory;ProviderRegistry registry;SettingsService settings{directory.filePath("config.json")};DelaySource source;
 std::unique_ptr<OperationService> operations;
 Fixture() {
  auto document=configuration(directory.path(),true);
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
 void lockedScreenAtExpiryCapturesNothing() {
  Fixture f;f.source.locked=true;const auto operation=f.run("desktop",{{"delaySeconds",3}});
  QCOMPARE(operation["state"].toString(),QString("failed"));
  QCOMPARE(operation["error"].toObject()["code"].toString(),QString("session_locked"));
  QCOMPARE(f.source.events,QStringList({"countdown:3","locked?"}));
  QVERIFY(operation["items"].toArray().isEmpty());
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
 }
};
QTEST_GUILESS_MAIN(CaptureDelayTest)
#include "test_capture_delay.moc"
