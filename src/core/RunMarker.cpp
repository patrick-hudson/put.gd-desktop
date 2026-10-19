#include "RunMarker.h"
#include "Log.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <mutex>
#ifdef Q_OS_UNIX
#include <csignal>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif
namespace putgd {
namespace {
std::mutex mutex;
QString root;
QJsonObject current;
#ifdef Q_OS_UNIX
volatile sig_atomic_t signalFd=-1;
// Async-signal-safe: one write of the decimal signal number, then the default action.
void noteSignal(int number) {
 const int fd=signalFd;
 if(fd>=0) {
  char text[8];int length=0;int value=number;
  char reversed[8];do{reversed[length++]=char('0'+value%10);value/=10;}while(value>0&&length<7);
  for(int i=0;i<length;++i)text[i]=reversed[length-1-i];
  text[length++]='\n';
  [[maybe_unused]] const auto written=::write(fd,text,size_t(length));
 }
 ::signal(number,SIG_DFL);::raise(number);
}
#endif
QByteArray readBounded(const QString& path,qint64 limit) {QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>limit)return {};return file.readAll();}
bool savePrivate(const QString& path,const QByteArray& bytes) {
 QSaveFile file(path);file.setDirectWriteFallback(false);
 return file.open(QIODevice::WriteOnly)&&file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)&&file.write(bytes)==bytes.size()&&file.commit();
}
QString bootId() {const auto bytes=readBounded("/proc/sys/kernel/random/boot_id",128);return QString::fromLatin1(bytes).trimmed();}
bool residentAlive(qint64 pid) {
#ifdef Q_OS_LINUX
 if(pid<=0||pid==QCoreApplication::applicationPid())return false;
 return QString::fromLatin1(readBounded(QString("/proc/%1/comm").arg(pid),64)).trimmed()=="putgd";
#else
 Q_UNUSED(pid);return false;
#endif
}
}
void RunMarker::begin(const QString& stateRoot,const QString& version) {
 std::lock_guard lock(mutex);
 root=stateRoot;QDir().mkpath(root);QFile::setPermissions(root,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 const auto marker=root+"/running.json",note=root+"/running.signal",history=root+"/unclean-exits.json";
 QJsonArray recent=QJsonDocument::fromJson(readBounded(history,256*1024)).array();
 current={{"detected",false},{"last",QJsonValue()}};
 const auto previous=QJsonDocument::fromJson(readBounded(marker,64*1024)).object();
 if(QFile::exists(marker)&&!residentAlive(previous["pid"].toInteger())) {
  bool numeric=false;const int number=QString::fromLatin1(readBounded(note,64)).split('\n').first().trimmed().toInt(&numeric);
  const auto boot=bootId();
  QJsonObject record{{"detectedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
   {"previousPid",previous["pid"]},{"previousStart",previous["started"]},{"previousVersion",previous["version"]},
   {"sameBoot",!boot.isEmpty()&&previous["bootId"].toString()==boot},{"cause",numeric&&number>0?"signal":"unknown"}};
  if(numeric&&number>0)record["signal"]=number;
  recent.prepend(record);while(recent.size()>retainedRecords)recent.removeLast();
  savePrivate(history,QJsonDocument(recent).toJson(QJsonDocument::Compact));
  current={{"detected",true},{"last",record}};
  // A logout's SIGTERM is expected; anything else ended without a clean shutdown.
  logging::write(logApp(),numeric&&number>0?QtInfoMsg:QtWarningMsg,
   numeric&&number>0?"The previous resident was ended by a signal":"The previous resident did not shut down cleanly",record);
 }
 current["recent"]=recent;
 savePrivate(marker,QJsonDocument(QJsonObject{{"pid",QCoreApplication::applicationPid()},{"started",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
  {"version",version},{"bootId",bootId()}}).toJson(QJsonDocument::Compact));
#ifdef Q_OS_UNIX
 const auto path=QFile::encodeName(note);
 signalFd=::open(path.constData(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
 struct sigaction action{};action.sa_handler=noteSignal;sigemptyset(&action.sa_mask);
 for(const int number:{SIGTERM,SIGHUP,SIGINT}) {
  struct sigaction existing{};
  // Respect a disposition someone else chose (an ignored SIGHUP under nohup).
  if(::sigaction(number,nullptr,&existing)==0&&existing.sa_handler==SIG_DFL)::sigaction(number,&action,nullptr);
 }
#endif
}
void RunMarker::end() {
 std::lock_guard lock(mutex);
 if(root.isEmpty())return;
#ifdef Q_OS_UNIX
 for(const int number:{SIGTERM,SIGHUP,SIGINT}) {
  struct sigaction existing{};
  if(::sigaction(number,nullptr,&existing)==0&&existing.sa_handler==noteSignal)::signal(number,SIG_DFL);
 }
 const int fd=signalFd;signalFd=-1;if(fd>=0)::close(fd);
#endif
 QFile::remove(root+"/running.json");QFile::remove(root+"/running.signal");
 root.clear();
}
QJsonObject RunMarker::report() {
 std::lock_guard lock(mutex);
 return current.isEmpty()?QJsonObject{{"detected",false},{"last",QJsonValue()},{"recent",QJsonArray{}}}:current;
}
}
