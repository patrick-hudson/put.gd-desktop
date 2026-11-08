#include "HyprlandIpc.h"
#include "platform/NativeSession.h"
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonParseError>
#include <QPointer>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <memory>

namespace putgd {
namespace {
constexpr int requestTimeoutMs=3000;
constexpr int connectRetryMs=50;
constexpr qsizetype maximumReply=8*1024*1024;
constexpr qsizetype maximumEventLine=64*1024;
// A non-blocking connected socket, or -1 with errno set.
int connectSocket(const QString& path,bool blocking){
 const QByteArray native=QFile::encodeName(path);
 sockaddr_un address{};address.sun_family=AF_UNIX;
 if(native.isEmpty()||size_t(native.size())>=sizeof address.sun_path){errno=ENAMETOOLONG;return -1;}
 std::memcpy(address.sun_path,native.constData(),size_t(native.size()));
 const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|(blocking?0:SOCK_NONBLOCK),0);
 if(fd<0)return -1;
 // A local stream connect completes or fails at once, even non-blocking.
 if(::connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof address)!=0){const int saved=errno;::close(fd);errno=saved;return -1;}
 return fd;
}
Error unavailable(const QString& message,const QString& request={}){
 QJsonObject details;if(!request.isEmpty())details["request"]=request.section(' ',0,0);
 return {"ipc_unavailable",message,details};
}
// One request on its own connection, driven by notifiers.
struct Exchange:QObject {
 int fd=-1;QByteArray out,in;qsizetype written=0;QString text;HyprlandIpc::Reply reply;
 QSocketNotifier* readable=nullptr;QSocketNotifier* writable=nullptr;QTimer timer;
 ~Exchange() override{if(fd>=0)::close(fd);}
 void finish(QByteArray data,Error error){
  if(readable)readable->setEnabled(false);if(writable)writable->setEnabled(false);timer.stop();
  if(fd>=0){::close(fd);fd=-1;}
  auto cb=std::move(reply);reply={};deleteLater();
  if(cb)cb(std::move(data),std::move(error));
 }
 void write(){
  while(written<out.size()){
   const auto sent=::send(fd,out.constData()+written,size_t(out.size()-written),MSG_NOSIGNAL);
   if(sent<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK){writable->setEnabled(true);return;}finish({},unavailable("Hyprland's IPC socket refused the request: "+QString::fromLocal8Bit(std::strerror(errno)),text));return;}
   written+=sent;
  }
  writable->setEnabled(false);
  // Hyprland reads until the client stops writing.
  ::shutdown(fd,SHUT_WR);
  readable->setEnabled(true);
 }
 void read(){
  char buffer[65536];
  for(;;){
   const auto count=::recv(fd,buffer,sizeof buffer,0);
   if(count<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return;finish({},unavailable("Hyprland's IPC reply failed: "+QString::fromLocal8Bit(std::strerror(errno)),text));return;}
   if(count==0){finish(in,{});return;}
   in.append(buffer,count);
   if(in.size()>maximumReply){finish({},unavailable("Hyprland's IPC reply exceeds the size put.gd reads",text));return;}
  }
 }
};
}
HyprlandIpc::HyprlandIpc(QObject* parent):QObject(parent){
 reconnect_.setInterval(1000);reconnect_.setSingleShot(true);
 connect(&reconnect_,&QTimer::timeout,this,[this]{if(listen_&&eventFd_<0)connectEvents();});
}
HyprlandIpc::~HyprlandIpc(){listen_=false;dropEvents();}
QString HyprlandIpc::requestSocket(){return hyprlandSocketPath(".socket.sock");}
QString HyprlandIpc::eventSocket(){return hyprlandSocketPath(".socket2.sock");}
bool HyprlandIpc::okReply(const QByteArray& reply){return reply.trimmed()=="ok";}
QString HyprlandIpc::luaString(const QString& text){
 const QByteArray utf8=text.toUtf8();
 QString out;out.reserve(utf8.size()+2);out+='"';
 for(const char c:utf8){
  const auto byte=static_cast<unsigned char>(c);
  if(byte=='"')out+="\\\"";
  else if(byte=='\\')out+="\\\\";
  // Decimal escapes keep the literal pure printable ASCII; Lua reads UTF-8 back byte for byte.
  else if(byte<0x20||byte>=0x7f)out+=QStringLiteral("\\%1").arg(int(byte),3,10,QChar('0'));
  else out+=QChar(byte);
 }
 out+='"';return out;
}
void HyprlandIpc::request(const QString& text,Reply reply){request(text,std::move(reply),QDeadlineTimer(requestTimeoutMs));}
void HyprlandIpc::request(const QString& text,Reply reply,QDeadlineTimer deadline){
 const auto path=requestSocket();
 if(path.isEmpty()){reply({},unavailable("HYPRLAND_INSTANCE_SIGNATURE does not name a Hyprland session",text));return;}
 const int fd=connectSocket(path,false);
 if(fd<0){
  // While Hyprland reloads its configuration it stops accepting for a moment
  // and a full backlog refuses with EAGAIN: try again within the deadline.
  if((errno==EAGAIN||errno==ECONNREFUSED)&&deadline.remainingTime()>connectRetryMs){
   QTimer::singleShot(connectRetryMs,this,[this,text,reply=std::move(reply),deadline]()mutable{request(text,std::move(reply),deadline);});
   return;
  }
  reply({},unavailable("Cannot connect to Hyprland's IPC socket: "+QString::fromLocal8Bit(std::strerror(errno)),text));return;
 }
 auto* exchange=new Exchange;exchange->setParent(this);exchange->fd=fd;exchange->out=text.toUtf8();exchange->text=text;exchange->reply=std::move(reply);
 exchange->readable=new QSocketNotifier(fd,QSocketNotifier::Read,exchange);exchange->readable->setEnabled(false);
 exchange->writable=new QSocketNotifier(fd,QSocketNotifier::Write,exchange);exchange->writable->setEnabled(false);
 connect(exchange->readable,&QSocketNotifier::activated,exchange,[exchange]{exchange->read();});
 connect(exchange->writable,&QSocketNotifier::activated,exchange,[exchange]{exchange->write();});
 exchange->timer.setSingleShot(true);
 connect(&exchange->timer,&QTimer::timeout,exchange,[exchange]{exchange->finish({},unavailable("Hyprland did not answer its IPC socket in time",exchange->text));});
 // One bound for the whole request, connect retries included.
 exchange->timer.start(deadline.isForever()?requestTimeoutMs:int(qMax<qint64>(1,deadline.remainingTime())));
 exchange->write();
}
void HyprlandIpc::json(const QString& text,JsonReply reply){
 request(text,[text,reply=std::move(reply)](QByteArray data,Error error){
  if(error){reply({},std::move(error));return;}
  QJsonParseError parse{};auto document=QJsonDocument::fromJson(data,&parse);
  if(parse.error!=QJsonParseError::NoError){reply({},{"ipc_reply","Hyprland's reply is not JSON",{{"request",text.section(' ',0,0)},{"reply",QString::fromUtf8(data.left(200))}}});return;}
  reply(std::move(document),{});
 });
}
namespace {
// poll() until ready, the deadline, or an error; a signal does not end the wait.
int pollWithin(int fd,short events,const QDeadlineTimer& deadline){
 for(;;){
  // remainingTime(): -1 for a deadline that never expires, which poll() takes as no timeout.
  pollfd ready{fd,events,0};const int count=::poll(&ready,1,int(deadline.remainingTime()));
  if(count<0&&errno==EINTR){if(deadline.hasExpired())return 0;continue;}
  return count;
 }
}
}
Error HyprlandIpc::requestBlocking(const QString& text,QByteArray* reply,int timeoutMs){
 const auto path=requestSocket();
 if(path.isEmpty())return unavailable("HYPRLAND_INSTANCE_SIGNATURE does not name a Hyprland session",text);
 QDeadlineTimer deadline(timeoutMs);
 int fd=connectSocket(path,false);
 while(fd<0&&(errno==EAGAIN||errno==ECONNREFUSED)&&deadline.remainingTime()>connectRetryMs){::usleep(connectRetryMs*1000);fd=connectSocket(path,false);}
 if(fd<0)return unavailable("Cannot connect to Hyprland's IPC socket: "+QString::fromLocal8Bit(std::strerror(errno)),text);
 const auto close=qScopeGuard([fd]{::close(fd);});const QByteArray out=text.toUtf8();qsizetype written=0;QByteArray in;
 while(written<out.size()){
  if(pollWithin(fd,POLLOUT,deadline)<=0)return unavailable("Hyprland did not take the IPC request in time",text);
  const auto sent=::send(fd,out.constData()+written,size_t(out.size()-written),MSG_NOSIGNAL);
  if(sent<0){if(errno==EINTR||errno==EAGAIN)continue;return unavailable("Hyprland's IPC socket refused the request",text);}
  written+=sent;
 }
 ::shutdown(fd,SHUT_WR);
 for(;;){
  if(pollWithin(fd,POLLIN,deadline)<=0)return unavailable("Hyprland did not answer its IPC socket in time",text);
  char buffer[16384];const auto count=::recv(fd,buffer,sizeof buffer,0);
  if(count<0){if(errno==EINTR||errno==EAGAIN)continue;return unavailable("Hyprland's IPC reply failed",text);}
  if(count==0)break;
  in.append(buffer,count);if(in.size()>maximumReply)return unavailable("Hyprland's IPC reply exceeds the size put.gd reads",text);
 }
 if(reply)*reply=in;
 return {};
}
void HyprlandIpc::listen(){listen_=true;if(eventFd_<0)connectEvents();}
bool HyprlandIpc::listening()const{return eventFd_>=0;}
void HyprlandIpc::connectEvents(){
 const auto path=eventSocket();
 const int fd=path.isEmpty()?-1:connectSocket(path,false);
 if(fd<0){reconnect_.start();return;}
 eventFd_=fd;eventBuffer_.clear();
 eventNotifier_=new QSocketNotifier(fd,QSocketNotifier::Read,this);
 connect(eventNotifier_,&QSocketNotifier::activated,this,&HyprlandIpc::readEvents);
 emit listeningChanged(true);
}
void HyprlandIpc::dropEvents(){
 if(eventNotifier_){eventNotifier_->setEnabled(false);eventNotifier_->disconnect(this);eventNotifier_->deleteLater();eventNotifier_=nullptr;}
 if(eventFd_>=0){::close(eventFd_);eventFd_=-1;}
 eventBuffer_.clear();
}
void HyprlandIpc::readEvents(){
 char buffer[16384];
 for(;;){
  const auto count=::recv(eventFd_,buffer,sizeof buffer,0);
  if(count<0&&errno==EINTR)continue;
  if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
  if(count<=0){
   // Hyprland exited or restarted; the next instance may come back on the same signature.
   dropEvents();emit listeningChanged(false);if(listen_)reconnect_.start();return;
  }
  eventBuffer_.append(buffer,count);
 }
 QPointer<HyprlandIpc> guard(this);
 for(qsizetype end=eventBuffer_.indexOf('\n');end>=0&&guard;end=eventBuffer_.indexOf('\n')){
  // A line this long is not an event put.gd reads, even when complete.
  if(end>maximumEventLine){eventBuffer_.remove(0,end+1);continue;}
  const QString line=QString::fromUtf8(eventBuffer_.left(end));eventBuffer_.remove(0,end+1);
  const auto split=line.indexOf(QStringLiteral(">>"));
  if(split>0)emit event(line.left(split),line.mid(split+2));
 }
 // A line this long is not an event put.gd reads.
 if(guard&&eventBuffer_.size()>maximumEventLine)eventBuffer_.clear();
}
}
