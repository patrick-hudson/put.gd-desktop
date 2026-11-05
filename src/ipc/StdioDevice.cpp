#include "StdioDevice.h"
#include <QSocketNotifier>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace putgd {
UiControlPipes::UiControlPipes(){
#ifdef Q_OS_UNIX
 struct stat inputStat{},outputStat{};
 if(fstat(STDIN_FILENO,&inputStat)||fstat(STDOUT_FILENO,&outputStat)||!S_ISFIFO(inputStat.st_mode)||!S_ISFIFO(outputStat.st_mode))return;
 const int input=fcntl(STDIN_FILENO,F_DUPFD_CLOEXEC,3);
 const int output=fcntl(STDOUT_FILENO,F_DUPFD_CLOEXEC,3);
 const int empty=open("/dev/null",O_RDONLY|O_CLOEXEC);
 if(input<0||output<0||empty<0){
  if(input>=0)::close(input);
  if(output>=0)::close(output);
  if(empty>=0)::close(empty);
  return;
 }
 if(dup2(STDERR_FILENO,STDOUT_FILENO)<0||dup2(empty,STDIN_FILENO)<0){
  dup2(input,STDIN_FILENO);dup2(output,STDOUT_FILENO);
  ::close(input);::close(output);::close(empty);return;
 }
 ::close(empty);input_=input;output_=output;
#endif
}
UiControlPipes::~UiControlPipes(){
#ifdef Q_OS_UNIX
 if(input_>=0)::close(input_);
 if(output_>=0)::close(output_);
#endif
}
StdioDevice::StdioDevice(int input,int output,QObject* parent):QIODevice(parent),input_(input),output_(output){
#ifdef Q_OS_UNIX
 inputFlags_=fcntl(input_,F_GETFL);outputFlags_=fcntl(output_,F_GETFL);
 if(inputFlags_<0||outputFlags_<0||fcntl(input_,F_SETFL,inputFlags_|O_NONBLOCK)<0||fcntl(output_,F_SETFL,outputFlags_|O_NONBLOCK)<0){setErrorString("Cannot open interface control pipes");return;}
 open(QIODevice::ReadWrite|QIODevice::Unbuffered);
 reader_=new QSocketNotifier(input_,QSocketNotifier::Read,this);
 writer_=new QSocketNotifier(output_,QSocketNotifier::Write,this);writer_->setEnabled(false);
 connect(reader_,&QSocketNotifier::activated,this,[this]{emit readyRead();});
 connect(writer_,&QSocketNotifier::activated,this,[this]{writer_->setEnabled(false);emit bytesWritten(0);});
#else
 setErrorString("Interface control pipes are not implemented on this platform");
#endif
}
StdioDevice::~StdioDevice(){
#ifdef Q_OS_UNIX
 if(inputFlags_>=0)fcntl(input_,F_SETFL,inputFlags_);
 if(outputFlags_>=0)fcntl(output_,F_SETFL,outputFlags_);
#endif
}
qint64 StdioDevice::bytesAvailable()const{
#ifdef Q_OS_UNIX
 if(!isOpen()||eof_)return 0;
 int bytes=0;if(ioctl(input_,FIONREAD,&bytes)<0)return 1;
 // A readable empty pipe means EOF; let readData observe it once.
 return qMax(1,bytes);
#else
 return 0;
#endif
}
qint64 StdioDevice::readData(char* data,qint64 count){
#ifdef Q_OS_UNIX
 ssize_t result;do{result=::read(input_,data,size_t(count));}while(result<0&&errno==EINTR);
 if(result==0){eof_=true;reader_->setEnabled(false);emit readChannelFinished();}
 if(result<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;
 return result;
#else
 Q_UNUSED(data);Q_UNUSED(count);return -1;
#endif
}
qint64 StdioDevice::writeData(const char* data,qint64 count){
#ifdef Q_OS_UNIX
 ssize_t result;do{result=::write(output_,data,size_t(count));}while(result<0&&errno==EINTR);
 if(result<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){writer_->setEnabled(true);return 0;}
 return result;
#else
 Q_UNUSED(data);Q_UNUSED(count);return -1;
#endif
}
}
