// PortalGlobalShortcuts against a scripted GlobalShortcuts portal on a private
// session bus (#1275 review): failed calls end their stage, availability comes
// back with a working session, and a pause waits for Session.Close.
#include "platform/wayland/PortalGlobalShortcuts.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <atomic>
#include "fixtures/Waits.h"

using namespace putgd;
namespace {
struct FakeShortcut {QString id;QVariantMap properties;};
QDBusArgument& operator<<(QDBusArgument& a,const FakeShortcut& s){a.beginStructure();a<<s.id<<s.properties;a.endStructure();return a;}
const QDBusArgument& operator>>(const QDBusArgument& a,FakeShortcut& s){a.beginStructure();a>>s.id>>s.properties;a.endStructure();return a;}
}
Q_DECLARE_METATYPE(FakeShortcut)
namespace {
const QString root=QStringLiteral("/org/freedesktop/portal/desktop");
// Answers on its own connection in its own thread, as the real portal would.
class FakePortal final:public QDBusVirtualObject {
public:
 std::atomic<int> failCreate{0},failBind{0},closes{0};std::atomic<int> closeDelayMs{0};std::atomic<qint64> closedAt{0};
 QElapsedTimer clock;
 QDBusConnection bus{QDBusConnection::connectToBus(QDBusConnection::SessionBus,QStringLiteral("fake-portal"))};
 FakePortal(){clock.start();}
 QString introspect(const QString&)const override{return {};}
 bool handleMessage(const QDBusMessage& message,const QDBusConnection& connection)override{
  const auto member=message.member();
  auto requestPath=[&](const QVariantMap& options){auto sender=message.service();sender.remove(0,1);sender.replace('.','_');return root+"/request/"+sender+'/'+options.value("handle_token").toString();};
  auto respond=[connection](QString path,uint code,QVariantMap results){QTimer::singleShot(20,[connection,path,code,results]{auto signal=QDBusMessage::createSignal(path,"org.freedesktop.portal.Request","Response");signal.setArguments({code,results});QDBusConnection(connection).send(signal);});};
  if(member=="Get"){connection.send(message.createReply(QVariant::fromValue(QDBusVariant(uint(1)))));return true;}
  if(member=="Register"){connection.send(message.createReply());return true;}
  if(member=="CreateSession"){
   if(failCreate.fetch_sub(1)>0){connection.send(message.createErrorReply("org.freedesktop.DBus.Error.Failed","scripted CreateSession failure"));return true;}
   const auto options=qdbus_cast<QVariantMap>(message.arguments().at(0));const auto path=requestPath(options);
   connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(path))));
   respond(path,0,{{"session_handle",root+"/session/fake/"+options.value("session_handle_token").toString()}});return true;
  }
  if(member=="BindShortcuts"){
   if(failBind.fetch_sub(1)>0){connection.send(message.createErrorReply("org.freedesktop.DBus.Error.Failed","scripted BindShortcuts failure"));return true;}
   const auto options=qdbus_cast<QVariantMap>(message.arguments().at(3));const auto path=requestPath(options);
   connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(path))));
   QList<FakeShortcut> bound{{"region",QVariantMap{{"trigger_description",QStringLiteral("Ctrl+Shift+1")}}}};
   respond(path,0,{{"shortcuts",QVariant::fromValue(bound)}});return true;
  }
  if(member=="Close"){
   const auto reply=message.createReply();const int delay=closeDelayMs;
   QTimer::singleShot(delay,[this,connection,reply]{++closes;closedAt=clock.elapsed();QDBusConnection(connection).send(reply);});return true;
  }
  return false;
 }
};
}
class PortalGlobalShortcutsTest:public QObject {
 Q_OBJECT
 QThread thread_;QObject* worker_=nullptr;FakePortal* fake_=nullptr;
 QString state(PortalGlobalShortcuts& shortcuts){return shortcuts.status()["region"].toObject()["state"].toString();}
private slots:
 void initTestCase(){
  qDBusRegisterMetaType<FakeShortcut>();qDBusRegisterMetaType<QList<FakeShortcut>>();
  if(!QDBusConnection::sessionBus().isConnected())QSKIP("no session bus");
  worker_=new QObject;worker_->moveToThread(&thread_);thread_.start();
  bool ok=false;
  QMetaObject::invokeMethod(worker_,[this,&ok]{
   fake_=new FakePortal;ok=fake_->bus.registerVirtualObject(root,fake_,QDBusConnection::SubPath)&&fake_->bus.registerService("org.freedesktop.portal.Desktop");
  },Qt::BlockingQueuedConnection);
  QVERIFY(ok);
 }
 void cleanupTestCase(){
  if(fake_)QMetaObject::invokeMethod(worker_,[this]{fake_->bus.unregisterService("org.freedesktop.portal.Desktop");fake_->bus.unregisterObject(root,QDBusConnection::UnregisterTree);delete fake_;fake_=nullptr;QDBusConnection::disconnectFromBus("fake-portal");},Qt::BlockingQueuedConnection);
  QMetaObject::invokeMethod(worker_,[this]{delete worker_;worker_=nullptr;},Qt::BlockingQueuedConnection);
  thread_.quit();thread_.wait();
 }
 void init(){fake_->failCreate=0;fake_->failBind=0;fake_->closeDelayMs=0;fake_->closes=0;}
 void aFailedBindReportsTheErrorAndRebindRetries(){
  fake_->failBind=1;PortalGlobalShortcuts shortcuts;
  shortcuts.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(putgd::test::waitFor([&]{return state(shortcuts)=="shortcut_unavailable";},5000));
  QVERIFY(shortcuts.status()["region"].toObject()["error"].toString().contains("BindShortcuts failed"));
  // The same request again is a retry, not a no-op behind a stuck "bind" stage.
  shortcuts.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(putgd::test::waitFor([&]{return state(shortcuts)=="bound";},5000));
 }
 void availabilityReturnsWithAWorkingSession(){
  fake_->failCreate=1;PortalGlobalShortcuts shortcuts;
  shortcuts.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(putgd::test::waitFor([&]{return !shortcuts.available();},5000));
  shortcuts.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(putgd::test::waitFor([&]{return state(shortcuts)=="bound";},5000));QVERIFY(shortcuts.available());
 }
 void pauseWaitsForTheSessionToClose(){
  PortalGlobalShortcuts shortcuts;shortcuts.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(putgd::test::waitFor([&]{return state(shortcuts)=="bound";},5000));
  fake_->closeDelayMs=400;bool paused=false;qint64 pausedAt=0;
  shortcuts.setPaused(true,[&](Error){paused=true;pausedAt=fake_->clock.elapsed();});
  QVERIFY(!paused);
  QVERIFY(putgd::test::waitFor([&]{return paused;},5000));
  QCOMPARE(fake_->closes.load(),1);QVERIFY(pausedAt>=fake_->closedAt.load());
  QCOMPARE(state(shortcuts),QString("recording"));
 }
};
QTEST_GUILESS_MAIN(PortalGlobalShortcutsTest)
#include "test_portal_global_shortcuts.moc"
