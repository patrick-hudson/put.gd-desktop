#include "app/ResidentSession.h"
#include "fixtures/Waits.h"
#include <QCoreApplication>
#include <QHash>
#include <QSignalSpy>
#include <QTest>
#include <sys/socket.h>
#include <unistd.h>
using namespace putgd;
namespace {
// logind as sd-login reports it, with a socket pair for its change monitor.
class FakeSessions final:public LoginSessions {
public:
 struct Data {QHash<qint64,QString> pids;QHash<QString,LoginSessionState> sessions;int fds[2]={-1,-1};bool monitorAvailable=true;bool available=true;};
 explicit FakeSessions(std::shared_ptr<Data> data):data_(std::move(data)) {}
 bool available()const override {return data_->available;}
 QString sessionOf(qint64 pid)const override {return data_->pids.value(pid);}
 LoginSessionState state(const QString& id)const override {return data_->sessions.contains(id)?data_->sessions.value(id):LoginSessionState{id,false,{},{}};}
 int monitor() override {
  if(!data_->monitorAvailable)return -1;
  if(data_->fds[0]<0&&::socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,data_->fds)!=0)return -1;
  return data_->fds[0];
 }
 void flush() override {char bytes[64];while(::read(data_->fds[0],bytes,sizeof(bytes))>0){}}
private:
 std::shared_ptr<Data> data_;
};
struct Logind {
 std::shared_ptr<FakeSessions::Data> data=std::make_shared<FakeSessions::Data>();
 ~Logind() {for(const int fd:data->fds)if(fd>=0)::close(fd);}
 std::unique_ptr<LoginSessions> sessions() {return std::make_unique<FakeSessions>(data);}
 void set(const QString& id,const QString& state,const QString& type) {data->sessions.insert(id,{id,true,state,type});}
 // logind rewrites the session's state file; sd-login's monitor turns readable.
 void change() {QVERIFY(data->fds[1]>=0);QCOMPARE(::write(data->fds[1],"x",1),ssize_t(1));}
};
constexpr qint64 resident=4100,launcher=4200;
}
class ResidentSessionContract:public QObject {
 Q_OBJECT
private slots:
 void launcherDecision_data() {
  QTest::addColumn<QString>("residentSession");
  QTest::addColumn<QString>("launcherSession");
  QTest::addColumn<QString>("state");
  QTest::addColumn<QString>("type");
  QTest::addColumn<bool>("present");
  QTest::addColumn<bool>("ended");
  QTest::addColumn<QString>("reason");
  // #490: the Cinnamon logout. The new login's launcher finds the old resident.
  QTest::newRow("ended-x11-login") << QString("c24") << QString("c26") << QString("closing") << QString("x11") << true << true << QString("session_closing");
  QTest::newRow("ended-wayland-login") << QString("4") << QString("7") << QString("") << QString("wayland") << false << true << QString("session_removed");
  QTest::newRow("same-login") << QString("c26") << QString("c26") << QString("active") << QString("x11") << true << false << QString("same_session");
  QTest::newRow("launcher-outside-any-session") << QString("c26") << QString("") << QString("active") << QString("x11") << true << false << QString("live_session");
  QTest::newRow("locked-or-switched-away") << QString("c26") << QString("c27") << QString("online") << QString("x11") << true << false << QString("live_session");
  // A lab or terminal resident started over SSH for the desktop's DISPLAY: its
  // tty session closes with the SSH connection while the desktop runs on.
  QTest::newRow("closing-ssh-session") << QString("5308") << QString("c26") << QString("closing") << QString("tty") << true << false << QString("live_session");
  // GNOME and Plasma start autostart entries as systemd units, outside the session.
  QTest::newRow("resident-in-systemd-unit") << QString("") << QString("3") << QString("") << QString("") << true << false << QString("outside_session");
 }
 void launcherDecision() {
  QFETCH(QString,residentSession);QFETCH(QString,launcherSession);QFETCH(QString,state);QFETCH(QString,type);QFETCH(bool,present);QFETCH(bool,ended);QFETCH(QString,reason);
  Logind logind;
  if(!residentSession.isEmpty())logind.data->pids.insert(resident,residentSession);
  if(!launcherSession.isEmpty())logind.data->pids.insert(launcher,launcherSession);
  // Not present: the scope outlived logind's record of the session.
  if(!residentSession.isEmpty()&&present)logind.set(residentSession,state,type);
  if(!launcherSession.isEmpty()&&launcherSession!=residentSession)logind.set(launcherSession,"active",type);
  const auto sessions=logind.sessions();
  const auto decision=putgd::residentSession(*sessions,resident,launcher);
  QCOMPARE(decision.ended,ended);
  QCOMPARE(decision.reason,reason);
  if(!residentSession.isEmpty())QCOMPARE(decision.session.id,residentSession);
 }
 void launcherHandsOffWithoutLogind() {
  Logind logind;logind.data->available=false;logind.data->pids.insert(resident,"c24");logind.set("c24","closing","x11");
  const auto decision=residentSession(*logind.sessions(),resident,launcher);
  QVERIFY(!decision.ended);QCOMPARE(decision.reason,QString("logind_unavailable"));
 }
 void residentEndsWithLogoutNotLockOrSwitch() {
  Logind logind;logind.data->pids.insert(resident,"c24");logind.set("c24","active","x11");
  SessionWatch watch(logind.sessions(),resident);
  QSignalSpy ended(&watch,&SessionWatch::ended);
  QCOMPARE(watch.status(),(QJsonObject{{"id","c24"},{"type","x11"},{"watched",true}}));
  // A screen lock only sets LockedHint: the session stays active.
  logind.change();
  PUTGD_COMPARE_STEADY(ended.size(),0,200);
  // Another user's or a greeter's session takes the seat: online, not closing.
  logind.set("c24","online","x11");logind.change();
  PUTGD_COMPARE_STEADY(ended.size(),0,200);
  logind.set("c24","active","x11");logind.change();
  PUTGD_COMPARE_STEADY(ended.size(),0,200);
  // Logout: the scope keeps this process, so logind keeps the session, closing.
  logind.set("c24","closing","x11");logind.change();
  PUTGD_TRY_COMPARE(ended.size(),1);
  QCOMPARE(ended.first().at(0).toString(),QString("session_closing"));
  QCOMPARE(ended.first().at(1).toJsonObject(),(QJsonObject{{"id","c24"},{"type","x11"},{"state","closing"}}));
  logind.change();
  PUTGD_COMPARE_STEADY(ended.size(),1,200);
 }
 void residentEndsWhenSessionIsRemoved() {
  Logind logind;logind.data->pids.insert(resident,"2");logind.set("2","active","wayland");
  SessionWatch watch(logind.sessions(),resident);QSignalSpy ended(&watch,&SessionWatch::ended);
  PUTGD_COMPARE_STEADY(ended.size(),0,100);
  logind.data->sessions.remove("2");logind.change();
  PUTGD_TRY_COMPARE(ended.size(),1);
  QCOMPARE(ended.first().at(0).toString(),QString("session_removed"));
 }
 void residentStartedInEndingSessionEnds() {
  Logind logind;logind.data->pids.insert(resident,"c24");logind.set("c24","closing","x11");
  SessionWatch watch(logind.sessions(),resident);QSignalSpy ended(&watch,&SessionWatch::ended);
  QVERIFY(ended.isEmpty());
  PUTGD_TRY_COMPARE(ended.size(),1);
  QCOMPARE(ended.first().at(0).toString(),QString("session_closing"));
 }
 void residentOutsideGraphicalSessionIsNotWatched_data() {
  QTest::addColumn<QString>("session");
  QTest::addColumn<QString>("type");
  QTest::addColumn<bool>("available");
  QTest::addColumn<bool>("monitor");
  QTest::addColumn<QString>("reason");
  QTest::newRow("ssh") << QString("5308") << QString("tty") << true << true << QString("not_graphical");
  QTest::newRow("systemd-unit") << QString("") << QString("") << true << true << QString("outside_session");
  QTest::newRow("no-logind") << QString("c24") << QString("x11") << false << true << QString("logind_unavailable");
  QTest::newRow("no-monitor") << QString("c24") << QString("x11") << true << false << QString("monitor_unavailable");
 }
 void residentOutsideGraphicalSessionIsNotWatched() {
  QFETCH(QString,session);QFETCH(QString,type);QFETCH(bool,available);QFETCH(bool,monitor);QFETCH(QString,reason);
  Logind logind;logind.data->available=available;logind.data->monitorAvailable=monitor;
  if(!session.isEmpty()){logind.data->pids.insert(resident,session);logind.set(session,"closing",type);}
  SessionWatch watch(logind.sessions(),resident);QSignalSpy ended(&watch,&SessionWatch::ended);
  QCOMPARE(watch.status()["watched"].toBool(),false);
  QCOMPARE(watch.status()["reason"].toString(),reason);
  PUTGD_COMPARE_STEADY(ended.size(),0,200);
 }
 void graphicalTypes() {
  for(const auto* type:{"x11","wayland","mir"})QVERIFY(graphicalSessionType(type));
  for(const auto* type:{"tty","unspecified","web",""})QVERIFY(!graphicalSessionType(type));
 }
 // The real sd-login, where the host has one: a lookup never fails loudly.
 void systemSessionsAnswer() {
  const auto sessions=LoginSessions::system();
  if(!sessions->available())QSKIP("No sd-login on this host");
  const auto id=sessions->sessionOf(QCoreApplication::applicationPid());
  if(!id.isEmpty())QVERIFY(sessions->state(id).exists);
  QVERIFY(!sessions->state("putgdnosuchsession0").exists);
 }
};
QTEST_GUILESS_MAIN(ResidentSessionContract)
#include "test_resident_session.moc"
