#include "LocalIpc.h"
#include <QFile>
#include <QQueue>
#include <QDeadlineTimer>
#include <climits>
#ifdef Q_OS_LINUX
#include <QSocketNotifier>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#else
#include <QLocalServer>
#include <QLocalSocket>
#endif
namespace putgd {
namespace {
constexpr int activationDeadlineMs=15000;
constexpr qsizetype maximumActivationResponse=16*1024;
bool frameComplete(QByteArray& response,QString& errorCode,qsizetype limit) {
 if(response.size()>limit)errorCode="response_too_large";
 else if(response.contains('\n')&&response.indexOf('\n')!=response.size()-1)errorCode="invalid_response";
 if(!errorCode.isEmpty()){response.clear();return true;}
 return response.endsWith('\n');
}
// A reply that never completed: the deadline passed, or the peer stopped.
void incompleteFrame(QByteArray& response,QString& errorCode,const QDeadlineTimer& deadline) {
 if(errorCode.isEmpty()&&!response.endsWith('\n')) {
  errorCode=deadline.hasExpired()?"activation_timeout":"transport_failed";
  response.clear();
 }
}
}
#ifdef Q_OS_LINUX
namespace {
bool waitSocket(int fd,short events,QDeadlineTimer& deadline) {
 for(;;) {
  if(deadline.hasExpired())return false;
  pollfd item{fd,events,0};
  const int result=::poll(&item,1,int(std::min<qint64>(deadline.remainingTime(),INT_MAX)));
  if(result<0&&errno==EINTR)continue;
  return result>0&&(item.revents&(events|POLLERR|POLLHUP));
 }
}
// A failed probe only identifies a stale socket on an explicit refused/missing reply.
static int connectSocket(const QString& path,int timeoutMs,LocalSessionState& state) {
 state=LocalSessionState::Unavailable;
 const auto name=QFile::encodeName(path);
 sockaddr_un address{};
 if(name.isEmpty()||name.contains('\0')||name.size()>=qsizetype(sizeof(address.sun_path)))return -1;
 address.sun_family=AF_UNIX;
 std::memcpy(address.sun_path,name.constData(),name.size()+1);
 const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
 if(fd<0)return -1;
 int result;do{result=::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));}while(result<0&&errno==EINTR);
 int error=result==0?0:errno;
 if(error==EINPROGRESS) {
  QDeadlineTimer deadline(timeoutMs);
  if(waitSocket(fd,POLLOUT,deadline)){socklen_t size=sizeof(error);if(::getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)error=errno;}
  else error=ETIMEDOUT;
 }
 if(error==0||error==EISCONN){state=LocalSessionState::Connected;return fd;}
 if(error==ECONNREFUSED||error==ENOENT)state=LocalSessionState::Absent;
 ::close(fd);
 return -1;
}
} // namespace
LocalSessionState probeLocalSession(const QString& path,int timeoutMs) {
 LocalSessionState state;
 const int fd=connectSocket(path,timeoutMs,state);
 if(fd>=0)::close(fd);
 return state;
}
quint64 processStartTime(qint64 pid) {
 QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
 if(pid<=0||!stat.open(QIODevice::ReadOnly))return 0;
 // The command name may hold spaces and parentheses; fields resume after the last ')'.
 const auto line=stat.readAll();const auto fields=line.mid(line.lastIndexOf(')')+1).split(' ');
 // fields[0] is empty (the space after ')'); field 22, starttime, is index 20.
 return fields.size()>20?fields[20].toULongLong():0;
}
namespace {
LocalPeer peerOf(int fd) {
 LocalPeer peer;peer.connected=true;
 ucred credentials{};socklen_t size=sizeof(credentials);
 if(::getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&size)==0&&size==sizeof(credentials)&&credentials.pid>0) {
  peer.pid=credentials.pid;peer.uid=credentials.uid;peer.start=processStartTime(peer.pid);
 }
 return peer;
}
}
namespace {
// Sends the whole request, then reads one reply frame of at most limit bytes.
void exchange(int fd,const QByteArray& request,QDeadlineTimer& deadline,qsizetype limit,bool& sent,QByteArray& response,QString& errorCode) {
 qsizetype done=0;
 while(done<request.size()&&!deadline.hasExpired()) {
  const auto count=::send(fd,request.constData()+done,size_t(request.size()-done),MSG_NOSIGNAL);
  if(count>0){done+=count;continue;}
  if(count<0&&errno==EINTR)continue;
  if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&waitSocket(fd,POLLOUT,deadline))continue;
  break;
 }
 sent=done==request.size();
 if(sent) {
  char chunk[16384];
  while(!deadline.hasExpired()) {
   const auto count=::recv(fd,chunk,size_t(std::min<qsizetype>(sizeof(chunk),limit+1-response.size())),0);
   if(count>0){response.append(chunk,count);if(frameComplete(response,errorCode,limit))break;continue;}
   if(count<0&&errno==EINTR)continue;
   if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&waitSocket(fd,POLLIN,deadline))continue;
   break;
  }
 }
 incompleteFrame(response,errorCode,deadline);
}
}
LocalActivation activateLocalSession(const QString& path,const QByteArray& request,const std::function<bool(const LocalPeer&)>& admit) {
 QDeadlineTimer deadline(activationDeadlineMs);
 LocalSessionState state;
 const int fd=connectSocket(path,200,state);
 if(fd<0)return {};
 LocalActivation result;result.connected=true;result.peer=peerOf(fd);
 if(admit&&!admit(result.peer)){::close(fd);result.refused=true;return result;}
 if(!request.isEmpty()){bool sent=false;exchange(fd,request,deadline,maximumActivationResponse,sent,result.response,result.errorCode);}
 ::close(fd);
 return result;
}
LocalExchange exchangeLocalSession(const QString& path,const QByteArray& request,int deadlineMs,qsizetype maximumResponse) {
 QDeadlineTimer deadline(deadlineMs);
 LocalSessionState state;
 const int fd=connectSocket(path,deadlineMs,state);
 if(fd<0)return {};
 LocalExchange result;result.connected=true;
 exchange(fd,request,deadline,maximumResponse,result.sent,result.response,result.errorCode);
 ::close(fd);
 return result;
}
struct LocalIpcConnection::State {
 LocalIpcConnection* owner;
 int fd;
 QSocketNotifier *readable,*writable;
 qsizetype limit=1024*1024+1;
 QByteArray output;
 qsizetype sent=0;
 bool requestHandled=false;
 State(LocalIpcConnection* q,int handle):owner(q),fd(handle),readable(new QSocketNotifier(fd,QSocketNotifier::Read,q)),writable(new QSocketNotifier(fd,QSocketNotifier::Write,q)) {
  writable->setEnabled(false);
  QObject::connect(readable,&QSocketNotifier::activated,q,[this]{
   if(requestHandled)discardInput();else emit owner->readyRead();
  });
  QObject::connect(writable,&QSocketNotifier::activated,q,[this]{flush();});
 }
 ~State(){if(fd>=0)::close(fd);}
 void close() {
  if(fd<0)return;
  readable->setEnabled(false);
  writable->setEnabled(false);
  ::close(fd);
  fd=-1;
  output.clear();
  emit owner->disconnected();
 }
 // Keep observing EOF while an asynchronous reply is pending. Ignore further
 // request bytes without retaining them or monopolizing an event-loop turn.
 void discardInput() {
  char chunk[16384];
  qsizetype drained=0;
  while(fd>=0&&drained<64*1024) {
   const auto count=::recv(fd,chunk,sizeof(chunk),0);
   if(count>0){drained+=count;continue;}
   if(count<0&&errno==EINTR)continue;
   if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return;
   close();
   return;
  }
 }
 void flush() {
  while(fd>=0&&sent<output.size()) {
   const auto count=::send(fd,output.constData()+sent,size_t(output.size()-sent),MSG_NOSIGNAL);
   if(count>0){sent+=count;continue;}
   if(count<0&&errno==EINTR)continue;
   if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){writable->setEnabled(true);return;}
   close();
   return;
  }
  close();
 }
};
LocalIpcConnection::LocalIpcConnection(qintptr handle,QObject* parent):QObject(parent),state_(std::make_unique<State>(this,int(handle))){}
LocalIpcConnection::~LocalIpcConnection()=default;
void LocalIpcConnection::setReadBufferSize(qsizetype size){state_->limit=size;}
QByteArray LocalIpcConnection::readAll() {
 QByteArray bytes;
 char chunk[16384];
 while(state_->fd>=0&&bytes.size()<state_->limit) {
  const auto count=::recv(state_->fd,chunk,size_t(std::min<qsizetype>(sizeof(chunk),state_->limit-bytes.size())),0);
  if(count>0){bytes.append(chunk,count);continue;}
  if(count<0&&errno==EINTR)continue;
  if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
  // Preserve a final complete request from a peer that half-closed its write side.
  if(count==0&&!bytes.isEmpty())state_->readable->setEnabled(false);else state_->close();
  break;
 }
 return bytes;
}
void LocalIpcConnection::stopReading(){
 state_->requestHandled=true;
 state_->readable->setEnabled(state_->fd>=0);
}
void LocalIpcConnection::sendAndClose(QByteArray bytes) {
 if(state_->fd<0)return;
 state_->readable->setEnabled(false);
 state_->output=std::move(bytes);
 state_->sent=0;
 state_->flush();
}
void LocalIpcConnection::disconnectFromServer(){state_->close();}
struct LocalIpcServer::State {
 LocalIpcServer* owner;
 int fd=-1;
 QString path;
 QSocketNotifier* notifier=nullptr;
 QQueue<LocalIpcConnection*> pending;
 dev_t device=0;
 ino_t inode=0;
 explicit State(LocalIpcServer* q):owner(q){}
 ~State() {
  if(notifier)notifier->setEnabled(false);
  if(fd>=0)::close(fd);
  if(!path.isEmpty()) {
   struct stat status{};
   const auto name=QFile::encodeName(path);
   if(::lstat(name.constData(),&status)==0&&status.st_dev==device&&
      status.st_ino==inode&&S_ISSOCK(status.st_mode))::unlink(name.constData());
  }
 }
 void accept() {
  for(;;) {
   const int client=::accept4(fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
   if(client<0){if(errno==EINTR)continue;return;}
   pending.enqueue(new LocalIpcConnection(client,owner));
   emit owner->newConnection();
  }
 }
};
LocalIpcServer::LocalIpcServer(QObject* parent):QObject(parent),state_(std::make_unique<State>(this)){}
LocalIpcServer::~LocalIpcServer()=default;
bool LocalIpcServer::listen(const QString& path) {
 if(state_->fd>=0)return false;
 const auto name=QFile::encodeName(path);
 sockaddr_un address{};
 if(name.isEmpty()||name.contains('\0')||name.size()>=qsizetype(sizeof(address.sun_path)))return false;
 address.sun_family=AF_UNIX;
 std::memcpy(address.sun_path,name.constData(),name.size()+1);
 const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
 if(fd<0)return false;
 if(::bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){::close(fd);return false;}
 struct stat status{};
 if(::lstat(name.constData(),&status)!=0||::chmod(name.constData(),0600)!=0||::listen(fd,64)!=0){::close(fd);::unlink(name.constData());return false;}
 state_->fd=fd;
 state_->path=path;
 state_->device=status.st_dev;
 state_->inode=status.st_ino;
 state_->notifier=new QSocketNotifier(fd,QSocketNotifier::Read,this);
 connect(state_->notifier,&QSocketNotifier::activated,this,[this]{state_->accept();});
 return true;
}
QString LocalIpcServer::fullServerName()const{return state_->path;}
LocalIpcConnection* LocalIpcServer::nextPendingConnection(){return state_->pending.isEmpty()?nullptr:state_->pending.dequeue();}
bool LocalIpcServer::removeServer(const QString& path){return ::unlink(QFile::encodeName(path).constData())==0;}
#else
LocalSessionState probeLocalSession(const QString& path,int timeoutMs) {
 QLocalSocket socket;socket.connectToServer(path);if(socket.waitForConnected(timeoutMs))return LocalSessionState::Connected;
 return socket.error()==QLocalSocket::ServerNotFoundError||socket.error()==QLocalSocket::ConnectionRefusedError?LocalSessionState::Absent:LocalSessionState::Unavailable;
}
quint64 processStartTime(qint64) {return 0;}
namespace {
void exchange(QLocalSocket& socket,const QByteArray& request,QDeadlineTimer& deadline,qsizetype limit,bool& sent,QByteArray& response,QString& errorCode) {
 bool written=socket.write(request)==request.size();
 while(written&&socket.bytesToWrite()>0&&!deadline.hasExpired())written=socket.waitForBytesWritten(int(deadline.remainingTime()));
 sent=written&&socket.bytesToWrite()==0;
 if(sent) {
  while(!deadline.hasExpired()) {
   response+=socket.read(limit+1-response.size());
   if(frameComplete(response,errorCode,limit))break;
   if(socket.state()==QLocalSocket::UnconnectedState||!socket.waitForReadyRead(int(deadline.remainingTime())))break;
  }
 }
 incompleteFrame(response,errorCode,deadline);
}
}
LocalActivation activateLocalSession(const QString& path,const QByteArray& request,const std::function<bool(const LocalPeer&)>& admit) {
 QDeadlineTimer deadline(activationDeadlineMs);
 QLocalSocket socket;socket.setReadBufferSize(maximumActivationResponse+1);socket.connectToServer(path);
 if(!socket.waitForConnected(200))return {};
 LocalActivation result;result.connected=true;result.peer.connected=true;
 if(admit&&!admit(result.peer)){result.refused=true;return result;}
 if(request.isEmpty())return result;
 bool sent=false;exchange(socket,request,deadline,maximumActivationResponse,sent,result.response,result.errorCode);
 return result;
}
LocalExchange exchangeLocalSession(const QString& path,const QByteArray& request,int deadlineMs,qsizetype maximumResponse) {
 QDeadlineTimer deadline(deadlineMs);
 QLocalSocket socket;socket.setReadBufferSize(maximumResponse+1);socket.connectToServer(path);
 if(!socket.waitForConnected(deadlineMs))return {};
 LocalExchange result;result.connected=true;
 exchange(socket,request,deadline,maximumResponse,result.sent,result.response,result.errorCode);
 return result;
}
struct LocalIpcConnection::State {QLocalSocket* socket;explicit State(QLocalSocket* value):socket(value){}};
LocalIpcConnection::LocalIpcConnection(qintptr handle,QObject* parent):QObject(parent),state_(std::make_unique<State>(reinterpret_cast<QLocalSocket*>(handle))) {
 state_->socket->setParent(this);connect(state_->socket,&QLocalSocket::readyRead,this,&LocalIpcConnection::readyRead);connect(state_->socket,&QLocalSocket::disconnected,this,&LocalIpcConnection::disconnected);
}
LocalIpcConnection::~LocalIpcConnection()=default;
void LocalIpcConnection::setReadBufferSize(qsizetype size){state_->socket->setReadBufferSize(size);}
QByteArray LocalIpcConnection::readAll(){return state_->socket->readAll();}
void LocalIpcConnection::stopReading(){QObject::disconnect(state_->socket,&QLocalSocket::readyRead,this,&LocalIpcConnection::readyRead);}
void LocalIpcConnection::sendAndClose(QByteArray bytes){state_->socket->write(bytes);state_->socket->disconnectFromServer();}
void LocalIpcConnection::disconnectFromServer(){state_->socket->disconnectFromServer();}
struct LocalIpcServer::State {QLocalServer server;};
LocalIpcServer::LocalIpcServer(QObject* parent):QObject(parent),state_(std::make_unique<State>()) {state_->server.setSocketOptions(QLocalServer::UserAccessOption);connect(&state_->server,&QLocalServer::newConnection,this,&LocalIpcServer::newConnection);}
LocalIpcServer::~LocalIpcServer()=default;
bool LocalIpcServer::listen(const QString& path){return state_->server.listen(path);}
QString LocalIpcServer::fullServerName()const{return state_->server.fullServerName();}
LocalIpcConnection* LocalIpcServer::nextPendingConnection(){auto* socket=state_->server.nextPendingConnection();return socket?new LocalIpcConnection(reinterpret_cast<qintptr>(socket),this):nullptr;}
bool LocalIpcServer::removeServer(const QString& path){return QLocalServer::removeServer(path);}
#endif
}
