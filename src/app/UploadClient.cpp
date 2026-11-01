// GLib first: Qt's keyword macros (signals) must not reach its headers.
#include <gio/gio.h>
#include "UploadClient.h"
#include "LocalIpc.h"
#include "LocalServer.h"
#include "ResidentSession.h"
#include "client/SessionClient.h"
#include "core/HostEnvironment.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
namespace putgd::UploadClient {
namespace {
// Every systemd-run call: newer systemd expands ${...} in a scope's arguments,
// so a selected /somewhere/${USER}.txt would otherwise become another file.
const QStringList scopeArguments{"--user","--scope","--quiet","--expand-environment=no","--"};
// One starter at a time, and how long a cold start may take: the resident
// AppRun's sycoca rebuild and journal recovery come before the socket answers.
constexpr int startDeadlineMs=60000;
// URI resolution has one deadline, shared by the batch call and any per-URI calls.
constexpr int resolveDeadlineMs=30000;
// The resident refuses requests over 1 MiB; leave room for the envelope.
constexpr qsizetype requestBudget=1024*1024-4096;
constexpr int pollIntervalMs=250;
const char* const scopedVariable="PUTGD_UPLOAD_SCOPED";
const char* const startedFdVariable="PUTGD_UPLOAD_STARTED_FD";

QString executablePath(){return QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();}
QString binaryDirectory(){return QFileInfo(executablePath()).absolutePath();}
// The private-library wrapper beside the binary, as login startup runs it.
QString systemLauncher(){const auto launcher=QDir(binaryDirectory()).filePath("putgd-launcher");return QFileInfo::exists(launcher)?launcher:executablePath();}

QList<QByteArray> environmentList(const QProcessEnvironment& environment){QList<QByteArray> list;for(const auto& key:environment.keys())list.append((key+'='+environment.value(key)).toLocal8Bit());return list;}
struct CStrings {
 QList<QByteArray> storage;std::vector<char*> pointers;
 explicit CStrings(QList<QByteArray> values):storage(std::move(values)){for(auto& value:storage)pointers.push_back(value.data());pointers.push_back(nullptr);}
 char** data(){return pointers.data();}
};
QList<QByteArray> encoded(const QStringList& values){QList<QByteArray> list;for(const auto& value:values)list.append(QFile::encodeName(value));return list;}

// What the user sees: put.gd's own notification, the same server the resident uses.
void notify(const QString& title,const QString& body,bool failure){
 const auto bytes=(title+": "+body).toUtf8();std::fwrite(bytes.constData(),1,size_t(bytes.size()),stderr);std::fputc('\n',stderr);
 auto bus=QDBusConnection::sessionBus();if(!bus.isConnected())return;
 // The AppImage integration names its stable icon copy; AppRun's points into a per-launch mount.
 const auto iconFile=qEnvironmentVariable("PUTGD_UPLOAD_ICON");
 const auto icon=QDir::isAbsolutePath(iconFile)&&QFileInfo(iconFile).isFile()?iconFile:QStringLiteral("putgd");
 auto call=QDBusMessage::createMethodCall("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","Notify");
 const QVariantMap hints{{"urgency",QVariant::fromValue(static_cast<uchar>(failure?2:1))},{"desktop-entry","putgd"}};
 call.setArguments({QStringLiteral("put.gd"),uint(0),icon,title.left(512),body.left(4096),QStringList{},hints,failure?0:10000});
 bus.call(call,QDBus::Block,5000);
}
void refuse(const QString& body){notify(QStringLiteral("put.gd — nothing uploaded"),body,true);}

// Leaving the file manager's .service: re-exec under a scope of our own. The
// re-executed client reports that it started through an inherited pipe, so a
// systemd-run that can't make the scope leaves this process to go on in place.
int leaveServiceCgroup(int argc,char** argv){
 if(qEnvironmentVariableIsSet(scopedVariable))return -1;
 QFile cgroup(QStringLiteral("/proc/self/cgroup"));
 if(!cgroup.open(QIODevice::ReadOnly)||!leafUnitIsService(cgroup.read(64*1024)))return -1;
 auto environment=QProcessEnvironment::systemEnvironment();
 const auto runner=host::findProgram("systemd-run",environment);if(runner.isEmpty())return -1;
 int started[2];if(::pipe2(started,O_CLOEXEC)!=0)return -1;
 // systemd-run is a host program: no private SDK libraries; the wrapper adds them again.
 const auto libraries=withoutPrivateLibraries(environment.value("LD_LIBRARY_PATH"),binaryDirectory());
 if(libraries.isEmpty())environment.remove("LD_LIBRARY_PATH");else environment.insert("LD_LIBRARY_PATH",libraries);
 environment.insert(scopedVariable,"1");environment.insert(startedFdVariable,QString::number(started[1]));
 QStringList arguments{runner};arguments<<scopeArguments<<systemLauncher();
 QList<QByteArray> argumentBytes=encoded(arguments);for(int i=1;i<argc;++i)argumentBytes.append(QByteArray(argv[i]));
 CStrings childArguments(argumentBytes),childEnvironment(environmentList(environment));
 const pid_t child=::fork();
 if(child<0){::close(started[0]);::close(started[1]);return -1;}
 if(child==0){::fcntl(started[1],F_SETFD,0);::execve(childArguments.data()[0],childArguments.data(),childEnvironment.data());::_exit(127);}
 ::close(started[1]);
 char byte=0;ssize_t count;do count=::read(started[0],&byte,1);while(count<0&&errno==EINTR);
 ::close(started[0]);
 int status=0;
 if(count==1){while(::waitpid(child,&status,0)<0&&errno==EINTR){}return WIFEXITED(status)?WEXITSTATUS(status):1;}
 while(::waitpid(child,&status,0)<0&&errno==EINTR){}
 return -1;
}
void reportScoped(){
 if(!qEnvironmentVariableIsSet(startedFdVariable))return;
 bool ok=false;const int fd=qEnvironmentVariableIntValue(startedFdVariable,&ok);
 if(ok&&fd>2){const char one='1';[[maybe_unused]]const auto written=::write(fd,&one,1);::close(fd);}
 qunsetenv(startedFdVariable);
}

// `gio info` for the AppImage: the host's gio, with the host's own GIO modules
// (the image bundles GLib without them), in an English UTF-8 output locale.
// Plain C would print é as ?, matching a different real file.
struct Child { QByteArray out; bool complete=false; };
Child runBounded(const QString& program,const QStringList& arguments,const QProcessEnvironment& environment,const QDeadlineTimer& deadline){
 Child result;
 int out[2],err[2];if(::pipe2(out,O_CLOEXEC)!=0)return result;if(::pipe2(err,O_CLOEXEC)!=0){::close(out[0]);::close(out[1]);return result;}
 QStringList all{program};all<<arguments;CStrings childArguments(encoded(all)),childEnvironment(environmentList(environment));
 const pid_t pid=::fork();
 if(pid<0){for(int fd:{out[0],out[1],err[0],err[1]})::close(fd);return result;}
 if(pid==0){
  const int null=::open("/dev/null",O_RDONLY);if(null>=0)::dup2(null,0);
  ::dup2(out[1],1);::dup2(err[1],2);
  ::execve(childArguments.data()[0],childArguments.data(),childEnvironment.data());::_exit(127);
 }
 ::close(out[1]);::close(err[1]);
 ::fcntl(out[0],F_SETFL,O_NONBLOCK);::fcntl(err[0],F_SETFL,O_NONBLOCK);
 // Both pipes drain together, so a child that fills stderr can't stall.
 bool outOpen=true,errOpen=true;char chunk[16384];
 while((outOpen||errOpen)&&!deadline.hasExpired()){
  pollfd fds[2]={{outOpen?out[0]:-1,POLLIN,0},{errOpen?err[0]:-1,POLLIN,0}};
  const int ready=::poll(fds,2,int(qMin<qint64>(deadline.remainingTime(),1000)));
  if(ready<0&&errno!=EINTR)break;
  for(int i=0;i<2;++i){
   if(!fds[i].revents)continue;
   const auto count=::read(fds[i].fd,chunk,sizeof(chunk));
   if(count>0){if(i==0&&result.out.size()<16*1024*1024)result.out.append(chunk,count);}
   else if(count==0||(errno!=EAGAIN&&errno!=EINTR)){if(i==0)outOpen=false;else errOpen=false;}
  }
 }
 ::close(out[0]);::close(err[0]);
 int status=0;
 if(outOpen||errOpen){
  // The deadline passed: TERM, then KILL a second later, and always reap.
  ::kill(pid,SIGTERM);
  for(int i=0;i<20&&::waitpid(pid,&status,WNOHANG)==0;++i)::usleep(50000);
  if(::waitpid(pid,&status,WNOHANG)==0){::kill(pid,SIGKILL);while(::waitpid(pid,&status,0)<0&&errno==EINTR){}}
  result.out.clear();return result;
 }
 // Output closed: the child is ending; wait for it inside the same deadline.
 while(!deadline.hasExpired()){const auto done=::waitpid(pid,&status,WNOHANG);if(done==pid)break;if(done<0&&errno!=EINTR)break;::usleep(10000);}
 if(::waitpid(pid,&status,WNOHANG)==0){::kill(pid,SIGKILL);while(::waitpid(pid,&status,0)<0&&errno==EINTR){}result.out.clear();return result;}
 result.complete=WIFEXITED(status);
 if(!result.complete)result.out.clear();
 return result;
}
// The in-process GIO queries share the URI deadline: a timer thread cancels them
// when it passes, so a stalled GVfs backend can't keep the client from reporting.
class QueryDeadline {
public:
 explicit QueryDeadline(int ms):timer_([this,ms]{std::unique_lock lock(mutex_);if(!wake_.wait_for(lock,std::chrono::milliseconds(ms),[this]{return done_;}))g_cancellable_cancel(cancellable_);}) {}
 ~QueryDeadline(){{std::lock_guard lock(mutex_);done_=true;}wake_.notify_one();timer_.join();g_object_unref(cancellable_);}
 GCancellable* cancellable() const {return cancellable_;}
private:
 GCancellable* cancellable_=g_cancellable_new();
 std::mutex mutex_;std::condition_variable wake_;bool done_=false;
 std::thread timer_;
};
QHash<QString,QString> hostGioInfo(const QStringList& uris){
 QHash<QString,QString> found;if(uris.isEmpty())return found;
 const QDeadlineTimer deadline(resolveDeadlineMs);
 auto environment=host::environment();
 const auto gio=host::findProgram("gio",environment);if(gio.isEmpty())return found;
 environment.insert("LC_ALL","C.UTF-8");
 for(const char* name:{"LANGUAGE","CHARSET","G_FILENAME_ENCODING","G_BROKEN_FILENAMES"})environment.remove(QString::fromLatin1(name));
 // C.UTF-8 comes from distribution packaging; trust gio's output only in a UTF-8 charset.
 const auto locale=host::findProgram("locale",environment);
 if(locale.isEmpty()||runBounded(locale,{"charmap"},environment,deadline).out.trimmed()!="UTF-8")return found;
 QStringList arguments{"info","-a","standard::type,standard::target-uri"};arguments<<uris;
 const auto batch=runBounded(gio,arguments,environment,deadline);
 if(batch.complete)found=parseGioInfo(batch.out);
 for(const auto& uri:uris){
  if(found.contains(normalizedUri(uri))||deadline.hasExpired())continue;
  const auto single=runBounded(gio,{"info","-a","standard::type,standard::target-uri",uri},environment,deadline);
  if(single.complete)found.insert(parseGioInfo(single.out));
 }
 return found;
}

QJsonObject request(const QString& method,const QJsonObject& params,Error& error){
 SessionClient client(LocalServer::defaultSocketPath());QEventLoop loop;QJsonObject result;
 client.request(method,params,&loop,[&](Error e,QJsonObject r){error=std::move(e);result=std::move(r);loop.quit();});
 loop.exec();return result;
}
void sleepMs(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}

// #490: a resident left from an ended login session is retired, never used.
enum class Probe { Running, Absent, Failed };
Probe probeResident(){
 const auto path=LocalServer::defaultSocketPath();ResidentSession session;
 const auto activation=activateLocalSession(path,{},[&session](const LocalPeer& peer){
  if(peer.pid<=0)return true;
  session=residentSession(*LoginSessions::system(),peer.pid,QCoreApplication::applicationPid());
  return !session.ended;
 });
 if(!activation.connected)return Probe::Absent;
 if(!activation.refused)return Probe::Running;
 return retireResident(path,activation.peer).done?Probe::Absent:Probe::Failed;
}

// The resident runs detached in a scope of its own, from $HOME, so it never
// keeps the file manager's working directory (a USB stick) busy or lives in
// its cgroup. Plain setsid exec when there is no systemd-run or it fails.
struct Start { pid_t pid=-1; bool scoped=false; };
Start startResident(bool appImage,const QString& appImagePath,bool scoped){
 auto environment=appImage?host::environment():QProcessEnvironment::systemEnvironment();
 QString program;
 if(appImage)program=appImagePath;
 else {
  program=systemLauncher();
  const auto libraries=withoutPrivateLibraries(environment.value("LD_LIBRARY_PATH"),binaryDirectory());
  if(libraries.isEmpty())environment.remove("LD_LIBRARY_PATH");else environment.insert("LD_LIBRARY_PATH",libraries);
 }
 // The resident skips its no-tray Settings window for this start; activation
 // tokens belong to the click that ran this client, not to a later window.
 environment.insert("PUTGD_STARTED_FOR_UPLOAD","1");
 for(const char* name:{"XDG_ACTIVATION_TOKEN","DESKTOP_STARTUP_ID",scopedVariable,startedFdVariable})environment.remove(QString::fromLatin1(name));
 const auto runner=scoped?host::findProgram("systemd-run",environment):QString();
 QStringList arguments;
 if(!runner.isEmpty())arguments<<runner<<scopeArguments;
 arguments<<program<<"--background";
 CStrings childArguments(encoded(arguments)),childEnvironment(environmentList(environment));
 const auto home=QFile::encodeName(QDir::homePath());
 const pid_t pid=::fork();
 if(pid==0){
  ::setsid();if(::chdir(home.constData())!=0&&::chdir("/")!=0)::_exit(127);
  const int null=::open("/dev/null",O_RDWR);if(null>=0){::dup2(null,0);::dup2(null,1);::dup2(null,2);}
  ::execve(childArguments.data()[0],childArguments.data(),childEnvironment.data());::_exit(127);
 }
 return {pid,!runner.isEmpty()};
}

// The start lock: an O_CLOEXEC descriptor (the resident never inherits it),
// polled without blocking because flock has no timeout.
int takeStartLock(const QDeadlineTimer& deadline){
 const auto directory=LocalServer::defaultRuntimeDirectory();if(directory.isEmpty())return -1;
 QDir().mkpath(directory);QFile::setPermissions(directory,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 const auto path=QFile::encodeName(QDir(directory).filePath("start.lock"));
 const int fd=::open(path.constData(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -1;
 while(::flock(fd,LOCK_EX|LOCK_NB)!=0){
  if(errno!=EWOULDBLOCK&&errno!=EINTR){::close(fd);return -1;}
  if(deadline.hasExpired()){::close(fd);return -1;}
  // Another client is starting the resident; it may be up already.
  if(probeLocalSession(LocalServer::defaultSocketPath(),200)==LocalSessionState::Connected){::close(fd);return -2;}
  ::usleep(100000);
 }
 return fd;
}
// Cold start: one starter, the resident started once, and the socket awaited.
bool ensureResident(bool appImage,const QString& appImagePath){
 const QDeadlineTimer deadline(startDeadlineMs);
 for(int attempt=0;attempt<2;++attempt){
  const auto probe=probeResident();
  if(probe==Probe::Running)return true;
  if(probe==Probe::Failed)return false;
  const int lock=takeStartLock(deadline);
  if(lock==-2)continue;
  if(lock<0)return false;
  const auto unlock=qScopeGuard([lock]{::close(lock);});
  if(probeLocalSession(LocalServer::defaultSocketPath(),200)==LocalSessionState::Connected)return true;
  auto started=startResident(appImage,appImagePath,true);
  if(started.pid<0)return false;
  while(!deadline.hasExpired()){
   if(probeLocalSession(LocalServer::defaultSocketPath(),200)==LocalSessionState::Connected)return true;
   int status=0;
   // A scope that could not be made ends at once; start the resident without one.
   if(started.pid>0&&::waitpid(started.pid,&status,WNOHANG)==started.pid){
    if(!started.scoped)return false;
    started=startResident(appImage,appImagePath,false);if(started.pid<0)return false;
    continue;
   }
   ::usleep(200000);
  }
  return false;
 }
 return probeResident()==Probe::Running;
}
}

bool requested(int argc,char** argv){return argc>=2&&std::strcmp(argv[1],"--upload")==0;}

bool leafUnitIsService(const QByteArray& cgroup){
 for(const auto& line:cgroup.split('\n')){
  // cgroup v2 only: "0::/user.slice/.../app.slice/dbus-:1.2-org.gnome.Nautilus@0.service".
  if(!line.startsWith("0::"))continue;
  const auto path=line.mid(3).trimmed();const auto leaf=path.mid(path.lastIndexOf('/')+1);
  return leaf.endsWith(".service");
 }
 return false;
}
QString withoutPrivateLibraries(const QString& libraryPath,const QString& binaryDirectory){
 const auto prefix=QDir::cleanPath(binaryDirectory+"/..")+"/lib";QStringList kept;
 for(const auto& entry:libraryPath.split(':')){
  const auto clean=QDir::cleanPath(entry);
  if(!entry.isEmpty()&&clean.startsWith(prefix)&&clean.endsWith("/putgd"))continue;
  kept.append(entry);
 }
 while(!kept.isEmpty()&&kept.last().isEmpty())kept.removeLast();
 return kept.join(':');
}
QString normalizedUri(const QString& uri){
 const auto bytes=uri.toUtf8();GError* error=nullptr;
 GUri* parsed=g_uri_parse(bytes.constData(),GUriFlags(G_URI_FLAGS_ENCODED|G_URI_FLAGS_PARSE_RELAXED),&error);
 if(!parsed){g_clear_error(&error);return uri;}
 // Scheme and host compare case-blind, and a scheme's default port is the same as none.
 static const QHash<QString,int> defaultPorts{{"sftp",22},{"ssh",22},{"smb",445},{"ftp",21},{"ftps",990},{"dav",80},{"http",80},{"davs",443},{"https",443},{"afp",548},{"nfs",2049}};
 const auto scheme=QString::fromUtf8(g_uri_get_scheme(parsed)).toLower();
 const auto host=g_uri_get_host(parsed)?QString::fromUtf8(g_uri_get_host(parsed)).toLower().toUtf8():QByteArray();
 int port=g_uri_get_port(parsed);if(port==defaultPorts.value(scheme,-2))port=-1;
 char* text=g_uri_join(G_URI_FLAGS_ENCODED,scheme.toUtf8().constData(),g_uri_get_userinfo(parsed),g_uri_get_host(parsed)?host.constData():nullptr,port,g_uri_get_path(parsed),g_uri_get_query(parsed),g_uri_get_fragment(parsed));
 g_uri_unref(parsed);
 const auto result=QString::fromUtf8(text);g_free(text);
 return result;
}
bool decodedNewline(const QString& uri){
 const auto decoded=QByteArray::fromPercentEncoding(uri.toUtf8());
 return decoded.contains('\n')||decoded.contains('\r')||uri.contains('\n')||uri.contains('\r');
}
QHash<QString,QString> parseGioInfo(const QByteArray& output){
 QHash<QString,QString> found;QString current,path;QByteArray target;
 // A record's local path, else the local file its target URI names: Recent and
 // Starred items (recent://, starred://) have only a target (#1105).
 const auto settle=[&]{
  if(!current.isEmpty()&&path.isEmpty()&&target.startsWith("file:"))
   if(char* local=g_filename_from_uri(target.constData(),nullptr,nullptr)){path=QFile::decodeName(local);g_free(local);}
  if(!current.isEmpty()&&QDir::isAbsolutePath(path))found.insert(current,path);
  current.clear();path.clear();target.clear();
 };
 for(const auto& line:output.split('\n')){
  if(line.startsWith("uri: ")){settle();current=normalizedUri(QString::fromUtf8(line.mid(5)));continue;}
  if(line.startsWith("local path: "))path=QFile::decodeName(line.mid(12));
  else if(line.trimmed().startsWith("standard::target-uri: "))target=line.trimmed().mid(22);
 }
 settle();
 return found;
}
Resolution resolve(const QStringList& arguments,bool appImage,const GioInfo& gioInfo){
 Resolution result;QStringList remote;QList<int> remoteIndexes;
 QStringList paths(arguments.size());QList<bool> resolved(arguments.size(),false);
 static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
 std::optional<QueryDeadline> deadline;
 for(int i=0;i<arguments.size();++i){
  const auto& argument=arguments[i];
  if(argument.startsWith('/')){paths[i]=argument;resolved[i]=true;continue;}
  if(!scheme.match(argument).hasMatch()||decodedNewline(argument))continue;
  const auto bytes=argument.toUtf8();
  if(appImage){
   // file:// needs no GIO module: the image's GLib decodes it in-process.
   if(argument.startsWith(QLatin1String("file:"),Qt::CaseInsensitive)){
    if(char* path=g_filename_from_uri(bytes.constData(),nullptr,nullptr)){paths[i]=QFile::decodeName(path);g_free(path);resolved[i]=QDir::isAbsolutePath(paths[i]);}
    continue;
   }
   remote.append(argument);remoteIndexes.append(i);continue;
  }
  // DEB/RPM: the host's GIO with its own modules, GVfs's FUSE paths included.
  GFile* file=g_file_new_for_uri(bytes.constData());
  char* path=g_file_get_path(file);
  // Recent and Starred items have no path of their own, only a target URI.
  if(!path&&!deadline)deadline.emplace(resolveDeadlineMs);
  if(!path)if(GFileInfo* info=g_file_query_info(file,G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,G_FILE_QUERY_INFO_NONE,deadline->cancellable(),nullptr)){
   // Only a file:// target: decoding it needs no GVfs call that could stall.
   if(const char* target=g_file_info_get_attribute_string(info,G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);target&&g_str_has_prefix(target,"file:"))path=g_filename_from_uri(target,nullptr,nullptr);
   g_object_unref(info);
  }
  if(path){paths[i]=QFile::decodeName(path);g_free(path);resolved[i]=QDir::isAbsolutePath(paths[i]);}
  g_object_unref(file);
 }
 if(!remote.isEmpty()&&gioInfo){
  const auto found=gioInfo(remote);
  for(int n=0;n<remote.size();++n){const auto path=found.value(normalizedUri(remote[n]));if(QDir::isAbsolutePath(path)){paths[remoteIndexes[n]]=path;resolved[remoteIndexes[n]]=true;}}
 }
 for(int i=0;i<arguments.size();++i)(resolved[i]?result.paths:result.unresolved).append(resolved[i]?paths[i]:arguments[i]);
 return result;
}

int run(int argc,char** argv){
 if(argc<3||std::strcmp(argv[2],"--")!=0){std::fputs("Usage: putgd --upload -- <absolute paths or URIs>\n",stderr);return 2;}
 reportScoped();
 // Both are read before the host environment is built, which drops them.
 const auto installation=host::installationClass();const bool appImage=installation=="appimage";
 const auto appImagePath=appImage?qEnvironmentVariable("APPIMAGE"):QString();
 // DEB/RPM leave a D-Bus-activated file manager's cgroup here; the AppImage's
 // stable launcher does it on the host, before the image is mounted.
 if(!appImage)if(const int status=leaveServiceCgroup(argc,argv);status>=0)return status;
 QStringList arguments;for(int i=3;i<argc;++i)arguments.append(QFile::decodeName(argv[i]));
 // Everything is resolved and checked before a Qt application object exists:
 // Qt would treat a selected "-platform" or "--style" as its own option.
 const auto resolution=resolve(arguments,appImage,hostGioInfo);
 int fakeArgc=1;char* fakeArgv[]={argv[0],nullptr};
 QCoreApplication application(fakeArgc,fakeArgv);
 QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd");
 if(arguments.isEmpty()){refuse(QStringLiteral("No files were selected."));return 2;}
 if(!resolution.unresolved.isEmpty()){
  QStringList names;for(const auto& item:resolution.unresolved.mid(0,8)){const QUrl url(item);names.append(url.isValid()&&!url.fileName().isEmpty()?url.fileName():item.left(120));}
  refuse(QStringLiteral("These items have no local path, so put.gd can't read them: %1%2").arg(names.join(", "),resolution.unresolved.size()>8?QStringLiteral(" and %1 more").arg(resolution.unresolved.size()-8):QString()));
  return 1;
 }
 QJsonObject params{{"paths",QJsonArray::fromStringList(resolution.paths)},{"installation",installation}};
 if(QJsonDocument(params).toJson(QJsonDocument::Compact).size()>requestBudget){
  refuse(QStringLiteral("Too many items were selected at once (%1). Select fewer, or upload their folder instead.").arg(resolution.paths.size()));return 1;
 }
 if(appImage&&(appImagePath.isEmpty()||!QFileInfo(appImagePath).isFile())){refuse(QStringLiteral("The put.gd AppImage could not be found. Start put.gd once, then try again."));return 1;}
 if(!ensureResident(appImage,appImagePath)){refuse(QStringLiteral("put.gd did not start. Start put.gd from your applications menu, then try again."));return 1;}
 // initializing is a refusal before any operation exists, so it is safe to resend.
 const QDeadlineTimer ready(startDeadlineMs);
 Error error;QJsonObject accepted;
 for(;;){
  error={};accepted=request("upload.files",params,error);
  if(error.code!="initializing"||ready.hasExpired())break;
  sleepMs(pollIntervalMs);
 }
 if(error){
  if(error.code=="unknown_method")refuse(QStringLiteral("put.gd was just updated. Quit put.gd from its tray menu or log out and in, then try again."));
  else if(error.details["outcomeUnknown"].toBool())notify(QStringLiteral("put.gd — upload uncertain"),QStringLiteral("put.gd didn't confirm the upload. Check put.gd's History before trying again."),true);
  else if(error.code=="client_unavailable"||error.code=="client_timeout")refuse(QStringLiteral("put.gd isn't answering. Try again in a moment."));
  else if(error.code=="initializing")refuse(QStringLiteral("put.gd is still starting. Try again in a moment."));
  else refuse(error.message.isEmpty()?error.code:error.message);
  return 1;
 }
 // Wait until put.gd holds its own copy: a KIO temporary copy or a GVfs file
 // must stay readable until then. Any state past acquiring ends the wait.
 const auto id=accepted["operationId"].toString();
 for(;;){
  Error inspectError;const auto operation=request("operation.inspect",{{"id",id}},inspectError);
  if(inspectError){
   if(inspectError.code=="client_unavailable"||inspectError.code=="client_timeout"||inspectError.code=="operation_not_found")
    refuse(QStringLiteral("put.gd stopped before it had a copy of the files. Check its History, then try again."));
   else refuse(inspectError.message.isEmpty()?inspectError.code:inspectError.message);
   return 1;
  }
  const auto state=operation["state"].toString();
  if(state=="accepted"||state=="acquiring"){sleepMs(pollIntervalMs);continue;}
  // The resident announces a failed operation itself; an interrupted one it doesn't.
  if(state=="interrupted"){refuse(QStringLiteral("put.gd stopped before it had a copy of the files. Check its History, then try again."));return 1;}
  return state=="failed"||state=="cancelled"?1:0;
 }
}
}
