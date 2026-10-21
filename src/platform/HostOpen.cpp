#include "platform/HostOpen.h"
#include "core/HostEnvironment.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <fcntl.h>
#include <unistd.h>

namespace putgd::host {
namespace {
const QString portalService=QStringLiteral("org.freedesktop.portal.Desktop");
const QString portalPath=QStringLiteral("/org/freedesktop/portal/desktop");
const QString openUriInterface=QStringLiteral("org.freedesktop.portal.OpenURI");
const QString requestInterface=QStringLiteral("org.freedesktop.portal.Request");
// A first use may start the portal. A link or folder opens without a chooser,
// so an answer that takes longer is given up on, still without opening the
// target a second time; history.openLink replies inside its 15 s.
int callTimeoutMs=10000;
int responseTimeoutMs=12000;
std::function<void(const QUrl&)> testOpener;
void report(const Opened& done,const QJsonObject& result){if(done)done(result);}
bool tested(const QUrl& target,const Opened& done){if(!testOpener)return false;testOpener(target);report(done,{{"route","test"},{"opened",true}});return true;}
// gio is GLib's own opener; xdg-open where a host has no gio.
QJsonObject launch(const QString& target,QJsonObject result){
 result["route"]="launcher";
 qint64 pid=0;
 if(startDetached("gio",{"open",target},&pid))result["program"]="gio";
 else if(startDetached("xdg-open",{target},&pid))result["program"]="xdg-open";
 else{result["opened"]=false;result["error"]="launcher_missing";return result;}
 result["opened"]=true;result["pid"]=pid;
 return result;
}
QString fallbackReason(const QString& first,const QString& second){return first.isEmpty()?second:second.isEmpty()?first:first+", "+second;}
}

// One portal request: its Response arrives as a signal on the handle the
// portal makes from the token, so it is subscribed before asking.
class PortalRequest final:public QObject {
 Q_OBJECT
public:
 // An empty failure: opened. "cancelled" or "no_answer": not opened, and not
 // to be tried another way. Anything else: the D-Bus error or response.
 using Finished=std::function<void(const QString& failure)>;
 explicit PortalRequest(Finished finished):finished_(std::move(finished)){}
 void start(const QString& method,QVariantList arguments){
  auto bus=QDBusConnection::sessionBus();
  if(!bus.isConnected()){finish("no_session_bus");return;}
  const auto token="putgd"+QUuid::createUuid().toString(QUuid::Id128).left(16);
  subscribe(portalPath+"/request/"+bus.baseService().mid(1).replace('.','_')+"/"+token);
  arguments.append(QVariantMap{{"handle_token",token}});
  auto call=QDBusMessage::createMethodCall(portalService,portalPath,openUriInterface,method);
  call.setArguments(arguments);
  auto* watcher=new QDBusPendingCallWatcher(bus.asyncCall(call,callTimeoutMs),this);
  connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher]{
   watcher->deleteLater();
   const QDBusPendingReply<QDBusObjectPath> reply=*watcher;
   if(reply.isError()){finish(reply.error().name());return;}
   // An old portal may name another handle than the token's.
   if(const auto handle=reply.value().path();handle!=handle_){unsubscribe();subscribe(handle);}
  });
  QTimer::singleShot(responseTimeoutMs,this,[this]{finish("no_answer");});
 }
public slots:
 void response(uint code,const QVariantMap&){finish(code==0?QString():code==1?QStringLiteral("cancelled"):"portal_response_"+QString::number(code));}
private:
 void subscribe(const QString& handle){handle_=handle;QDBusConnection::sessionBus().connect(portalService,handle,requestInterface,"Response",this,SLOT(response(uint,QVariantMap)));}
 void unsubscribe(){if(!handle_.isEmpty())QDBusConnection::sessionBus().disconnect(portalService,handle_,requestInterface,"Response",this,SLOT(response(uint,QVariantMap)));handle_.clear();}
 void finish(const QString& failure){
  if(done_)return;
  done_=true;unsubscribe();
  const auto finished=std::move(finished_);
  deleteLater();
  finished(failure);
 }
 Finished finished_;QString handle_;bool done_=false;
};

void setOpenTimeoutsMs(int callMs,int responseMs){callTimeoutMs=qMax(1,callMs);responseTimeoutMs=qMax(1,responseMs);}
void setTestOpener(std::function<void(const QUrl&)> opener){testOpener=std::move(opener);}

void openLink(const QUrl& link,Opened done){
 if(!link.isValid()||(link.scheme()!="https"&&link.scheme()!="http")){report(done,{{"route","none"},{"opened",false},{"error","unsupported_link"}});return;}
 if(tested(link,done))return;
 const auto uri=link.toString(QUrl::FullyEncoded);
 auto* request=new PortalRequest([uri,done](const QString& failure){
  if(failure.isEmpty())report(done,{{"route","portal"},{"opened",true}});
  else if(failure=="cancelled"||failure=="no_answer")report(done,{{"route","portal"},{"opened",false},{"error",failure}});
  else report(done,launch(uri,{{"fallbackFrom",failure}}));
 });
 request->start("OpenURI",{QString(),uri});
}

namespace {
void openDirectory(const QString& path,const QString& earlier,const Opened& done){
 const auto folder=QUrl::fromLocalFile(QFileInfo(path).absolutePath()).toString(QUrl::FullyEncoded);
 if(!(QDBusConnection::sessionBus().connectionCapabilities()&QDBusConnection::UnixFileDescriptorPassing)){report(done,launch(folder,{{"fallbackFrom",fallbackReason(earlier,"no_fd_passing")}}));return;}
 const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NOCTTY);
 if(fd<0){report(done,launch(folder,{{"fallbackFrom",fallbackReason(earlier,"open_failed")}}));return;}
 // The descriptor is copied into the message; this one is closed at once.
 const QDBusUnixFileDescriptor descriptor(fd);::close(fd);
 auto* request=new PortalRequest([folder,earlier,done](const QString& failure){
  if(failure.isEmpty())report(done,{{"route","portal"},{"opened",true},{"fallbackFrom",earlier}});
  else if(failure=="cancelled"||failure=="no_answer")report(done,{{"route","portal"},{"opened",false},{"error",failure},{"fallbackFrom",earlier}});
  else report(done,launch(folder,{{"fallbackFrom",fallbackReason(earlier,failure)}}));
 });
 request->start("OpenDirectory",{QString(),QVariant::fromValue(descriptor)});
}
}

void showInFolder(const QString& path,Opened done){
 const QFileInfo file(path);
 if(!file.isAbsolute()||!file.exists()){report(done,{{"route","none"},{"opened",false},{"error","missing"}});return;}
 if(tested(QUrl::fromLocalFile(file.absolutePath()),done))return;
 // The file manager selects the file; the portal's OpenDirectory asks it the
 // same where it can, so it comes second.
 auto call=QDBusMessage::createMethodCall("org.freedesktop.FileManager1","/org/freedesktop/FileManager1","org.freedesktop.FileManager1","ShowItems");
 call.setArguments({QStringList{QUrl::fromLocalFile(file.absoluteFilePath()).toString(QUrl::FullyEncoded)},QString{}});
 auto* watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call,qMin(callTimeoutMs,5000)));
 QObject::connect(watcher,&QDBusPendingCallWatcher::finished,[watcher,path=file.absoluteFilePath(),done]{
  watcher->deleteLater();
  if(!watcher->isError()){report(done,{{"route","file-manager"},{"opened",true}});return;}
  openDirectory(path,watcher->error().name(),done);
 });
}

void openFile(const QString& path,Opened done){
 const QFileInfo file(path);
 if(!file.isAbsolute()||!file.exists()){report(done,{{"route","none"},{"opened",false},{"error","missing"}});return;}
 if(tested(QUrl::fromLocalFile(file.absoluteFilePath()),done))return;
 report(done,launch(QUrl::fromLocalFile(file.absoluteFilePath()).toString(QUrl::FullyEncoded),{}));
}
}
#include "HostOpen.moc"
