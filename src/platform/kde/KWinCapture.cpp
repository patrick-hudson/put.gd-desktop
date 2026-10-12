#include "KWinCapture.h"
#include "KWinTransfer.h"
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDBusUnixFileDescriptor>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
namespace putgd {
namespace {
const QString service="org.kde.KWin.ScreenShot2",path="/org/kde/KWin/ScreenShot2",iface=service;
bool installedEntryMatches(){
 auto entries=QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,"applications/putgd.desktop");
 // AppImage mounts vary by launch. AppRun registers a private, temporary
 // permission entry on the host; the same canonical executable check applies.
 for(const auto& location:QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
  QDir applications(QDir(location).filePath("applications"));
  for(const auto& name:applications.entryList({"putgd-appimage-*.desktop"},QDir::Files))entries.append(applications.filePath(name));
 }
 const auto actual=QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
 for(const auto& entry:entries){QFile file(entry);if(!file.open(QIODevice::ReadOnly))continue;QString exec,restricted;bool section=false;
  while(!file.atEnd()){const auto line=QString::fromUtf8(file.readLine()).trimmed();if(line.startsWith('[')){section=line=="[Desktop Entry]";continue;}if(!section)continue;if(line.startsWith("Exec="))exec=line.mid(5);if(line.startsWith("X-KDE-DBUS-Restricted-Interfaces="))restricted=line.section('=',1);}
  const auto command=QProcess::splitCommand(exec);if(command.isEmpty()||!restricted.split(';').contains(iface))continue;
  auto binary=command.first();if(!QFileInfo(binary).isAbsolute())binary=QStandardPaths::findExecutable(binary);
  if(!actual.isEmpty()&&QFileInfo(binary).canonicalFilePath()==actual)return true;
 }return false;
}
struct Transfer final:QObject {
 std::shared_ptr<std::atomic_bool> cancelled=std::make_shared<std::atomic_bool>(false);AsyncFdReader reader;KWinCapture::Reply reply;ResourceLimits limits;QVariantMap metadata;QByteArray bytes;bool dbusDone=false,fdDone=false,decoding=false,finished=false;
 explicit Transfer(QObject* parent):QObject(parent),reader(this){}
 void finish(QImage image={},Error error={}){if(finished)return;finished=true;cancelled->store(true);auto cb=std::move(reply);reply={};reader.cancel();QPointer<Transfer> self(this);if(cb)cb(std::move(image),metadata,std::move(error));if(self)deleteLater();}
};
}
KWinCapture::KWinCapture(QObject* parent):QObject(parent),watcher_(service,QDBusConnection::sessionBus(),QDBusServiceWatcher::WatchForOwnerChange,this){
 connect(&watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{cancel();version_=0;error_="KWin screenshot service changed";emit changed();probe();});probe();
}
KWinCapture::~KWinCapture(){cancel();for(auto* worker:workers_){worker->wait();delete worker;}}
void KWinCapture::probe(){
 const auto generation=++probeGeneration_;permissionDenied_=false;entryMatches_=installedEntryMatches();auto message=QDBusMessage::createMethodCall(service,path,"org.freedesktop.DBus.Properties","Get");message.setArguments({iface,"Version"});message.setAutoStartService(false);
 auto* call=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,3000),this);
 connect(call,&QDBusPendingCallWatcher::finished,this,[this,call,generation]{QDBusPendingReply<QDBusVariant> reply=*call;call->deleteLater();if(generation!=probeGeneration_)return;if(reply.isError()){version_=0;error_="KWin ScreenShot2 service is unavailable";}else{version_=reply.value().variant().toUInt();error_=version_<4?"ScreenShot2 version 4 or later is required":!entryMatches_?"Install the restricted desktop entry against this resident executable":QString{};}emit changed();});
}
QJsonObject KWinCapture::status()const{return {{"available",available()},{"interface","org.kde.KWin.ScreenShot2"},{"interface_version",int(version_)},{"permission_entry_matches_executable",entryMatches_},{"permission_rejected",permissionDenied_},{"permission","restricted-desktop-entry"},{"mode","compositor-native"},{"error",error_}};}
void KWinCapture::request(const QString& method,const QVariantList& args,const ResourceLimits& limits,Reply reply){
 if(!available()){reply({}, {}, {entryMatches_&&!permissionDenied_?"capture_unavailable":"capture_permission",error_,{}});return;}
 int pipes[2];if(pipe2(pipes,O_CLOEXEC|O_NONBLOCK)<0){reply({}, {}, {"transfer_fd","Cannot create screenshot transfer pipe",{}});return;}
 auto* transfer=new Transfer(this);transfer->reply=std::move(reply);transfer->limits=limits;requests_.append(transfer);connect(transfer,&QObject::destroyed,this,[this,transfer]{requests_.removeOne(transfer);});
 QPointer<Transfer> guard(transfer);
 auto maybeDecode=[this,guard]{if(!guard||guard->finished||guard->decoding||!guard->dbusDone||!guard->fdDone)return;guard->decoding=true;
  const auto metadata=guard->metadata;const auto limits=guard->limits;const auto bytes=std::move(guard->bytes);const auto cancelled=guard->cancelled;auto image=std::make_shared<QImage>();auto error=std::make_shared<Error>();
  auto* worker=QThread::create([metadata,limits,bytes,image,error,cancelled]{if(cancelled->load()){*error={"cancelled","Capture decoding cancelled",{}};return;}*image=decodeKWinImage(metadata,bytes,limits,*error);});workers_.append(worker);
  connect(worker,&QThread::finished,this,[this,worker,guard,image,error]{workers_.removeOne(worker);worker->deleteLater();if(guard&&!guard->finished)guard->finish(*image,*error);});worker->start();
 };
 transfer->reader.start(pipes[0],limits.imageBytes,10000,[guard,maybeDecode](QByteArray bytes,Error error){if(!guard||guard->finished)return;if(error){guard->finish({},error);return;}guard->fdDone=true;guard->bytes=std::move(bytes);maybeDecode();});
 QVariantMap options{{"include-cursor",false},{"native-resolution",true},{"include-decoration",true},{"include-shadow",false}};if(version_>=5)options["hide-caller-windows"]=false;
 auto message=QDBusMessage::createMethodCall(service,path,iface,method);auto arguments=args;arguments.append(options);arguments.append(QVariant::fromValue(QDBusUnixFileDescriptor(pipes[1])));message.setArguments(arguments);message.setAutoStartService(false);
 auto pending=QDBusConnection::sessionBus().asyncCall(message,10000);close(pipes[1]);auto* call=new QDBusPendingCallWatcher(pending,transfer);
 connect(call,&QDBusPendingCallWatcher::finished,transfer,[this,call,guard,maybeDecode]{
  QDBusPendingReply<QVariantMap> reply=*call;call->deleteLater();if(!guard||guard->finished)return;
  if(reply.isError()){const auto name=reply.error().name();Error error{"capture_failed","KWin capture failed; check desktop integration and retry",{{"dbus_error",name}}};if(name.endsWith(".NoAuthorized")){error={"capture_permission","KWin rejected screenshot permission; install the restricted entry against the running executable",{}};permissionDenied_=true;error_=error.message;emit changed();}else if(name.endsWith(".Cancelled"))error={"cancelled","KWin capture cancelled",{}};else if(name.endsWith(".InvalidWindow")||name.endsWith(".NoActiveWindow"))error={"target_changed","The invocation window is no longer available",{}};guard->finish({},error);return;}
  guard->metadata=reply.value();Error error;auto layout=validateKWinMetadata(guard->metadata,guard->limits,error);if(error){guard->finish({},error);return;}
  guard->dbusDone=true;guard->reader.setBound(layout.bytes);if(guard&&!guard->finished)maybeDecode();
 });
}
void KWinCapture::cancel(){const auto requests=requests_;for(auto* request:requests)static_cast<Transfer*>(request)->finish({},{"cancelled","KWin capture acquisition cancelled",{}});}
}
