#include "ResidentSession.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#ifdef Q_OS_LINUX
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace putgd {
namespace {
#ifdef Q_OS_LINUX
// The sd-login calls used here are libsystemd's stable public ABI; elogind
// ships the same API under its own name.
class SdLoginSessions final:public LoginSessions {
 using PidGetSession=int(*)(pid_t,char**);
 using SessionGet=int(*)(const char*,char**);
 using MonitorNew=int(*)(const char*,void**);
 using MonitorCall=int(*)(void*);
 using MonitorUnref=void*(*)(void*);
public:
 SdLoginSessions() {
  for(const char* name:{"libsystemd.so.0","libelogind.so.0"})if((library_=::dlopen(name,RTLD_NOW|RTLD_LOCAL)))break;
  if(!library_)return;
  const auto symbol=[this](const char* name){return ::dlsym(library_,name);};
  pidGetSession_=reinterpret_cast<PidGetSession>(symbol("sd_pid_get_session"));
  getState_=reinterpret_cast<SessionGet>(symbol("sd_session_get_state"));
  getType_=reinterpret_cast<SessionGet>(symbol("sd_session_get_type"));
  monitorNew_=reinterpret_cast<MonitorNew>(symbol("sd_login_monitor_new"));
  monitorFd_=reinterpret_cast<MonitorCall>(symbol("sd_login_monitor_get_fd"));
  monitorFlush_=reinterpret_cast<MonitorCall>(symbol("sd_login_monitor_flush"));
  monitorUnref_=reinterpret_cast<MonitorUnref>(symbol("sd_login_monitor_unref"));
 }
 // The library stays loaded: libdbus holds it too.
 ~SdLoginSessions() override {if(monitor_)monitorUnref_(monitor_);}
 bool available()const override {return pidGetSession_&&getState_&&getType_&&monitorNew_&&monitorFd_&&monitorFlush_&&monitorUnref_;}
 QString sessionOf(qint64 pid)const override {
  char* value=nullptr;
  if(!available()||pid<=0||pidGetSession_(pid_t(pid),&value)<0||!value)return {};
  const auto id=QString::fromUtf8(value);std::free(value);return id;
 }
 LoginSessionState state(const QString& id)const override {
  LoginSessionState result{id,true,{},{}};
  if(!available()||id.isEmpty())return result;
  const auto key=id.toUtf8();
  const auto read=[&](SessionGet get,QString& out)->int{char* value=nullptr;const int r=get(key.constData(),&value);if(r>=0&&value)out=QString::fromUtf8(value);std::free(value);return r;};
  // -ENXIO: logind has no such session. Any other failure leaves it unknown.
  if(read(getState_,result.state)==-ENXIO){result.exists=false;return result;}
  read(getType_,result.type);
  return result;
 }
 int monitor() override {
  if(!available())return -1;
  if(!monitor_&&monitorNew_("session",&monitor_)<0)monitor_=nullptr;
  return monitor_?monitorFd_(monitor_):-1;
 }
 void flush() override {if(monitor_)monitorFlush_(monitor_);}
private:
 void* library_=nullptr;
 PidGetSession pidGetSession_=nullptr;
 SessionGet getState_=nullptr,getType_=nullptr;
 MonitorNew monitorNew_=nullptr;
 MonitorCall monitorFd_=nullptr,monitorFlush_=nullptr;
 MonitorUnref monitorUnref_=nullptr;
 void* monitor_=nullptr;
};
// Running, not a zombie: the process found on the socket, by PID and start time.
bool running(const LocalPeer& peer) {
 QFile stat(QStringLiteral("/proc/%1/stat").arg(peer.pid));
 if(!stat.open(QIODevice::ReadOnly))return false;
 const auto line=stat.readAll();const auto fields=line.mid(line.lastIndexOf(')')+1).split(' ');
 return fields.size()>20&&fields[20].toULongLong()==peer.start&&fields[1]!="Z"&&fields[1]!="X";
}
#else
class SdLoginSessions final:public LoginSessions {
public:
 bool available()const override {return false;}
 QString sessionOf(qint64)const override {return {};}
 LoginSessionState state(const QString& id)const override {return {id,true,{},{}};}
 int monitor() override {return -1;}
 void flush() override {}
};
#endif
}
std::unique_ptr<LoginSessions> LoginSessions::system() {return std::make_unique<SdLoginSessions>();}
bool graphicalSessionType(const QString& type) {return type=="x11"||type=="wayland"||type=="mir";}
ResidentSession residentSession(const LoginSessions& sessions,qint64 residentPid,qint64 launcherPid) {
 if(!sessions.available()||residentPid<=0)return {false,"logind_unavailable",{}};
 const auto id=sessions.sessionOf(residentPid);
 if(id.isEmpty())return {false,"outside_session",{}};
 if(id==sessions.sessionOf(launcherPid))return {false,"same_session",{id,true,{},{}}};
 const auto session=sessions.state(id);
 // The scope outlived logind's record of it: nothing of that login is left.
 if(!session.exists)return {true,"session_removed",session};
 if(session.state=="closing"&&graphicalSessionType(session.type))return {true,"session_closing",session};
 return {false,"live_session",session};
}
Retirement retireResident(const QString& socketPath,const LocalPeer& peer,int quitMs,int terminateMs) {
#ifdef Q_OS_LINUX
 if(!peer.connected||peer.pid<=0||peer.pid==QCoreApplication::applicationPid()||peer.uid!=::getuid()||!peer.start||!running(peer))return {};
 // A pidfd keeps every signal on this process even if its PID is reused.
 int pidfd=-1;
#ifdef SYS_pidfd_open
 pidfd=int(::syscall(SYS_pidfd_open,pid_t(peer.pid),0));
 if(pidfd>=0&&!running(peer)){::close(pidfd);pidfd=-1;}
#endif
 struct Closer {int& fd;~Closer(){if(fd>=0)::close(fd);}} closer{pidfd};
 const auto exited=[&](int ms){
  QDeadlineTimer deadline(ms);
  for(;;) {
   if(pidfd>=0) {
    pollfd item{pidfd,POLLIN,0};
    const int result=::poll(&item,1,int(std::min<qint64>(deadline.remainingTime(),INT_MAX)));
    if(result>0)return true;
    if(result<0&&errno==EINTR)continue;
    if(result<0)return !running(peer);
   } else {
    if(!running(peer))return true;
    QThread::msleep(50);
   }
   if(deadline.hasExpired())return !running(peer);
  }
 };
 const auto send=[&](int number){
#ifdef SYS_pidfd_send_signal
  if(pidfd>=0){::syscall(SYS_pidfd_send_signal,pidfd,number,nullptr,0);return;}
#endif
  if(running(peer))::kill(pid_t(peer.pid),number);
 };
 const QJsonObject request{{"schemaVersion",1},{"requestId",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"session",socketPath},{"method","ui.quit"},{"params",QJsonObject{}}};
 activateLocalSession(socketPath,QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');
 if(exited(quitMs))return {true,"quit"};
 send(SIGTERM);
 if(exited(terminateMs))return {true,"terminated"};
 send(SIGKILL);
 if(exited(2000))return {true,"killed"};
#else
 Q_UNUSED(socketPath);Q_UNUSED(peer);Q_UNUSED(quitMs);Q_UNUSED(terminateMs);
#endif
 return {};
}
SessionWatch::SessionWatch(std::unique_ptr<LoginSessions> sessions,qint64 pid,QObject* parent):QObject(parent),sessions_(std::move(sessions)) {
 if(!sessions_||!sessions_->available()){status_={{"watched",false},{"reason","logind_unavailable"}};return;}
 id_=sessions_->sessionOf(pid>0?pid:QCoreApplication::applicationPid());
 if(id_.isEmpty()){status_={{"watched",false},{"reason","outside_session"}};return;}
 const auto session=sessions_->state(id_);
 status_={{"id",id_},{"type",session.type},{"watched",false}};
 if(!graphicalSessionType(session.type)){status_["reason"]="not_graphical";return;}
 const int fd=sessions_->monitor();
 if(fd<0){status_["reason"]="monitor_unavailable";return;}
 notifier_=new QSocketNotifier(fd,QSocketNotifier::Read,this);
 connect(notifier_,&QSocketNotifier::activated,this,[this]{sessions_->flush();check();});
 status_["watched"]=true;
 // A resident started while its session is already ending does not stay either.
 QTimer::singleShot(0,this,&SessionWatch::check);
}
SessionWatch::~SessionWatch() {delete notifier_;}
void SessionWatch::check() {
 if(ended_)return;
 const auto session=sessions_->state(id_);
 // A locked or switched-away session stays online or active; only a logout closes it.
 const QString reason=!session.exists?QStringLiteral("session_removed"):session.state=="closing"?QStringLiteral("session_closing"):QString();
 if(reason.isEmpty())return;
 ended_=true;notifier_->setEnabled(false);
 emit ended(reason,{{"id",id_},{"type",session.type},{"state",session.state}});
}
}
