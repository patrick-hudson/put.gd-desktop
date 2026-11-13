#include "EditorJob.h"
#include "Log.h"
#include "BuildInfo.h"
#include "SanitizedPng.h"
#include "TextJobProtocol.h"
#include "Settings.h"
#include "ipc/UiControl.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QScopeGuard>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <climits>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace putgd {
void EditorSession::request(const QString& command,const QJsonObject& params,Reply reply){
 {
  std::lock_guard lock(mutex_);
  if(phase_=="editing"&&queue_.size()<EditorJob::maximumQueuedCommands){queue_.append({command,params,std::move(reply)});return;}
 }
 if(phase()=="editing")reply({"editor_busy","Too many editor commands are waiting",{}},{});
 else if(phase()=="starting")reply({"editor_starting","The editor window is still opening",{}},{});
 else reply({"editor_finalizing","The edit was already submitted or discarded",{}},{});
}
void EditorSession::setAppearance(const QString& scheme){std::lock_guard lock(mutex_);pendingAppearance_=scheme=="light"?QStringLiteral("light"):QStringLiteral("dark");}
QString EditorSession::takeAppearance(){std::lock_guard lock(mutex_);return std::exchange(pendingAppearance_,{});}
QString EditorSession::phase() const {std::lock_guard lock(mutex_);return phase_;}
void EditorSession::setPhase(const QString& phase){std::lock_guard lock(mutex_);phase_=phase;}
QList<EditorSession::Pending> EditorSession::take(){std::lock_guard lock(mutex_);return std::exchange(queue_,{});}
void EditorSession::setCopyHandler(CopyHandler handler){std::lock_guard lock(mutex_);copyHandler_=std::move(handler);}
EditorSession::CopyHandler EditorSession::copyHandler() const {std::lock_guard lock(mutex_);return copyHandler_;}
void EditorSession::finishCopy(Error error){std::lock_guard lock(mutex_);copyResult_=std::move(error);}
std::optional<Error> EditorSession::takeCopyResult(){std::lock_guard lock(mutex_);return std::exchange(copyResult_,std::nullopt);}
void EditorSession::setTextCopyHandler(TextCopyHandler handler){std::lock_guard lock(mutex_);textCopyHandler_=std::move(handler);}
EditorSession::TextCopyHandler EditorSession::textCopyHandler() const {std::lock_guard lock(mutex_);return textCopyHandler_;}
void EditorSession::finishTextCopy(const QString& copyId,Error error){std::lock_guard lock(mutex_);textCopyResults_.append({copyId,std::move(error)});}
QList<std::pair<QString,Error>> EditorSession::takeTextCopyResults(){std::lock_guard lock(mutex_);return std::exchange(textCopyResults_,{});}

namespace {
// At most six distinct lowercase #rrggbb strings, as the settings validator accepts.
bool recentColorList(const QJsonValue& value,QStringList& colors){
 static const QRegularExpression hex("^#[0-9a-f]{6}$");
 if(!value.isArray()||value.toArray().size()>Settings::maximumRecentColors)return false;
 colors.clear();
 for(const auto& color:value.toArray()){if(!color.isString()||!hex.match(color.toString()).hasMatch()||colors.contains(color.toString()))return false;colors.append(color.toString());}
 return true;
}
Error failure(const QString& code){
 if(code=="edit_cancelled"||code=="cancelled")return {"cancelled","Edit discarded; nothing was uploaded",{}};
 if(code=="input_excessive")return {"edit_input_too_large","This image is too large to edit; use a smaller copy",{}};
 if(code=="source_unsupported")return {"source_unsupported","The image could not be opened for editing",{}};
 if(code=="edit_export_failed")return {"edit_export_failed","The edited image could not be prepared",{}};
 if(code=="editor_timeout")return {code,"The image editor did not respond in time",{}};
 if(code=="editor_start")return {code,"The image editor could not start",{}};
 if(code=="edit_staging_failed")return {code,"Could not save the image for editing; check free disk space",{}};
 if(code=="editor_unavailable")return {code,"The image editor is not available on this platform",{}};
 if(code=="editor_output_invalid"||code=="edit_output_invalid")return {"edit_output_invalid","The editor returned an image that is not a sanitized PNG",{}};
 return {code,"The image editor stopped before handing off an image",{}};
}
#ifdef Q_OS_UNIX
QJsonObject build(){return QJsonDocument::fromJson(buildManifestJson()).object();}
QJsonObject envelope(const QString& nonce){return {{"role","image-edit"},{"nonce",nonce},{"generation","1"}};}
bool validEnvelope(const QJsonObject& payload,const QString& nonce){return payload["role"]=="image-edit"&&payload["nonce"]==nonce&&payload["generation"]=="1";}
bool number(const QJsonValue& value,qint64& out){bool ok=false;if(!value.isString())return false;out=value.toString().toLongLong(&ok);return ok&&out>=0&&QString::number(out)==value.toString();}
bool privateFile(const struct stat& s,mode_t mode){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==mode&&s.st_nlink==1;}
bool sameFile(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool ownedDirectory(const QString& path,struct stat& s){return !::lstat(QFile::encodeName(path).constData(),&s)&&S_ISDIR(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0700;}
QJsonObject descriptor(const QString& path,const struct stat& s,const QString& hash){
 return {{"path",path},{"device",QString::number(qulonglong(s.st_dev))},{"inode",QString::number(qulonglong(s.st_ino))},{"bytes",QString::number(s.st_size)},{"sha256",hash}};
}
QString digest(QFile& file,qint64 size){
 if(!file.seek(0))return {};
 QCryptographicHash hash(QCryptographicHash::Sha256);qint64 total=0;
 while(total<size){const auto chunk=file.read(std::min<qint64>(1024*1024,size-total));if(chunk.isEmpty())return {};hash.addData(chunk);total+=chunk.size();}
 return QString::fromLatin1(hash.result().toHex());
}
#endif
}

EditorJob::EditorJob(QString root,QString helper,int startup,int finalize,int command)
 :root_(std::move(root)),helperPath_(helper.isEmpty()?QCoreApplication::applicationDirPath()+"/putgd-ui":std::move(helper)),
  startupTimeoutMs_(std::clamp(startup,100,60000)),finalizeTimeoutMs_(std::clamp(finalize,100,600000)),commandTimeoutMs_(std::clamp(command,50,9000)){}

void EditorJob::discardStaging(const QString& root){
#ifdef Q_OS_UNIX
 struct stat s{};if(::lstat(QFile::encodeName(root).constData(),&s)||!S_ISDIR(s.st_mode)||s.st_uid!=getuid())return;
#endif
 QDir(root).removeRecursively();
}

EditorResult EditorJob::run(QImage& image,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>& session) const {
 if(image.isNull()||qint64(image.width())*image.height()>admissionPixels){
  const bool excessive=!image.isNull();image=QImage();
  EditorResult result;result.error=failure(excessive?"input_excessive":"source_unsupported");session->setPhase("closed");return result;
 }
 // Fast zlib staging keeps the private source handoff short; it is never published.
 return supervise([&image](QFile& input){
  QImageWriter writer(&input,"png");writer.setQuality(80);const bool written=writer.write(image);image=QImage();return written;
 },destination,outputBytes,session);
}

EditorResult EditorJob::runFile(const QString& path,qint64 maximumInputBytes,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>& session) const {
 // The resident copies the user's PNG/JPEG bytes verbatim and never decodes them;
 // the helper applies pixel admission before allocating and decodes once.
 return supervise([&](QFile& input){
  QFile source(path);
  if(!QFileInfo(path).isFile()||!source.open(QIODevice::ReadOnly)||source.size()<1||source.size()>maximumInputBytes)return false;
  qint64 copied=0;
  while(!source.atEnd()){
   if(session->cancelled())return false;
   const auto chunk=source.read(1024*1024);
   if(chunk.isEmpty()||copied+chunk.size()>maximumInputBytes||input.write(chunk)!=chunk.size())return false;
   copied+=chunk.size();
  }
  return copied==source.size();
 },destination,outputBytes,session);
}

EditorResult EditorJob::supervise(const std::function<bool(QFile&)>& stage,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>& session) const {
 EditorResult result;
 auto fail=[&](const QString& code){result.error=failure(code);session->setPhase("closed");for(auto& pending:session->take())pending.reply(failure("editor_closed"),{});return result;};
#ifndef Q_OS_UNIX
 Q_UNUSED(stage);Q_UNUSED(destination);Q_UNUSED(outputBytes);
 return fail("editor_unavailable");
#else
 if(outputBytes<1)return fail("input_excessive");
 if(session->cancelled())return fail("cancelled");
 struct stat rootIdentity{};
 if(!QDir().mkpath(root_)||!QFile::setPermissions(root_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)||!ownedDirectory(root_,rootIdentity))return fail("editor_io");
 QTemporaryDir directory(root_+"/pge-XXXXXX");struct stat directoryIdentity{};
 if(!directory.isValid()||!QFile::setPermissions(directory.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)||!ownedDirectory(directory.path(),directoryIdentity))return fail("editor_io");
 const auto inputPath=directory.path()+"/input.img",outputPath=directory.path()+"/output.png";
 QFile input(inputPath),output(outputPath);struct stat inputIdentity{},outputIdentity{};
 if(!input.open(QIODevice::ReadWrite|QIODevice::NewOnly)||!input.setPermissions(QFile::ReadOwner|QFile::WriteOwner))return fail("editor_io");
 // A failed write (a full disk, a quota) is not an unsupported image.
 const auto writeFailed=[&input]{return input.error()==QFileDevice::WriteError||input.error()==QFileDevice::ResourceError;};
 if(!stage(input))return fail(session->cancelled()?"cancelled":writeFailed()?"edit_staging_failed":"source_unsupported");
 if(!input.flush())return fail("edit_staging_failed");
 if(!input.setPermissions(QFile::ReadOwner)||::fstat(input.handle(),&inputIdentity)||!privateFile(inputIdentity,0400)||inputIdentity.st_size<1)return fail("editor_io");
 const auto inputHash=digest(input,inputIdentity.st_size);if(inputHash.isEmpty())return fail("editor_io");
 if(!output.open(QIODevice::ReadWrite|QIODevice::NewOnly)||!output.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||::fstat(output.handle(),&outputIdentity)||!privateFile(outputIdentity,0600))return fail("editor_io");
 if(session->cancelled())return fail("cancelled");

 const auto nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);
 auto request=envelope(nonce);request["build"]=build();
 request["input"]=descriptor(inputPath,inputIdentity,inputHash);request["output"]=descriptor(outputPath,outputIdentity,{});
 request["limits"]=QJsonObject{{"imagePixels",QString::number(admissionPixels)},{"outputBytes",QString::number(outputBytes)}};
 if(submissionError_)request["submissionError"]=QJsonObject{{"code",submissionError_.code},{"message",submissionError_.message}};
 request["destination"]=destination.left(200).isEmpty()?QStringLiteral("Upload"):destination.left(200);
 request["recentColors"]=QJsonArray::fromStringList(recentColors_.mid(0,Settings::maximumRecentColors));
 request["appearance"]=appearance_=="light"?QStringLiteral("light"):QStringLiteral("dark");
 request["textLanguages"]=QJsonArray::fromStringList(textjob::validLanguages(textLanguages_)?textLanguages_:QStringList{});
 QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);
 UiControl control(process);
 bool ready=false,editing=false,finalizing=false,checkpoint=false,protocolFailed=false,copying=false;QString copyId;QJsonObject reply;qint64 stderrBytes=0;QSet<QString> textCopies;
 // Copy image: the helper staged copy.png in this private directory. Read it
 // through the pinned identities, check it is the sanitized PNG it announced,
 // remove it, then hand the bytes to the clipboard. Never kept or uploaded.
 // The staging directory stays pinned by descriptor: copy.png is opened and
 // removed through it, so a replaced path cannot redirect the handoff.
 const int stagingFd=::open(QFile::encodeName(directory.path()).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_DIRECTORY);
 const auto closeStaging=qScopeGuard([stagingFd]{if(stagingFd>=0)::close(stagingFd);});
 struct stat stagingIdentity{};const bool stagingPinned=stagingFd>=0&&!::fstat(stagingFd,&stagingIdentity)&&sameFile(stagingIdentity,directoryIdentity);
 const auto takeCopy=[&](const QJsonObject& payload,QByteArray& bytes)->Error{
  const Error invalid{"edit_output_invalid","The editor returned an image that is not a sanitized PNG",{}};
  qint64 count=0;const auto hash=payload["sha256"].toString();
  struct stat identity{};
  if(!stagingPinned)return {"editor_io","The editor's private staging is not available",{}};
  const int fd=::openat(stagingFd,"copy.png",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
  ::unlinkat(stagingFd,"copy.png",0);
  if(fd<0)return {"editor_io","The edited image for the clipboard is missing",{}};
  QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return {"editor_io","The edited image for the clipboard is missing",{}};}
  if(::fstat(fd,&identity)||!S_ISREG(identity.st_mode)||identity.st_uid!=getuid()||(identity.st_mode&0777)!=0600)return {"editor_io","The edited image for the clipboard is not private",{}};
  if(!number(payload["bytes"],count)||count<1||count>maximumCopyBytes||identity.st_size!=count||hash.size()!=64||QByteArray::fromHex(hash.toLatin1()).toHex()!=hash.toLatin1())return invalid;
  bytes=file.read(count+1);
  if(bytes.size()!=count||QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=hash)return invalid;
  // The same pixel admission as the upload handoff, before any adapter decodes it.
  QSize size;if(inspectSanitizedPng(bytes,&size)||size.width()!=payload["width"].toInt()||size.height()!=payload["height"].toInt()||qint64(size.width())*size.height()>admissionPixels)return invalid;
  return {};
 };
 struct Outstanding {EditorSession::Reply reply;qint64 deadline=0;};
 QHash<QString,Outstanding> outstanding;
 // Requests answered with editor_timeout: the helper may still finish them
 // (a large Save a copy or Copy image); its late answer is dropped, not fatal.
 QSet<QString> expired;
 QElapsedTimer clock,phaseClock;clock.start();phaseClock.start();
 QObject::connect(&control,&UiControl::failed,&process,[&](Error){protocolFailed=true;});
 HelperOutput helperOutput("putgd-ui --edit-image",logUi());
 QObject::connect(&process,&QProcess::readyReadStandardError,&process,[&]{
  process.setReadChannel(QProcess::StandardError);
  while(process.bytesAvailable()>0){const auto bytes=process.read(16384);stderrBytes+=bytes.size();helperOutput.append(bytes);if(stderrBytes>1024*1024){protocolFailed=true;break;}}
  process.setReadChannel(QProcess::StandardOutput);
 });
 QObject::connect(&control,&UiControl::frame,&process,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(protocolFailed||!validEnvelope(payload,nonce)){protocolFailed=true;return;}
  if(type=="ready"&&!ready&&payload.size()==4&&payload["build"].toObject()==build()){ready=true;if(control.send("activate",envelope(nonce)).isEmpty())protocolFailed=true;return;}
  if(type=="editing"&&ready&&!editing&&!checkpoint&&payload.size()==3){editing=true;session->setPhase("editing");return;}
  if(type=="finalizing"&&editing&&!finalizing&&!checkpoint&&payload.size()==3){finalizing=true;session->setPhase("finalizing");phaseClock.restart();return;}
  // A preparation the helper could recover from (encoding, size): it handed
  // nothing off and editing resumes, without a deadline again.
  if(type=="editing"&&finalizing&&!checkpoint&&payload.size()==3){finalizing=false;session->setPhase("editing");phaseClock.restart();return;}
  if(type=="commandResult"&&editing){
   const auto id=payload["requestId"].toString();const auto found=outstanding.constFind(id);
   if(found==outstanding.cend()&&payload["ok"].isBool()&&expired.remove(id))return;
   if(found==outstanding.cend()||!payload["ok"].isBool()){protocolFailed=true;return;}
   const auto callback=found->reply;outstanding.remove(id);
   if(payload["ok"].toBool()){if(payload.size()!=6||!payload["result"].isObject()){protocolFailed=true;return;}callback({},payload["result"].toObject());}
   else{if(payload.size()!=7||!payload["code"].isString()||!payload["message"].isString()){protocolFailed=true;return;}callback({payload["code"].toString().left(64),payload["message"].toString().left(512),{}},{});}
   return;
  }
  if(type=="copyImage"&&editing&&!finalizing&&!checkpoint){
   if(payload.size()!=8||!payload["width"].isDouble()||!payload["height"].isDouble()||!payload["copyId"].isString()||payload["copyId"].toString().size()>64){protocolFailed=true;return;}
   if(copying){
    // The helper gave up on an earlier copy that the clipboard still holds:
    // refuse this one (its staged file goes too) and keep the edit open.
    if(stagingPinned)::unlinkat(stagingFd,"copy.png",0);
    auto frame=envelope(nonce);frame["copyId"]=payload["copyId"];frame["ok"]=false;frame["code"]="editor_busy";frame["message"]="The previous copy is still being delivered to the clipboard";
    if(control.send("copyResult",frame).isEmpty())protocolFailed=true;
    return;
   }
   copying=true;copyId=payload["copyId"].toString();QByteArray bytes;auto error=takeCopy(payload,bytes);
   const auto handler=session->copyHandler();
   if(!error&&!handler)error={"clipboard_unavailable","The clipboard is not available",{}};
   if(error){session->finishCopy(error);return;}
   handler(std::move(bytes),[session](Error result){session->finishCopy(std::move(result));});
   return;
  }
  // Copy text (#1428): plain text, answered with copyResult for its copyId.
  if(type=="copyText"&&editing&&!finalizing&&!checkpoint){
   if(payload.size()!=5||!payload["copyId"].isString()||payload["copyId"].toString().isEmpty()||payload["copyId"].toString().size()>64||!payload["text"].isString()||payload["text"].toString().size()>maximumTextCopyCharacters){protocolFailed=true;return;}
   const auto id=payload["copyId"].toString();const auto handler=session->textCopyHandler();
   if(textCopies.contains(id)){protocolFailed=true;return;}
   if(!handler||textCopies.size()>=maximumTextCopies){auto frame=envelope(nonce);frame["copyId"]=id;frame["ok"]=false;frame["code"]=handler?"editor_busy":"clipboard_unavailable";frame["message"]=handler?"Earlier copies are still being delivered to the clipboard":"The clipboard is not available";if(control.send("copyResult",frame).isEmpty())protocolFailed=true;return;}
   textCopies.insert(id);
   handler(payload["text"].toString(),[session,id](Error result){session->finishTextCopy(id,std::move(result));});
   return;
  }
  if(type=="checkpoint"&&ready&&!checkpoint&&payload["ok"].isBool()){
   QStringList colors;
   if(!recentColorList(payload["recentColors"],colors)){protocolFailed=true;return;}
   if(!payload["ok"].toBool()){
    const auto code=payload["code"].toString();
    if(payload.size()!=6||!QStringList{"edit_cancelled","source_unsupported","input_excessive","edit_export_failed","image_job_io"}.contains(code)){protocolFailed=true;return;}
   }else{
    qint64 count=0;const auto hash=payload["sha256"].toString();
    if(!finalizing||payload.size()!=9||!number(payload["bytes"],count)||count<1||count>outputBytes||hash.size()!=64||QByteArray::fromHex(hash.toLatin1()).toHex()!=hash.toLatin1()||!payload["width"].isDouble()||!payload["height"].isDouble()){protocolFailed=true;return;}
    const auto width=payload["width"].toInteger(),height=payload["height"].toInteger();
    if(payload["width"].toDouble()!=width||payload["height"].toDouble()!=height||width<1||height<1||width>INT_MAX||height>INT_MAX||width>admissionPixels/height){protocolFailed=true;return;}
   }
   checkpoint=true;reply=payload;result.recentColors=colors;session->setPhase("closed");phaseClock.restart();
   if(control.send("checkpointAccepted",envelope(nonce)).isEmpty())protocolFailed=true;
   return;
  }
  protocolFailed=true;
 });
 const auto inherited=QProcessEnvironment::systemEnvironment();
 if(inherited.value("QT_QPA_PLATFORM").isEmpty()&&inherited.value("DISPLAY").isEmpty()&&inherited.value("WAYLAND_DISPLAY").isEmpty()){
  auto environment=inherited;environment.insert("QT_QPA_PLATFORM","offscreen");process.setProcessEnvironment(environment);
 }
 process.start(helperPath_,{"--edit-image"});
 QString error;
 if(!process.waitForStarted(5000)){error="editor_start";helperOutput.failedToStart(process.errorString());}
 else{helperOutput.started(process.processId());if(control.send("bootstrap",request).isEmpty())protocolFailed=true;}
 while(error.isEmpty()&&process.state()!=QProcess::NotRunning&&!protocolFailed){
  if(session->cancelled()&&!checkpoint){error="cancelled";break;}
  if(!editing&&!checkpoint&&phaseClock.elapsed()>startupTimeoutMs_){error="editor_timeout";break;}
  if(finalizing&&!checkpoint&&phaseClock.elapsed()>finalizeTimeoutMs_){error="editor_timeout";break;}
  if(checkpoint&&phaseClock.elapsed()>5000){error="editor_exit";break;}
  if(ready&&!checkpoint){
   const auto scheme=session->takeAppearance();
   if(!scheme.isEmpty()){auto frame=envelope(nonce);frame["scheme"]=scheme;if(control.send("appearance",frame).isEmpty()){protocolFailed=true;break;}}
  }
  if(copying)if(const auto copied=session->takeCopyResult()){
   copying=false;auto frame=envelope(nonce);frame["copyId"]=copyId;frame["ok"]=!*copied;
   if(*copied){frame["code"]=copied->code.left(64);frame["message"]=copied->message.left(256);}
   if(!checkpoint&&control.send("copyResult",frame).isEmpty()){protocolFailed=true;break;}
  }
  for(const auto& [id,copied]:session->takeTextCopyResults()){
   if(!textCopies.remove(id))continue;
   auto frame=envelope(nonce);frame["copyId"]=id;frame["ok"]=!copied;
   if(copied){frame["code"]=copied.code.left(64);frame["message"]=copied.message.left(256);}
   if(!checkpoint&&control.send("copyResult",frame).isEmpty()){protocolFailed=true;break;}
  }
  for(auto& pending:session->take()){
   if(!editing||finalizing||checkpoint){pending.reply(failure("editor_finalizing"),{});continue;}
   const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
   auto frame=envelope(nonce);frame["requestId"]=id;frame["command"]=pending.command;frame["params"]=pending.params;
   if(control.send("command",frame).isEmpty()){protocolFailed=true;pending.reply(failure("editor_protocol"),{});break;}
   // Quick redact (#1428) answers when recognition is done, under the recognition deadline.
   bool recognizing=false;for(const auto& entry:pending.params["commands"].toArray())recognizing=recognizing||entry.toObject()["op"]=="redactText";
   const qint64 limit=pending.command=="apply"&&recognizing?qMax<qint64>(commandTimeoutMs_,textjob::maximumDeadlineMs+5000):commandTimeoutMs_;
   outstanding.insert(id,{std::move(pending.reply),clock.elapsed()+limit});
  }
  for(auto it=outstanding.begin();it!=outstanding.end();){if(clock.elapsed()>it->deadline){it->reply({"editor_timeout","The editor did not answer this command in time",{}},{});if(expired.size()<256)expired.insert(it.key());it=outstanding.erase(it);}else ++it;}
  process.waitForReadyRead(50);
 }
 for(auto& pending:outstanding)pending.reply({"editor_closed","The editor closed before answering",{}},{});
 outstanding.clear();session->setPhase("closed");
 for(auto& pending:session->take())pending.reply(failure("editor_finalizing"),{});
 if(protocolFailed&&error.isEmpty())error="editor_protocol";
 const bool stopping=process.state()!=QProcess::NotRunning;
 if(stopping){control.stop();process.terminate();if(!process.waitForFinished(500)){process.kill();process.waitForFinished(1500);}}
 if(helperOutput.pid()>0){helperOutput.append(process.readAllStandardError());helperOutput.finished(process.exitCode(),process.exitStatus(),stopping);}
 if(error.isEmpty()&&(!checkpoint||process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0))error="editor_exit";
 control.stop();
 if(!error.isEmpty())return fail(error);
 if(!reply["ok"].toBool())return fail(reply["code"].toString());
 // Bind the handoff to the authenticated digest and the pinned private files.
 struct stat finalDirectory{},finalOutput{},namedOutput{},finalInput{};
 if(!ownedDirectory(directory.path(),finalDirectory)||!sameFile(directoryIdentity,finalDirectory))return fail("editor_io");
 if(::fstat(output.handle(),&finalOutput)||!privateFile(finalOutput,0600)||!sameFile(outputIdentity,finalOutput)||::lstat(QFile::encodeName(outputPath).constData(),&namedOutput)||!sameFile(finalOutput,namedOutput)||finalOutput.st_size!=reply["bytes"].toString().toLongLong()||finalOutput.st_size>outputBytes)return fail("editor_io");
 if(::fstat(input.handle(),&finalInput)||!privateFile(finalInput,0400)||!sameFile(inputIdentity,finalInput)||finalInput.st_size!=inputIdentity.st_size||digest(input,inputIdentity.st_size)!=inputHash)return fail("editor_io");
 if(!output.seek(0))return fail("editor_io");
 auto encoded=output.read(finalOutput.st_size+1);
 if(encoded.size()!=finalOutput.st_size||QString::fromLatin1(QCryptographicHash::hash(encoded,QCryptographicHash::Sha256).toHex())!=reply["sha256"].toString())return fail("editor_io");
 QSize size;if(inspectSanitizedPng(encoded,&size))return fail("edit_output_invalid");
 if(size.width()!=reply["width"].toInt()||size.height()!=reply["height"].toInt())return fail("edit_output_invalid");
 result.png=std::move(encoded);result.width=size.width();result.height=size.height();
 return result;
#endif
}
}
