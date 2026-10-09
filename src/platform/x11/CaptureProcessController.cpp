#include "CaptureProcessController.h"
#include "X11NativeSnapshot.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QImageReader>
#include <QCryptographicHash>
#include <QFile>
#include <QUuid>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace putgd {
namespace {
QJsonObject build(){return QJsonDocument::fromJson(buildManifestJson()).object();}
QString processStartTicks(qint64 pid){
 QFile file(QString("/proc/%1/stat").arg(pid));
 if(pid<=0||!file.open(QIODevice::ReadOnly))return {};
 const auto bytes=file.read(4097);if(bytes.size()>4096)return {};
 const auto end=bytes.lastIndexOf(')');if(end<0)return {};
 const auto fields=bytes.mid(end+2).simplified().split(' ');
 if(fields.size()<20)return {};
 bool ok=false;const auto ticks=fields[19].toULongLong(&ok);
 return ok&&ticks>0?QString::number(ticks):QString{};
}
Error unavailable(const QString& code){return {code,"The capture helper could not complete source acquisition",{}};}
bool privateRegular(const struct stat& s){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0600&&s.st_nlink==1;}
}
CaptureProcessController::CaptureProcessController(QString helperPath,QObject* parent):QObject(parent),helperPath_(std::move(helperPath)),deadline_(this),stopDeadline_(this){
 if(helperPath_.isEmpty())helperPath_=QCoreApplication::applicationDirPath()+"/putgd-capture";
 deadline_.setSingleShot(true);stopDeadline_.setSingleShot(true);
 connect(&deadline_,&QTimer::timeout,this,[this]{fail(unavailable(ready_?"capture_timeout":"capture_start_timeout"));});
 connect(&stopDeadline_,&QTimer::timeout,this,[this]{if(process_&&process_->state()!=QProcess::NotRunning)process_->kill();});
}
CaptureProcessController::~CaptureProcessController(){closing_=true;deadline_.stop();stopDeadline_.stop();if(control_)control_->stop();if(process_){process_->disconnect(this);process_->kill();process_->waitForFinished(1000);}if(decoder_){decoder_->disconnect(this);decoder_->wait();delete decoder_;}artifact_.cleanup();if(callback_){auto callback=std::move(callback_);callback_={};callback({{},{"cancelled","Capture adapter closed",{}}});}}
bool CaptureProcessController::busy()const{return bool(callback_);}
qint64 CaptureProcessController::processId()const{return process_?process_->processId():0;}
QJsonObject CaptureProcessController::diagnostics()const{return {{"processId",processId()},{"active",busy()},{"lastExit",lastExit_},{"build",build()}};}
void CaptureProcessController::start(const QJsonObject& frozen,const ResourceLimits& limits,std::function<void(PayloadResult)> callback){
 if(busy()){callback({{},unavailable("busy_selecting")});return;}
 if(auto error=X11NativeSnapshot::validate(frozen);error){callback({{},error});return;}
 if(limits.imagePixels<1||limits.imageBytes<4||limits.maxFileBytes<1){callback({{},unavailable("input_excessive")});return;}
 launchPid_=0;launchStartTicks_.clear();callback_=std::move(callback);limits_=limits;nonce_=QUuid::createUuid().toString(QUuid::WithoutBraces);++generation_;failure_={};result_={};ready_=resultReceived_=accepted_=exited_=cancelling_=false;
 directory_=std::make_unique<QTemporaryDir>("/tmp/putgd-capture-XXXXXX");
 // Recursive QTemporaryDir removal could erase a replacement inode.
 directory_->setAutoRemove(false);
 if(!directory_->isValid()||!QFile::setPermissions(directory_->path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)){fail(unavailable("capture_io"));return;}
 path_=directory_->filePath("result.png");const int fd=::open(QFile::encodeName(path_).constData(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);struct stat s{};
 if(fd<0||::fstat(fd,&s)||!privateRegular(s)){if(fd>=0)::close(fd);fail(unavailable("capture_io"));return;}::close(fd);device_=QString::number(qulonglong(s.st_dev));inode_=QString::number(qulonglong(s.st_ino));
 if(!artifact_.acquire(path_,device_,inode_)){fail(unavailable("capture_io"));return;}
 auto* child=new QProcess(this);process_=child;child->setProcessChannelMode(QProcess::SeparateChannels);child->setReadChannel(QProcess::StandardOutput);control_=new CaptureControl(*child,child);
 connect(child,&QProcess::readyReadStandardError,this,[child]{while(!child->readAllStandardError().isEmpty()){};});
 connect(control_,&CaptureControl::frame,this,&CaptureProcessController::receive);connect(control_,&CaptureControl::failed,this,&CaptureProcessController::fail);
 connect(child,&QProcess::started,this,[this,frozen]{launchPid_=process_->processId();launchStartTicks_=processStartTicks(launchPid_);control_->send("bootstrap",{{"build",build()},{"nonce",nonce_},{"generation",QString::number(generation_)},{"target",frozen},{"path",path_},{"device",device_},{"inode",inode_},{"limits",QJsonObject{{"imagePixels",limits_.imagePixels},{"imageBytes",limits_.imageBytes},{"maxFileBytes",limits_.maxFileBytes}}}});});
 connect(child,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart){failure_=unavailable("capture_start_failed");exited(-1,QProcess::CrashExit);}});
 connect(child,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,&CaptureProcessController::exited);deadline_.start(10000);child->start(helperPath_,{});
}
void CaptureProcessController::cancel(){
 if(!busy()||cancelling_)return;
 cancelling_=true;failure_={"cancelled","Capture acquisition cancelled",{}};deadline_.stop();
 if(ready_&&control_)control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
 if(process_&&process_->state()!=QProcess::NotRunning)stopDeadline_.start(2000);else complete();
}
void CaptureProcessController::fail(Error error){
 if(!busy()||failure_)return;
 failure_=std::move(error);deadline_.stop();if(control_)control_->stop();if(process_&&process_->state()!=QProcess::NotRunning){process_->kill();stopDeadline_.start(2000);}else complete();
}
void CaptureProcessController::receive(const QString& type,const QJsonObject& payload){
 if(!busy())return;
 if(failure_){
  // A cancellation while QProcess is Starting must follow bootstrap/ready.
  // Sending it before bootstrap would turn a graceful cancel into protocol exit.
  if(cancelling_&&!ready_&&type=="ready"&&payload.size()==3&&payload["nonce"]==nonce_&&payload["generation"]==QString::number(generation_)&&payload["build"].toObject()==build()){
   ready_=true;
   if(control_)control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
  }
  return;
 }
 if(payload["nonce"]!=nonce_||payload["generation"]!=QString::number(generation_)){fail(unavailable("capture_protocol"));return;}
 if(type=="ready"){
  if(ready_||payload.size()!=3||payload["build"].toObject()!=build()){fail(unavailable("capture_incompatible"));return;}ready_=true;deadline_.start(120000);return;
 }
 if(type=="error"||type=="cancelled"){
  const auto code=payload["code"].toString();const QStringList allowed{"cancelled","display_changed","capture_target","capture_unavailable","capture_geometry","capture_failed","capture_timeout","input_excessive","image_limit","encoding_failed","capture_io"};
  if(!ready_||payload.size()!=3||!allowed.contains(code)){fail(unavailable("capture_protocol"));return;}
  failure_=code=="cancelled"?Error{"cancelled","Capture acquisition cancelled",{}}:unavailable(code);deadline_.stop();stopDeadline_.start(2000);return;
 }
 if(type=="result"){
  if(!ready_||resultReceived_||payload.size()!=6||!payload["bytes"].isString()||!payload["sha256"].isString()||!payload["width"].isDouble()||!payload["height"].isDouble()){fail(unavailable("capture_protocol"));return;}
  resultReceived_=true;deadline_.start(10000);decode(payload);return;
 }
 fail(unavailable("capture_protocol"));
}
void CaptureProcessController::decode(QJsonObject descriptor){
 const auto path=path_,device=device_,inode=inode_;const auto limits=limits_;auto decoded=std::make_shared<PayloadResult>();
 decoder_=QThread::create([decoded,descriptor,path,device,inode,limits]{
  auto reject=[&](const QString& code){decoded->error=unavailable(code);};bool numeric=false;const auto expectedBytes=descriptor["bytes"].toString().toLongLong(&numeric);const auto width=descriptor["width"].toInteger(),height=descriptor["height"].toInteger();
  if(!numeric||expectedBytes<45||expectedBytes>limits.maxFileBytes||QString::number(expectedBytes)!=descriptor["bytes"].toString()||width<1||height<1||width>INT_MAX||height>INT_MAX||descriptor["width"].toDouble()!=width||descriptor["height"].toDouble()!=height||width>limits.imagePixels/height||width>limits.imageBytes/4/height||descriptor["sha256"].toString().size()!=64){reject("input_excessive");return;}
  const qint64 raw=width*height*4;if(expectedBytes>raw+raw/1000+height+65536){reject("input_excessive");return;}
  const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat s{};if(fd<0||::fstat(fd,&s)||!privateRegular(s)||s.st_size!=expectedBytes||QString::number(qulonglong(s.st_dev))!=device||QString::number(qulonglong(s.st_ino))!=inode){if(fd>=0)::close(fd);reject("capture_io");return;}
  QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);reject("capture_io");return;}
  QCryptographicHash hash(QCryptographicHash::Sha256);if(!hash.addData(&file)||QString::fromLatin1(hash.result().toHex())!=descriptor["sha256"].toString()){reject("capture_failed");return;}file.seek(0);const auto header=file.read(33);file.seek(expectedBytes-12);const auto ending=file.read(12);
  if(header.left(8)!=QByteArray::fromHex("89504e470d0a1a0a")||header.mid(8,8)!=QByteArray::fromHex("0000000d49484452")||ending!=QByteArray::fromHex("0000000049454e44ae426082")||header.size()!=33||quint8(header[24])!=8){reject("capture_failed");return;}
  file.seek(0);QImageReader reader(&file,"PNG");
  if(reader.size()!=QSize(int(width),int(height))){reject("capture_geometry");return;}
  const auto format=reader.imageFormat();
  if(format!=QImage::Format_RGB32&&format!=QImage::Format_ARGB32){reject("capture_failed");return;}
  // This exact trusted capture was bounded before allocation. Supplying a
  // matching buffer preserves Qt's global reader guard for all other callers.
  QImage image(int(width),int(height),format);
  if(image.isNull()||image.sizeInBytes()>limits.imageBytes){reject("input_excessive");return;}
  if(!reader.read(&image)||image.size()!=QSize(int(width),int(height))||image.format()!=format||image.sizeInBytes()>limits.imageBytes){reject("capture_failed");return;}
  decoded->payload.image=std::move(image);decoded->payload.image.setDevicePixelRatio(1);decoded->payload.kind="image";decoded->payload.mime="image/png";
 });
 auto* worker=decoder_;connect(worker,&QThread::finished,this,[this,worker,decoded]{decoder_=nullptr;worker->deleteLater();if(failure_){complete();return;}if(decoded->error){fail(decoded->error);return;}result_=std::move(*decoded);if(!control_||!control_->send("accept",{{"nonce",nonce_},{"generation",QString::number(generation_)}})){fail(unavailable("capture_protocol"));return;}accepted_=true;deadline_.stop();stopDeadline_.start(2000);if(exited_)complete();});worker->start();
}
void CaptureProcessController::exited(int code,QProcess::ExitStatus status){
 if(!busy()||exited_)return;
 exited_=true;deadline_.stop();stopDeadline_.stop();lastExit_={{"generation",QString::number(generation_)},{"processId",launchPid_},{"startTicks",launchStartTicks_},{"code",code},{"status",status==QProcess::NormalExit?"normal":"crash"}};
 if(control_)control_->stop();
 control_=nullptr;
 if(!failure_&&(!accepted_||code!=0||status!=QProcess::NormalExit))failure_=unavailable("capture_crashed");
 complete();
}
void CaptureProcessController::complete(){
 if(!callback_||decoder_||(process_&&process_->state()!=QProcess::NotRunning))return;
 if(process_){process_->disconnect(this);
  // A completion callback may delete this controller inside QProcess signals.
  // Detach the sender so QObject destruction cannot delete its active stack.
  process_->setParent(nullptr);process_->deleteLater();process_=nullptr;}control_=nullptr;deadline_.stop();stopDeadline_.stop();artifact_.cleanup();directory_.reset();path_.clear();device_.clear();inode_.clear();nonce_.clear();
 auto callback=std::move(callback_);callback_={};auto result=failure_?PayloadResult{{},failure_}:std::move(result_);result_={};if(!closing_)callback(std::move(result));
}
}
