#include "AsyncFdReader.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
namespace putgd {
AsyncFdReader::~AsyncFdReader(){if(fd_>=0)close(fd_);}
void AsyncFdReader::start(int fd,qint64 bound,int timeoutMs,Reply reply){
 cancel();fd_=fd;bound_=bound;reply_=std::move(reply);bytes_.clear();
 if(fd_<0||bound<0){finish({"transfer_fd","Invalid native transfer descriptor or bound",{}});return;}
 const int flags=fcntl(fd_,F_GETFL);if(flags<0||fcntl(fd_,F_SETFL,flags|O_NONBLOCK)<0){finish({"transfer_fd","Cannot configure nonblocking native transfer",{}});return;}
 notifier_=new QSocketNotifier(fd_,QSocketNotifier::Read,this);connect(notifier_,&QSocketNotifier::activated,this,[this]{drain();});
 timer_.setSingleShot(true);timer_.disconnect(this);connect(&timer_,&QTimer::timeout,this,[this]{finish({"transfer_timeout","Native transfer timed out; copy the source again and retry",{}});});timer_.start(timeoutMs);
 QTimer::singleShot(0,this,[this]{drain();});
}
void AsyncFdReader::setBound(qint64 bound){bound_=bound;if(bytes_.size()>bound_)finish({"input_excessive","Native transfer exceeds the expected byte bound",{}});}
void AsyncFdReader::cancel(){if(reply_)finish({"cancelled","Native transfer cancelled",{}});}
void AsyncFdReader::drain(){
 if(fd_<0)return;
 char buffer[65536];
 // Yield after a bounded chunk so a fast large transfer cannot monopolize Qt.
 for(int i=0;i<16;++i){const auto n=read(fd_,buffer,sizeof buffer);if(n>0){if(n>bound_-bytes_.size()){finish({"input_excessive","Native transfer exceeds resource limits",{}});return;}bytes_.append(buffer,n);}
 else if(n==0){finish();return;}else if(errno==EINTR){--i;continue;}else if(errno==EAGAIN||errno==EWOULDBLOCK)return;else{finish({"transfer_fd","Native transfer descriptor failed",{}});return;}}
}
void AsyncFdReader::finish(Error error){
 timer_.stop();if(notifier_){notifier_->setEnabled(false);notifier_->deleteLater();notifier_=nullptr;}if(fd_>=0){close(fd_);fd_=-1;}
 auto reply=std::move(reply_);reply_={};auto bytes=error?QByteArray{}:std::move(bytes_);bytes_.clear();if(reply)reply(std::move(bytes),std::move(error));
}
}
