#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QProcessEnvironment>
#include <QStringList>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
namespace putgd {
// The Proton helper process and its request and answer pipes. Unlike a QProcess it belongs
// to no thread: a helper started when an action is invoked is handed to the operation
// that then uses it. One thread uses it at a time. Its error output is discarded.
class HelperProcess {
public:
 HelperProcess()=default;
 HelperProcess(const HelperProcess&)=delete;
 HelperProcess& operator=(const HelperProcess&)=delete;
 ~HelperProcess(){
  kill();
  if(pid_>0&&!reaped_)while(::waitpid(pid_,&status_,0)<0&&errno==EINTR){}
  shut(in_);shut(out_);
 }
 bool start(const QString& program,const QStringList& arguments,const QProcessEnvironment& environment){
  int request[2],answer[2];
  if(::pipe2(request,O_CLOEXEC)!=0)return false;
  if(::pipe2(answer,O_CLOEXEC)!=0){::close(request[0]);::close(request[1]);return false;}
  QList<QByteArray> words{QFile::encodeName(program)},variables;
  for(const auto& argument:arguments)words.append(argument.toLocal8Bit());
  for(const auto& variable:environment.toStringList())variables.append(variable.toLocal8Bit());
  std::vector<char*> argv,envp;
  for(auto& word:words)argv.push_back(word.data());
  for(auto& variable:variables)envp.push_back(variable.data());
  argv.push_back(nullptr);envp.push_back(nullptr);
  posix_spawn_file_actions_t files;posix_spawn_file_actions_init(&files);
  posix_spawn_file_actions_adddup2(&files,request[0],STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&files,answer[1],STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&files,STDERR_FILENO,"/dev/null",O_WRONLY,0);
  // The helper starts with no signal blocked and SIGPIPE at its default, whatever this process does.
  posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);
  sigset_t none,pipe;sigemptyset(&none);sigemptyset(&pipe);sigaddset(&pipe,SIGPIPE);
  posix_spawnattr_setsigmask(&attributes,&none);posix_spawnattr_setsigdefault(&attributes,&pipe);
  posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF);
  pid_t pid=-1;
  const int failed=::posix_spawn(&pid,argv[0],&files,&attributes,argv.data(),envp.data());
  posix_spawn_file_actions_destroy(&files);posix_spawnattr_destroy(&attributes);
  ::close(request[0]);::close(answer[1]);
  if(failed){::close(request[1]);::close(answer[0]);return false;}
  pid_=pid;in_=request[1];out_=answer[0];
  ::fcntl(in_,F_SETFL,::fcntl(in_,F_GETFL)|O_NONBLOCK);
  ::fcntl(out_,F_SETFL,::fcntl(out_,F_GETFL)|O_NONBLOCK);
  return true;
 }
 qint64 pid() const {return pid_;}
 // False once the helper has ended.
 bool running(){
  if(pid_<=0||reaped_)return false;
  const auto ended=::waitpid(pid_,&status_,WNOHANG);
  if(ended==pid_)reaped_=true;
  else if(ended<0&&errno!=EINTR){reaped_=true;status_=-1;}
  return !reaped_;
 }
 bool exitedCleanly() const {return reaped_&&status_!=-1&&WIFEXITED(status_)&&WEXITSTATUS(status_)==0;}
 void kill(){if(pid_>0&&!reaped_)::kill(pid_,SIGKILL);}
 bool waitForFinished(int milliseconds){
  QElapsedTimer waited;waited.start();
  while(running()){
   if(waited.elapsed()>=milliseconds)return false;
   timespec nap{0,2000000};::nanosleep(&nap,nullptr);
  }
  return true;
 }
 // The helper reads its input to the end before it exits on its own.
 void closeInput(){shut(in_);}
 // Writes all of `bytes`, unless the helper has ended or reads nothing for `milliseconds`.
 qint64 write(const QByteArray& bytes,int milliseconds=5000){
  qint64 written=0;QElapsedTimer waited;waited.start();
  while(in_>=0&&written<bytes.size()){
   const auto count=quietWrite(in_,bytes.constData()+written,size_t(bytes.size()-written));
   if(count>0){written+=count;continue;}
   if(count<0&&errno==EINTR)continue;
   if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&waited.elapsed()<milliseconds){
    pollfd wait{in_,POLLOUT,0};::poll(&wait,1,int(milliseconds-waited.elapsed()));continue;
   }
   break;
  }
  return written;
 }
 // False once the helper has closed its output.
 bool outputOpen() const {return out_>=0;}
 // Waits for output, or its end, to be readable. With the output closed it waits for the exit.
 bool waitForReadable(int milliseconds){
  if(out_<0){waitForFinished(milliseconds);return false;}
  pollfd wait{out_,POLLIN,0};
  return ::poll(&wait,1,milliseconds)>0;
 }
 // What can be read now without waiting, up to `maximum` bytes.
 QByteArray read(qint64 maximum){
  QByteArray bytes;char buffer[65536];
  while(out_>=0&&bytes.size()<maximum){
   const auto count=::read(out_,buffer,size_t(qMin<qint64>(qint64(sizeof buffer),maximum-bytes.size())));
   if(count>0){bytes.append(buffer,qsizetype(count));continue;}
   if(count<0&&errno==EINTR)continue;
   if(count==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK))shut(out_);
   break;
  }
  return bytes;
 }
private:
 static void shut(int& descriptor){if(descriptor>=0)::close(descriptor);descriptor=-1;}
 // A write to a helper that has gone fails with EPIPE; it must not raise SIGPIPE here.
 static ssize_t quietWrite(int descriptor,const char* data,size_t size){
  sigset_t block,prior,pending;sigemptyset(&block);sigaddset(&block,SIGPIPE);
  pthread_sigmask(SIG_BLOCK,&block,&prior);sigpending(&pending);
  const bool already=sigismember(&pending,SIGPIPE);
  const auto count=::write(descriptor,data,size);const int failure=errno;
  if(count<0&&failure==EPIPE&&!already){timespec zero{};sigtimedwait(&block,nullptr,&zero);}
  pthread_sigmask(SIG_SETMASK,&prior,nullptr);errno=failure;
  return count;
 }
 pid_t pid_=-1;int in_=-1,out_=-1,status_=-1;bool reaped_=false;
};
}
