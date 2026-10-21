#include "CinnamonRegionCapture.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QSocketNotifier>
#include <QThread>
#include <sys/inotify.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <memory>
namespace putgd {
namespace {
int watchErrorForTesting=0;
const QByteArray pngTrailer=QByteArray::fromHex("0000000049454e44ae426082");
QJsonArray rectangle(const QRect& r){return {r.x(),r.y(),r.width(),r.height()};}
QString errnoName(int error){
 switch(error){
  case EMFILE:return "EMFILE";case ENFILE:return "ENFILE";case ENOSPC:return "ENOSPC";case ENOMEM:return "ENOMEM";
  case EACCES:return "EACCES";case ENOENT:return "ENOENT";case EINVAL:return "EINVAL";default:return QString::number(error);
 }
}
}
void CinnamonRegionCapture::setWatchErrorForTesting(int error){watchErrorForTesting=error;}
CinnamonRegionCapture::CinnamonRegionCapture(QList<CaptureFrame> frames,ResourceLimits limits,QObject* parent)
 :QObject(parent),frames_(std::move(frames)),limits_(std::move(limits)),deadline_(this),poll_(this),remaining_(frames_.size()) {
 deadline_.setSingleShot(true);
 connect(&deadline_,&QTimer::timeout,this,[this]{
  QJsonArray pending;for(const auto& name:std::as_const(pending_))pending.append(name.chopped(4).toInt());
  finish(failure("capture_timeout","Cinnamon did not finish the desktop capture","deadline",{{"pendingFrames",pending}}));
 });
 poll_.setInterval(20);
 connect(&poll_,&QTimer::timeout,this,&CinnamonRegionCapture::pollFiles);
}
CinnamonRegionCapture::~CinnamonRegionCapture(){
 cleanFiles();
 for(auto* worker:workers_){worker->wait();delete worker;}
}
Error CinnamonRegionCapture::failure(const QString& code,const QString& message,const QString& check,QJsonObject details)const{
 // Geometry and check names only: no paths, pixels or service-provided text.
 details["stage"]="cinnamon";details["check"]=check;if(!watch_.isEmpty())details["completion"]=watch_;
 return {code,message,details};
}
void CinnamonRegionCapture::cleanFiles(){
 if(notifier_)notifier_->setEnabled(false);
 deadline_.stop();poll_.stop();
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
  if(count<=0||count>limits_.imagePixels-pixels||count>(limits_.imageBytes-bytes)/4){finish(failure("input_excessive","Desktop frame snapshots exceed resource limits","frame_limits"));return;}
  pixels+=count;bytes+=count*4;
 }
 if(frames_.isEmpty()){finish(failure("capture_geometry","No enabled monitor is available","no_monitors"));return;}
 if(!directory_.isValid()){finish(failure("capture_failed","Cannot create a private Cinnamon capture directory","private_directory"));return;}
 // Create 0600 outputs before watching their 0700 directory. Only the native
 // writer's final close-write event starts decoding, even for a slow PNG write;
 // without a watch, only a complete PNG does.
 for(int index=0;index<frames_.size();++index){
  QFile file(directory_.filePath(QString::number(index)+".png"));
  if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)){finish(failure("capture_failed","Cannot create a private Cinnamon capture file","private_file",{{"frame",index}}));return;}
 }
 int watchError=watchErrorForTesting;
 if(!watchError){fd_=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);if(fd_<0)watchError=errno;}
 if(fd_>=0&&inotify_add_watch(fd_,QFile::encodeName(directory_.path()).constData(),IN_CLOSE_WRITE)<0){watchError=errno;::close(fd_);fd_=-1;}
 if(fd_>=0){
  watch_="inotify";
  notifier_=new QSocketNotifier(fd_,QSocketNotifier::Read,this);
  connect(notifier_,&QSocketNotifier::activated,this,[this](QSocketDescriptor,QSocketNotifier::Type){completedFiles();});
 }else{
  // inotify instances are a per-user limit (128 by default) that every other
  // program shares; a desktop full of editors and agents exhausts it (#331).
  // Cinnamon replies before it paints, so the D-Bus reply cannot mark a
  // finished file either: poll each output until it ends in a stable IEND.
  watch_="poll:"+errnoName(watchError);
  qWarning("Cinnamon capture completion watch unavailable (%s); polling for finished PNGs",qPrintable(errnoName(watchError)));
  poll_.start();
 }
 deadline_.start(10000);
 for(int index=0;index<frames_.size();++index){
  const auto name=QString::number(index)+".png";pending_.insert(name);const auto rect=frames_[index].nativeRect;
  auto message=QDBusMessage::createMethodCall("org.Cinnamon","/org/Cinnamon","org.Cinnamon","ScreenshotArea");
  message.setArguments({false,rect.x(),rect.y(),rect.width(),rect.height(),false,directory_.filePath(name)});
  auto* call=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,10000),this);
  connect(call,&QDBusPendingCallWatcher::finished,this,[this,call,index]{
   QDBusPendingReply<> reply=*call;call->deleteLater();
   // The error type is Qt's own name; a service's error text stays private.
   if(active_&&reply.isError())finish(failure("capture_unavailable","Cinnamon native screenshot service is unavailable","service_call",{{"frame",index},{"output",frames_[index].identity},{"native",rectangle(frames_[index].nativeRect)},{"dbusError",QDBusError::errorString(reply.error().type())}}));
  });
 }
}
void CinnamonRegionCapture::completedFiles(){
 alignas(inotify_event) std::array<char,4096> events{};
 while(active_){
  const auto count=::read(fd_,events.data(),events.size());
  if(count<0){if(errno==EAGAIN)return;finish(failure("capture_failed","Cannot read Cinnamon capture completion","completion_read",{{"errno",errnoName(errno)}}));return;}
  if(count==0)return;
  for(ssize_t offset=0;offset<count;){
   const auto* event=reinterpret_cast<const inotify_event*>(events.data()+offset);offset+=sizeof(inotify_event)+event->len;
   if(event->mask&IN_Q_OVERFLOW){finish(failure("capture_failed","Cinnamon capture completion events overflowed","completion_overflow"));return;}
   const auto name=QString::fromLocal8Bit(event->name);
   if(event->mask&IN_CLOSE_WRITE&&pending_.remove(name))decode(name.chopped(4).toInt(),directory_.filePath(name));
  }
 }
}
void CinnamonRegionCapture::pollFiles(){
 // Cairo writes the IEND trailer last. A file is finished when it ends in that
 // trailer and its size held still across one poll; the decoder rechecks it all.
 const auto names=pending_.values();
 for(const auto& name:names){
  if(!active_)return;
  QFile file(directory_.filePath(name));const auto size=file.size();
  const auto previous=polledSizes_.value(name,-1);polledSizes_[name]=size;
  if(size<45||size!=previous||!file.open(QIODevice::ReadOnly)||!file.seek(size-12)||file.read(12)!=pngTrailer)continue;
  pending_.remove(name);polledSizes_.remove(name);decode(name.chopped(4).toInt(),file.fileName());
 }
 if(pending_.isEmpty())poll_.stop();
}
void CinnamonRegionCapture::decode(int index,const QString& path){
 const auto expected=frames_[index].nativeRect.size();
 struct Decoded {QImage image;QString code,message,check;QJsonObject details;};auto decoded=std::make_shared<Decoded>();
 auto* worker=QThread::create([decoded,path,expected]{
  auto reject=[&](const char* code,const char* message,const char* check,QJsonObject details={}){decoded->code=code;decoded->message=message;decoded->check=check;decoded->details=std::move(details);};
  QFile file(path);const qint64 raw=qint64(expected.width())*expected.height()*4;
  if(!file.open(QIODevice::ReadOnly)){reject("capture_failed","Cannot open the Cinnamon PNG","png_open");return;}
  const QJsonObject sized{{"fileBytes",file.size()},{"maxBytes",raw+raw/1000+expected.height()+65536}};
  if(file.size()<45||file.size()>raw+raw/1000+expected.height()+65536){reject("capture_failed","Cinnamon returned an empty or excessive PNG","png_size",sized);return;}
  const auto header=file.read(33);file.seek(file.size()-12);const auto ending=file.read(12);
  if(header.left(8)!=QByteArray::fromHex("89504e470d0a1a0a")||header.mid(8,8)!=QByteArray::fromHex("0000000d49484452")){reject("capture_failed","Cinnamon returned a file without a PNG header","png_signature",sized);return;}
  if(ending!=pngTrailer){reject("capture_failed","Cinnamon returned an incomplete PNG","png_trailer",sized);return;}
  // Cairo's RGB/ARGB32 path writes depth 8. Reject wider samples before the
  // decoder can allocate more than the accepted four-byte pixel budget.
  if(static_cast<unsigned char>(header[24])!=8){reject("capture_failed","Cinnamon returned an unsupported PNG sample depth","png_depth",{{"depth",int(static_cast<unsigned char>(header[24]))}});return;}
  // Qt 6.4.2 and 6.8.2 instantiate every installed image plugin for a named
  // format unless detection is off; the PNG signature was checked above.
  file.seek(0);QImageReader reader(&file,"PNG");reader.setAutoDetectImageFormat(false);
  if(const auto size=reader.size();size!=expected){reject("capture_geometry","Cinnamon frame dimensions disagree with frozen monitor geometry","png_dimensions",{{"width",size.width()},{"height",size.height()}});return;}
  decoded->image=reader.read();decoded->image.setDevicePixelRatio(1);
  if(decoded->image.isNull()){reject("capture_failed","Cannot decode Cinnamon desktop capture","png_decode",{{"reader",QString::number(int(reader.error()))}});return;}
  if(decoded->image.sizeInBytes()>raw){reject("input_excessive","Cinnamon frame exceeds its accepted image budget","frame_budget",{{"imageBytes",decoded->image.sizeInBytes()}});decoded->image={};}
 });
 workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,decoded,index]{
  workers_.removeOne(worker);worker->deleteLater();
  if(!active_){if(workers_.isEmpty())deleteLater();return;}
  if(!decoded->code.isEmpty()){
   auto details=decoded->details;details["frame"]=index;details["output"]=frames_[index].identity;details["native"]=rectangle(frames_[index].nativeRect);
   finish(failure(decoded->code,decoded->message,decoded->check,details));return;
  }
  frames_[index].pixels=std::move(decoded->image);
  if(--remaining_==0)finish({});
 });
 worker->start();
}
}
