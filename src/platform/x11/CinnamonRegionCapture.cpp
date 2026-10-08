#include "CinnamonRegionCapture.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QImageReader>
#include <QSocketNotifier>
#include <QThread>
#include <sys/inotify.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <memory>
namespace putgd {
CinnamonRegionCapture::CinnamonRegionCapture(QList<CaptureFrame> frames,ResourceLimits limits,QObject* parent)
 :QObject(parent),frames_(std::move(frames)),limits_(std::move(limits)),deadline_(this),remaining_(frames_.size()) {
 deadline_.setSingleShot(true);
 connect(&deadline_,&QTimer::timeout,this,[this]{finish({"capture_timeout","Cinnamon did not finish the desktop capture",{}});});
}
CinnamonRegionCapture::~CinnamonRegionCapture(){
 cleanFiles();
 for(auto* worker:workers_){worker->wait();delete worker;}
}
void CinnamonRegionCapture::cleanFiles(){
 if(notifier_)notifier_->setEnabled(false);
 deadline_.stop();
 if(fd_>=0){::close(fd_);fd_=-1;}
 directory_.remove();
}
void CinnamonRegionCapture::finish(Error error){
 if(!active_)return;
 active_=false;cleanFiles();
 if(error)frames_.clear();
 emit acquired(std::move(frames_),error);
 // Cancellation acknowledges promptly. Decoders already holding an open file
 // can drain before QObject destruction; their results are discarded.
 if(workers_.isEmpty())deleteLater();
}
void CinnamonRegionCapture::cancel(){finish({"cancelled","Capture acquisition cancelled",{}});}
void CinnamonRegionCapture::start(){
 qint64 pixels=0,bytes=0;
 for(const auto& frame:frames_){
  const auto count=qint64(frame.nativeRect.width())*frame.nativeRect.height();
  if(count<=0||count>limits_.imagePixels-pixels||count>(limits_.imageBytes-bytes)/4){finish({"input_excessive","Desktop frame snapshots exceed resource limits",{}});return;}
  pixels+=count;bytes+=count*4;
 }
 if(frames_.isEmpty()){finish({"capture_geometry","No enabled monitor is available",{}});return;}
 if(!directory_.isValid()){finish({"capture_failed","Cannot create a private Cinnamon capture directory",{}});return;}
 // Create 0600 outputs before watching their 0700 directory. Only the native
 // writer's final close-write event starts decoding, even for a slow PNG write.
 for(int index=0;index<frames_.size();++index){
  QFile file(directory_.filePath(QString::number(index)+".png"));
  if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)){finish({"capture_failed","Cannot create a private Cinnamon capture file",{}});return;}
 }
 fd_=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
 if(fd_<0||inotify_add_watch(fd_,QFile::encodeName(directory_.path()).constData(),IN_CLOSE_WRITE)<0){finish({"capture_failed","Cannot watch Cinnamon capture completion",{}});return;}
 notifier_=new QSocketNotifier(fd_,QSocketNotifier::Read,this);
 connect(notifier_,&QSocketNotifier::activated,this,[this](QSocketDescriptor,QSocketNotifier::Type){completedFiles();});
 deadline_.start(10000);
 for(int index=0;index<frames_.size();++index){
  const auto name=QString::number(index)+".png";pending_.insert(name);const auto rect=frames_[index].nativeRect;
  auto message=QDBusMessage::createMethodCall("org.Cinnamon","/org/Cinnamon","org.Cinnamon","ScreenshotArea");
  message.setArguments({false,rect.x(),rect.y(),rect.width(),rect.height(),false,directory_.filePath(name)});
  auto* call=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,10000),this);
  connect(call,&QDBusPendingCallWatcher::finished,this,[this,call]{
   QDBusPendingReply<> reply=*call;call->deleteLater();
   if(active_&&reply.isError())finish({"capture_unavailable","Cinnamon native screenshot service is unavailable",{}});
  });
 }
}
void CinnamonRegionCapture::completedFiles(){
 alignas(inotify_event) std::array<char,4096> events{};
 while(active_){
  const auto count=::read(fd_,events.data(),events.size());
  if(count<0){if(errno==EAGAIN)return;finish({"capture_failed","Cannot read Cinnamon capture completion",{}});return;}
  if(count==0)return;
  for(ssize_t offset=0;offset<count;){
   const auto* event=reinterpret_cast<const inotify_event*>(events.data()+offset);offset+=sizeof(inotify_event)+event->len;
   if(event->mask&IN_Q_OVERFLOW){finish({"capture_failed","Cinnamon capture completion events overflowed",{}});return;}
   const auto name=QString::fromLocal8Bit(event->name);
   if(event->mask&IN_CLOSE_WRITE&&pending_.remove(name))decode(name.chopped(4).toInt(),directory_.filePath(name));
  }
 }
}
void CinnamonRegionCapture::decode(int index,const QString& path){
 const auto expected=frames_[index].nativeRect.size();
 struct Decoded {QImage image;Error error;};auto decoded=std::make_shared<Decoded>();
 auto* worker=QThread::create([decoded,path,expected]{
  QFile file(path);const qint64 raw=qint64(expected.width())*expected.height()*4;
  if(!file.open(QIODevice::ReadOnly)||file.size()<45||file.size()>raw+raw/1000+expected.height()+65536){decoded->error={"capture_failed","Cinnamon returned an invalid or excessive PNG",{}};return;}
  const auto header=file.read(33);file.seek(file.size()-12);const auto ending=file.read(12);
  if(header.left(8)!=QByteArray::fromHex("89504e470d0a1a0a")||header.mid(8,8)!=QByteArray::fromHex("0000000d49484452")||ending!=QByteArray::fromHex("0000000049454e44ae426082")){decoded->error={"capture_failed","Cinnamon returned an incomplete PNG",{}};return;}
  // Cairo's RGB/ARGB32 path writes depth 8. Reject wider samples before the
  // decoder can allocate more than the accepted four-byte pixel budget.
  if(static_cast<unsigned char>(header[24])!=8){decoded->error={"capture_failed","Cinnamon returned an unsupported PNG sample depth",{}};return;}
  file.seek(0);QImageReader reader(&file,"PNG");
  if(reader.size()!=expected){decoded->error={"capture_geometry","Cinnamon frame dimensions disagree with frozen monitor geometry",{}};return;}
  decoded->image=reader.read();decoded->image.setDevicePixelRatio(1);
  if(decoded->image.isNull()){decoded->error={"capture_failed","Cannot decode Cinnamon desktop capture",{}};return;}
  if(decoded->image.sizeInBytes()>raw){decoded->error={"input_excessive","Cinnamon frame exceeds its accepted image budget",{}};decoded->image={};}
 });
 workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,decoded,index]{
  workers_.removeOne(worker);worker->deleteLater();
  if(!active_){if(workers_.isEmpty())deleteLater();return;}
  if(decoded->error){finish(decoded->error);return;}
  frames_[index].pixels=std::move(decoded->image);
  if(--remaining_==0)finish({});
 });
 worker->start();
}
}
