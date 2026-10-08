#include "StdioDevice.h"
#include <QSocketNotifier>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
namespace putgd {
StdioDevice::StdioDevice(QObject* parent):QIODevice(parent){
#ifdef Q_OS_UNIX
 inputFlags_=fcntl(STDIN_FILENO,F_GETFL);outputFlags_=fcntl(STDOUT_FILENO,F_GETFL);
 if(inputFlags_<0||outputFlags_<0||fcntl(STDIN_FILENO,F_SETFL,inputFlags_|O_NONBLOCK)<0||fcntl(STDOUT_FILENO,F_SETFL,outputFlags_|O_NONBLOCK)<0){setErrorString("Cannot open interface control pipes");return;}
 open(QIODevice::ReadWrite|QIODevice::Unbuffered);
 reader_=new QSocketNotifier(STDIN_FILENO,QSocketNotifier::Read,this);
 writer_=new QSocketNotifier(STDOUT_FILENO,QSocketNotifier::Write,this);writer_->setEnabled(false);
 connect(reader_,&QSocketNotifier::activated,this,[this]{emit readyRead();});
 connect(writer_,&QSocketNotifier::activated,this,[this]{writer_->setEnabled(false);emit bytesWritten(0);});
#else
 setErrorString("Interface control pipes are not implemented on this platform");
#endif
}
StdioDevice::~StdioDevice(){
#ifdef Q_OS_UNIX
 if(inputFlags_>=0)fcntl(STDIN_FILENO,F_SETFL,inputFlags_);
 if(outputFlags_>=0)fcntl(STDOUT_FILENO,F_SETFL,outputFlags_);
#endif
}
qint64 StdioDevice::bytesAvailable()const{
#ifdef Q_OS_UNIX
 if(!isOpen()||eof_)return 0;
 int bytes=0;if(ioctl(STDIN_FILENO,FIONREAD,&bytes)<0)return 1;
 // A readable empty pipe means EOF; let readData observe it once.
 return qMax(1,bytes);
#else
 return 0;
#endif
}
qint64 StdioDevice::readData(char* data,qint64 count){
#ifdef Q_OS_UNIX
 ssize_t result;do{result=::read(STDIN_FILENO,data,size_t(count));}while(result<0&&errno==EINTR);
 if(result==0){eof_=true;reader_->setEnabled(false);emit readChannelFinished();}
 if(result<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;
 return result;
#else
 Q_UNUSED(data);Q_UNUSED(count);return -1;
#endif
}
qint64 StdioDevice::writeData(const char* data,qint64 count){
#ifdef Q_OS_UNIX
 ssize_t result;do{result=::write(STDOUT_FILENO,data,size_t(count));}while(result<0&&errno==EINTR);
 if(result<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){writer_->setEnabled(true);return 0;}
 return result;
#else
 Q_UNUSED(data);Q_UNUSED(count);return -1;
#endif
}
}
