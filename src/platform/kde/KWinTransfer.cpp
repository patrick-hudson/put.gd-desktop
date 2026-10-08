#include "KWinTransfer.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cmath>
#include <limits>
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
KWinImageLayout validateKWinMetadata(const QVariantMap& m,const ResourceLimits& limits,Error& error){
 error={};KWinImageLayout out;auto bad=[&]{error={"capture_metadata","KWin returned invalid raw image metadata",{}};return KWinImageLayout{};};
 if(m.value("type").metaType().id()!=QMetaType::QString||m.value("type").toString()!="raw")return bad();
 for(const char* key:{"width","height","stride","format"})if(m.value(key).metaType().id()!=QMetaType::UInt)return bad();
 const auto width=m.value("width").toUInt(),height=m.value("height").toUInt(),stride=m.value("stride").toUInt(),format=m.value("format").toUInt();
 if(!width||!height)return bad();
 if(limits.imagePixels<1||width>quint64(limits.imagePixels)/height){error={"input_excessive","KWin image dimensions exceed resource limits",{}};return {};}
 if(width>INT_MAX||height>INT_MAX||stride>INT_MAX||format<=QImage::Format_Indexed8||format>=QImage::NImageFormats)return bad();
 QImage probe(1,1,QImage::Format(format));if(probe.isNull()||probe.depth()<8)return bad();
 const auto row=(quint64(width)*probe.depth()+7)/8;
 const int alignment=probe.depth()>=64?int(alignof(quint64)):4;
 if(stride<row||stride%alignment||!stride)return bad();
 if(limits.imageBytes<1||stride>quint64(limits.imageBytes)/height){error={"input_excessive","KWin raw image exceeds resource limits",{}};return {};}
 const auto scale=m.value("scale");if(scale.metaType().id()!=QMetaType::Double||!std::isfinite(scale.toDouble())||scale.toDouble()<=0)return bad();
 out={int(width),int(height),int(stride),QImage::Format(format),scale.toDouble(),qint64(stride)*height};return out;
}
QImage decodeKWinImage(const QVariantMap& metadata,const QByteArray& bytes,const ResourceLimits& limits,Error& error){
 const auto layout=validateKWinMetadata(metadata,limits,error);if(error)return {};
 if(bytes.size()!=layout.bytes){error={"capture_transfer","KWin image transfer is truncated or contains surplus bytes",{}};return {};}
 const QImage view(reinterpret_cast<const uchar*>(bytes.constData()),layout.width,layout.height,layout.stride,layout.format);
 auto image=view.copy();if(image.isNull()){error={"capture_transfer","Cannot allocate the KWin image",{}};return {};}
 image.setDevicePixelRatio(layout.scale);return image;
}
}
