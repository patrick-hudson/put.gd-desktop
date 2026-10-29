#include "CaptureProcessController.h"
#include "CaptureCursorPolicy.h"
#include "X11NativeSnapshot.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <limits>
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
const QString genericReason=QStringLiteral("The capture helper could not complete source acquisition");
// Resident-side failures name their check; helper failures arrive with theirs.
Error unavailable(const QString& code,const QString& check={},QJsonObject details={}){
 if(!check.isEmpty()){details["stage"]="resident";details["check"]=check;}
 return {code,genericReason,details};
}
Error rejected(const QString& code,const QString& reason,const QString& check,QJsonObject details){details["stage"]="resident_verify";details["check"]=check;return {code,reason,details};}
void logFailure(const Error& error){
 if(error&&error.code!="cancelled")logging::write(logCapture(),QtWarningMsg,"Capture failed",{{"code",error.code},{"reason",error.message},{"details",error.details}});
}
bool privateRegular(const struct stat& s){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0600&&s.st_nlink==1;}
}
CaptureProcessController::Preparation::~Preparation(){
 if(consumed_.load()||!controller_||!generation_)return;
 // PreparedAcquisition copies can die on a journal worker. Marshal cleanup to
 // the controller, and never invoke a user callback from this destructor.
 const auto controller=controller_;const auto generation=generation_;
 QMetaObject::invokeMethod(controller,[controller,generation]{if(controller)controller->abandon(generation);},Qt::QueuedConnection);
}
CaptureProcessController::CaptureProcessController(QString helperPath,QObject* parent):QObject(parent),helperPath_(std::move(helperPath)),deadline_(this),stopDeadline_(this){
 if(helperPath_.isEmpty())helperPath_=QCoreApplication::applicationDirPath()+"/putgd-capture";
 deadline_.setSingleShot(true);stopDeadline_.setSingleShot(true);
 connect(&deadline_,&QTimer::timeout,this,[this]{fail(unavailable(ready_?"capture_timeout":"capture_start_timeout","deadline",{{"ready",ready_},{"begun",begun_},{"result",resultReceived_}}));});
 connect(&stopDeadline_,&QTimer::timeout,this,[this]{if(process_&&process_->state()!=QProcess::NotRunning)process_->kill();});
}
CaptureProcessController::~CaptureProcessController(){closing_=true;deadline_.stop();stopDeadline_.stop();if(control_)control_->stop();if(process_){process_->disconnect(this);process_->kill();process_->waitForFinished(1000);}if(decoder_){decoder_->disconnect(this);decoder_->wait();delete decoder_;}artifact_.cleanup();if(callback_){auto callback=std::move(callback_);callback_={};callback({{},{"cancelled","Capture adapter closed",{}}});}}
bool CaptureProcessController::busy()const{return prepared_||bool(callback_)||process_;}
bool CaptureProcessController::preparedReady()const{return prepared_&&ready_&&!begun_&&!failure_;}
qint64 CaptureProcessController::processId()const{return process_?process_->processId():0;}
QJsonObject CaptureProcessController::diagnostics()const{return {{"processId",processId()},{"active",busy()},{"lastExit",lastExit_},{"build",build()}};}
void CaptureProcessController::start(const QJsonObject& frozen,const ResourceLimits& limits,std::function<void(PayloadResult)> callback,QString outcome,bool screenshotFirst){auto ticket=prepare(frozen,limits,{},{},std::move(outcome),screenshotFirst);begin(ticket,std::move(callback));}
CaptureProcessController::Ticket CaptureProcessController::prepare(const QJsonObject& frozen,const ResourceLimits& limits,std::function<void(Error)> earlyTerminal,std::function<void()> earlyReleased,QString outcome,bool screenshotFirst){
 auto ticket=std::make_shared<Preparation>();ticket->controller_=this;
 if(busy()){ticket->failure_=unavailable("busy_selecting");if(earlyReleased)earlyReleased();return ticket;}
 if(auto error=X11NativeSnapshot::validate(frozen);error){ticket->failure_=error;if(earlyReleased)earlyReleased();return ticket;}
 if(limits.imagePixels<1||limits.imageBytes<4||limits.maxFileBytes<1){ticket->failure_=unavailable("input_excessive");if(earlyReleased)earlyReleased();return ticket;}
 // Cursor defaults may depend on root dimensions as well as theme/DPI.
 // Key the bounded resource by all frozen layout metadata, not scale alone.
 const bool cursorCompatible=captureCursorCacheCompatible();
 if(!cursorCompatible)cursorCache_.clear();
 cursorFingerprint_=cursorCompatible&&frozen["action"]=="region"?QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(frozen["layout"].toObject()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()):QString{};requestedCursor_=cursorFingerprint_.isEmpty()?0:cursorCache_.lookup(cursorFingerprint_);
 earlyTerminal_=std::move(earlyTerminal);earlyReleased_=std::move(earlyReleased);region_=frozen["action"]=="region";previewed_=false;previewCursor_=0;
 launchPid_=0;launchStartTicks_.clear();limits_=limits;nonce_=QUuid::createUuid().toString(QUuid::WithoutBraces);++generation_;ticket->generation_=generation_;preparation_=ticket;prepared_=true;beginRequested_=begun_=false;failure_={};result_={};ready_=resultReceived_=accepted_=exited_=cancelling_=false;
 directory_=std::make_unique<QTemporaryDir>("/tmp/putgd-capture-XXXXXX");
 // Recursive QTemporaryDir removal could erase a replacement inode.
 directory_->setAutoRemove(false);
 if(!directory_->isValid()||!QFile::setPermissions(directory_->path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)){fail(unavailable("capture_io","private_directory"));return ticket;}
 path_=directory_->filePath("result.png");const int fd=::open(QFile::encodeName(path_).constData(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);struct stat s{};
 if(fd<0||::fstat(fd,&s)||!privateRegular(s)){if(fd>=0)::close(fd);fail(unavailable("capture_io","private_file"));return ticket;}::close(fd);device_=QString::number(qulonglong(s.st_dev));inode_=QString::number(qulonglong(s.st_ino));
 if(!artifact_.acquire(path_,device_,inode_)){fail(unavailable("capture_io","artifact_lease"));return ticket;}
 auto* child=new QProcess(this);process_=child;child->setProcessChannelMode(QProcess::SeparateChannels);child->setReadChannel(QProcess::StandardOutput);control_=new CaptureControl(*child,child);
 const auto epoch=generation_;
 connect(child,&QProcess::readyReadStandardError,this,[this,child,epoch]{const auto bytes=child->readAllStandardError();if(process_==child&&generation_==epoch)output_.append(bytes);});
 connect(control_,&CaptureControl::frame,this,[this,child,epoch](const QString& type,const QJsonObject& payload){if(process_==child&&generation_==epoch)receive(type,payload);});
 connect(control_,&CaptureControl::failed,this,[this,child,epoch](Error error){if(process_==child&&generation_==epoch)fail(std::move(error));});
 connect(child,&QProcess::started,this,[this,child,epoch,frozen,outcome,screenshotFirst]{if(process_!=child||generation_!=epoch)return;launchPid_=process_->processId();launchStartTicks_=processStartTicks(launchPid_);output_.started(launchPid_,{{"generation",QString::number(generation_)}});QJsonObject bootstrap{{"build",build()},{"nonce",nonce_},{"generation",QString::number(generation_)},{"target",frozen},{"outcome",outcome},{"cursor",QJsonObject{{"id",QString::number(requestedCursor_)},{"cache",!cursorFingerprint_.isEmpty()&&cursorCache_.supported()}}},{"path",path_},{"device",device_},{"inode",inode_},{"limits",QJsonObject{{"imagePixels",limits_.imagePixels},{"imageBytes",limits_.imageBytes},{"maxFileBytes",limits_.maxFileBytes}}}};if(screenshotFirst&&region_)bootstrap["regionMode"]="screenshot_first";control_->send("bootstrap",bootstrap);});
 connect(child,&QProcess::errorOccurred,this,[this,child,epoch](QProcess::ProcessError error){if(process_!=child||generation_!=epoch)return;if(error==QProcess::FailedToStart){output_.failedToStart(child->errorString());if(!failure_)failure_=unavailable("capture_start_failed");exited(-1,QProcess::CrashExit);}});
 connect(child,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,child,epoch](int code,QProcess::ExitStatus status){if(process_==child&&generation_==epoch)exited(code,status);});deadline_.start(10000);child->start(helperPath_,{});return ticket;
}
void CaptureProcessController::begin(const Ticket& ticket,std::function<void(PayloadResult)> callback){
 if(!ticket||ticket->controller_!=this||ticket->consumed_.exchange(true)){callback({{},unavailable("capture_protocol")});return;}
 if(ticket->failure_){callback({{},ticket->failure_});return;}
 if(ticket->generation_!=generation_||!prepared_||beginRequested_){callback({{},unavailable("capture_protocol")});return;}
 callback_=std::move(callback);beginRequested_=true;
 if(failure_){complete();return;}
 if(ready_)sendBegin();
}
void CaptureProcessController::discard(const Ticket& ticket){if(ticket&&ticket->controller_==this&&!ticket->consumed_.load())abandon(ticket->generation_);}
void CaptureProcessController::abandon(quint64 generation){if(!closing_&&prepared_&&!beginRequested_&&generation==generation_)cancel();}
void CaptureProcessController::sendPreview(){
 if(!region_||previewed_||!ready_||failure_||cancelling_)return;
 previewCursor_=cursorFingerprint_.isEmpty()?0:cursorCache_.lookup(cursorFingerprint_);previewed_=true;
 QPointer<CaptureProcessController> guard(this);
 const bool sent=control_&&control_->send("preview",{{"nonce",nonce_},{"generation",QString::number(generation_)},{"cursor",QString::number(previewCursor_)}});
 if(!guard)return;
 if(!sent){fail(unavailable("capture_protocol","preview_send"));return;}
 // Interactive selection is bounded separately from invisible startup.
 deadline_.start(120000);
}
void CaptureProcessController::notifyEarlyTerminal(){
 if(auto ticket=preparation_.lock();ticket&&failure_)ticket->failure_=failure_;
 if(!failure_||beginRequested_||closing_||!earlyTerminal_)return;
 auto notify=std::move(earlyTerminal_);earlyTerminal_={};const auto error=failure_;
 // All controller state is settled before user code runs. The context also
 // drops this metadata-only notification if the adapter is destroyed first.
 QTimer::singleShot(0,this,[notify=std::move(notify),error]{notify(error);});
}
void CaptureProcessController::sendBegin(){
 if(!beginRequested_||begun_||!ready_||failure_||(region_&&!previewed_))return;
 begun_=true;QPointer<CaptureProcessController> guard(this);
 const bool sent=control_&&control_->send("begin",{{"nonce",nonce_},{"generation",QString::number(generation_)},{"cursor",QString::number(region_?previewCursor_:0)}});
 if(!guard)return;
 if(!sent){fail(unavailable("capture_protocol","begin_send"));return;}
 if(!previewed_)deadline_.start(120000);
}
void CaptureProcessController::cancel(){
 if(!busy()||cancelling_)return;
 cancelling_=true;if(!failure_)failure_={"cancelled","Capture acquisition cancelled",{}};if(auto ticket=preparation_.lock())ticket->failure_=failure_;deadline_.stop();
 if(ready_&&control_)control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
 notifyEarlyTerminal();
 if(process_&&process_->state()!=QProcess::NotRunning)stopDeadline_.start(2000);else complete();
}
void CaptureProcessController::fail(Error error){
 if(!busy()||failure_)return;
 logFailure(error);failure_=std::move(error);notifyEarlyTerminal();deadline_.stop();if(control_)control_->stop();if(process_&&process_->state()!=QProcess::NotRunning){process_->kill();stopDeadline_.start(2000);}else complete();
}
void CaptureProcessController::receive(const QString& type,const QJsonObject& payload){
 if(!busy())return;
 if(failure_){
  // A cancellation while QProcess is Starting must follow bootstrap/ready.
  // Sending it before bootstrap would turn a graceful cancel into protocol exit.
  if(cancelling_&&!ready_&&type=="ready"&&payload.size()==4&&payload["cursor"].isObject()&&payload["nonce"]==nonce_&&payload["generation"]==QString::number(generation_)&&payload["build"].toObject()==build()){
   ready_=true;
   if(control_)control_->send("cancel",{{"nonce",nonce_},{"generation",QString::number(generation_)}});
  }
  return;
 }
 if(payload["nonce"]!=nonce_||payload["generation"]!=QString::number(generation_)){fail(unavailable("capture_protocol","envelope"));return;}
 if(type=="ready"){
  if(ready_||payload.size()!=4||payload["build"].toObject()!=build()||!payload["cursor"].isObject()){fail(unavailable("capture_incompatible","ready_frame"));return;}
  const auto seed=payload["cursor"].toObject();
  if(!seed.isEmpty()){
   if(requestedCursor_||cursorFingerprint_.isEmpty()||!cursorCache_.supported()||seed.size()!=5||!seed["pixels"].isString()){fail(unavailable("capture_protocol","cursor_seed"));return;}
   for(const auto& key:{"width","height","xhot","yhot"}){const auto value=seed[key];if(!value.isDouble()||value.toDouble()!=value.toInteger()||value.toInteger()<0||value.toInteger()>64){fail(unavailable("capture_protocol","cursor_seed"));return;}}
   const int width=seed["width"].toInt(),height=seed["height"].toInt(),xhot=seed["xhot"].toInt(),yhot=seed["yhot"].toInt();const auto text=seed["pixels"].toString();
   if(width<1||height<1||xhot>=width||yhot>=height||text.size()>21848){fail(unavailable("capture_protocol","cursor_seed"));return;}
   const auto decoded=QByteArray::fromBase64Encoding(text.toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
   if(!decoded||decoded.decoded.size()!=width*height*4||QString::fromLatin1(decoded.decoded.toBase64())!=text){fail(unavailable("capture_protocol","cursor_seed"));return;}
   // Optional cache failure keeps the exact Qt cursor fallback. No image data
   // survives this frame; the cache owns one server cursor, not a QImage heap.
   cursorCache_.seed(width,height,xhot,yhot,decoded.decoded,cursorFingerprint_);
  }
  ready_=true;QPointer<CaptureProcessController> guard(this);sendPreview();if(guard&&beginRequested_)sendBegin();return;
 }
 if(type=="error"||type=="cancelled"){
  const auto code=payload["code"].toString();const QStringList allowed{"cancelled","display_changed","capture_target","capture_unavailable","capture_geometry","capture_failed","capture_timeout","input_excessive","image_limit","encoding_failed","capture_io"};
  const auto reason=payload["reason"].toString();const auto details=payload["details"].toObject();
  if(!ready_||payload.size()!=5||!allowed.contains(code)||!payload["reason"].isString()||!payload["details"].isObject()||reason.size()>CaptureControl::reasonChars||QJsonDocument(details).toJson(QJsonDocument::Compact).size()>CaptureControl::detailBytes){fail(unavailable("capture_protocol","error_frame"));return;}
  // Keep the helper's own reason and details: they name the failed check and
  // its geometry, which a diagnostics package otherwise cannot recover (#331).
  failure_=code=="cancelled"?Error{"cancelled","Capture acquisition cancelled",{}}:Error{code,reason.isEmpty()?genericReason:reason,details};
  logFailure(failure_);notifyEarlyTerminal();deadline_.stop();stopDeadline_.start(2000);return;
 }
 if(type=="result"){
  if(!ready_||!begun_||resultReceived_||payload.size()!=6||!payload["bytes"].isString()||!payload["sha256"].isString()||!payload["width"].isDouble()||!payload["height"].isDouble()){fail(unavailable("capture_protocol","result_frame"));return;}
  resultReceived_=true;deadline_.start(10000);decode(payload);return;
 }
 fail(unavailable("capture_protocol","frame_type"));
}
void CaptureProcessController::decode(QJsonObject descriptor){
 const auto path=path_,device=device_,inode=inode_;const auto limits=limits_;auto decoded=std::make_shared<PayloadResult>();
 decoder_=QThread::create([decoded,descriptor,path,device,inode,limits]{
  bool numeric=false;const auto expectedBytes=descriptor["bytes"].toString().toLongLong(&numeric);const auto width=descriptor["width"].toInteger(),height=descriptor["height"].toInteger();
  // Each check names itself with the descriptor's geometry, never image bytes.
  auto reject=[&](const QString& code,const QString& reason,const QString& check,QJsonObject details={}){details["width"]=width;details["height"]=height;details["bytes"]=expectedBytes;decoded->error=rejected(code,reason,check,details);};
  if(!numeric||expectedBytes<45||expectedBytes>limits.maxFileBytes||QString::number(expectedBytes)!=descriptor["bytes"].toString()||width<1||height<1||width>INT_MAX||height>INT_MAX||descriptor["width"].toDouble()!=width||descriptor["height"].toDouble()!=height||width>limits.imagePixels/height||width>limits.imageBytes/4/height||descriptor["sha256"].toString().size()!=64){reject("input_excessive","The capture helper described an image outside the accepted limits","descriptor_limits");return;}
  const qint64 raw=width*height*4;if(expectedBytes>raw+raw/1000+height+65536){reject("input_excessive","The capture helper's PNG is larger than its pixels allow","descriptor_ratio");return;}
  const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat s{};if(fd<0||::fstat(fd,&s)||!privateRegular(s)||s.st_size!=expectedBytes||QString::number(qulonglong(s.st_dev))!=device||QString::number(qulonglong(s.st_ino))!=inode){const QJsonObject found{{"fileBytes",fd>=0?qint64(s.st_size):qint64(-1)}};if(fd>=0)::close(fd);reject("capture_io","The capture artifact is missing, replaced or a different size","artifact_identity",found);return;}
  QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);reject("capture_io","Cannot open the capture artifact","artifact_open");return;}
  QCryptographicHash hash(QCryptographicHash::Sha256);if(!hash.addData(&file)||QString::fromLatin1(hash.result().toHex())!=descriptor["sha256"].toString()){reject("capture_failed","The capture artifact does not match the helper's SHA-256","sha256");return;}file.seek(0);const auto header=file.read(33);file.seek(expectedBytes-12);const auto ending=file.read(12);
  if(header.size()!=33||header.left(8)!=QByteArray::fromHex("89504e470d0a1a0a")||header.mid(8,8)!=QByteArray::fromHex("0000000d49484452")){reject("capture_failed","The capture artifact has no PNG header","png_signature");return;}
  if(ending!=QByteArray::fromHex("0000000049454e44ae426082")){reject("capture_failed","The capture artifact's PNG has no IEND trailer","png_trailer");return;}
  if(quint8(header[24])!=8){reject("capture_failed","The capture artifact's PNG sample depth is not 8","png_depth",{{"depth",int(quint8(header[24]))}});return;}
  // Qt 6.4.2 and 6.8.2 instantiate every installed image plugin for a named
  // format unless detection is off; the PNG signature was checked above.
  file.seek(0);QImageReader reader(&file,"PNG");reader.setAutoDetectImageFormat(false);
  if(const auto size=reader.size();size!=QSize(int(width),int(height))){reject("capture_geometry","The capture artifact's PNG size disagrees with its descriptor","reader_size",{{"pngWidth",size.width()},{"pngHeight",size.height()}});return;}
  const auto format=reader.imageFormat();
  if(format!=QImage::Format_RGB32&&format!=QImage::Format_ARGB32){reject("capture_failed","The capture artifact's PNG is not 8-bit RGB or RGBA","reader_format",{{"format",int(format)}});return;}
  // This exact trusted capture was bounded before allocation. Supplying a
  // matching buffer preserves Qt's global reader guard for all other callers.
  QImage image(int(width),int(height),format);
  if(image.isNull()||image.sizeInBytes()>limits.imageBytes){reject("input_excessive","Cannot allocate the capture image within its limits","allocation");return;}
  if(!reader.read(&image)||image.size()!=QSize(int(width),int(height))||image.format()!=format||image.sizeInBytes()>limits.imageBytes){reject("capture_failed","Cannot decode the capture artifact's PNG","reader_decode",{{"reader",int(reader.error())}});return;}
  decoded->payload.image=std::move(image);decoded->payload.image.setDevicePixelRatio(1);decoded->payload.kind="image";decoded->payload.mime="image/png";
 });
 auto* worker=decoder_;connect(worker,&QThread::finished,this,[this,worker,decoded]{decoder_=nullptr;worker->deleteLater();if(failure_){complete();return;}if(decoded->error){fail(decoded->error);return;}result_=std::move(*decoded);if(!control_||!control_->send("accept",{{"nonce",nonce_},{"generation",QString::number(generation_)}})){fail(unavailable("capture_protocol","accept_send"));return;}accepted_=true;deadline_.stop();stopDeadline_.start(2000);if(exited_)complete();});worker->start();
}
void CaptureProcessController::exited(int code,QProcess::ExitStatus status){
 if(!busy()||exited_)return;
 exited_=true;deadline_.stop();stopDeadline_.stop();lastExit_={{"generation",QString::number(generation_)},{"processId",launchPid_},{"startTicks",launchStartTicks_},{"code",code},{"status",status==QProcess::NormalExit?"normal":"crash"}};
 if(output_.pid()>0){if(process_)output_.append(process_->readAllStandardError());output_.finished(code,status,cancelling_);}
 if(control_)control_->stop();
 control_=nullptr;
 if(!failure_&&(!accepted_||code!=0||status!=QProcess::NormalExit)){failure_=unavailable("capture_crashed","helper_exit",{{"exitCode",code},{"status",status==QProcess::NormalExit?"normal":"crash"},{"accepted",accepted_}});logFailure(failure_);}
 complete();
}
void CaptureProcessController::complete(){
 if(decoder_||(process_&&process_->state()!=QProcess::NotRunning))return;
 notifyEarlyTerminal();
 if(process_){process_->disconnect(this);
  // A completion callback may delete this controller inside QProcess signals.
  // Detach the sender so QObject destruction cannot delete its active stack.
  process_->setParent(nullptr);process_->deleteLater();process_=nullptr;}control_=nullptr;deadline_.stop();stopDeadline_.stop();artifact_.cleanup();directory_.reset();path_.clear();device_.clear();inode_.clear();nonce_.clear();
 preparation_.reset();prepared_=false;beginRequested_=begun_=false;earlyTerminal_={};region_=previewed_=false;previewCursor_=0;
 auto released=std::move(earlyReleased_);earlyReleased_={};
 auto callback=std::move(callback_);callback_={};auto result=failure_?PayloadResult{{},failure_}:std::move(result_);result_={};
 if(closing_)return;
 if(callback){
  // BEGIN owns terminal delivery, so no early notification is pending. Both
  // callbacks are detached locals; release first, and never access this again:
  // either callback can destroy the fully drained controller.
  if(released)released();
  callback(std::move(result));
 }else if(released){
  // Pre-BEGIN metadata was queued first. Deliver drain on the same context
  // afterward, without capturing mutable generation state or a user payload.
  QTimer::singleShot(0,this,[released=std::move(released)]{released();});
 }
}
}
