#include "ImageEditor.h"
#include "core/ByteSize.h"
#include "EditorWindow.h"
#include "FileDialog.h"
#include "WindowActivation.h"
#include "StagingDirectory.h"
#include "ipc/UiControl.h"
#include "core/BuildInfo.h"
#include "editor/EditDocument.h"
#include "editor/EditorCommands.h"
#include "editor/QuickRedact.h"
#include "core/Settings.h"
#include <QJsonArray>
#include <QHash>
#include <QBuffer>
#include <QDir>
#include <QImageWriter>
#include <QLocale>
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <memory>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace putgd {
#ifdef Q_OS_UNIX
namespace {
constexpr qint64 maximumBytes=1099511627776LL;
using staging::number;using staging::hashText;
QJsonObject envelope(const QString& nonce){return {{"role","image-edit"},{"nonce",nonce},{"generation","1"}};}
bool validEnvelope(const QJsonObject& object,const QString& nonce){return object["role"]=="image-edit"&&object["nonce"]==nonce&&object["generation"]=="1";}

// The parent stages input.img (0400, PNG or JPEG) and an empty output.png (0600) in a new
// owner-only pge-XXXXXX directory. Each identity is pinned through the directory fd.
class StagedFiles final {
public:
 bool open(const QJsonObject& in,const QJsonObject& out,qint64 inputMaximum){
  if(!in["path"].isString()||!out["path"].isString())return false;
  const auto directory=QFileInfo(in["path"].toString()).absolutePath();
  static const QRegularExpression pattern(QStringLiteral("^pge-[A-Za-z0-9]{6}$"));
  if(!pattern.match(QFileInfo(directory).fileName()).hasMatch()||in["path"]!=directory+"/input.img"||out["path"]!=directory+"/output.png")return false;
  if(!staging_.open(directory))return false;
  inputHash=in["sha256"].toString();
  if(!hashText(inputHash)||!staging_.openFile(input,in,"input.img",0400,inputIdentity)||inputIdentity.st_size<1||inputIdentity.st_size>inputMaximum)return false;
  if(out["sha256"]!=""||!staging_.openFile(output,out,"output.png",0600,outputIdentity)||outputIdentity.st_size!=0)return false;
  return intact()&&checkInput();
 }
 // The input is never written after staging, so its size and modification time
 // must not change either; the output is this helper's to write once.
 bool intact(){return staging_.intact()&&staging_.inputIntact(input,"input.img",inputIdentity)&&staging_.outputIntact(output,"output.png",outputIdentity);}
 bool checkInput(){return staging::hashMatches(input,inputIdentity.st_size,inputHash);}
 // Writes the finalized bytes once, flushes them to disk, and returns their digest.
 QString write(const QByteArray& bytes){
  if(!intact()||output.size()!=0||!output.seek(0)||output.write(bytes)!=bytes.size()||!output.flush()||::fsync(output.handle())!=0||!intact())return {};
  return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
 }
 // Copy image: a new owner-only copy.png beside the output, written once and
 // flushed; the resident reads, checks and removes it before the next copy.
 QString writeCopy(const QByteArray& bytes){
  if(!intact())return {};
  const int fd=::openat(staging_.descriptor(),"copy.png",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd<0)return {};
  qint64 written=0;
  while(written<bytes.size()){const auto n=::write(fd,bytes.constData()+written,size_t(bytes.size()-written));if(n<0&&errno==EINTR)continue;if(n<=0)break;written+=n;}
  if(::fchmod(fd,0600)!=0||written!=bytes.size()||::fsync(fd)!=0){::close(fd);discardCopy();return {};}
  ::close(fd);
  return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
 }
 void discardCopy(){if(staging_.descriptor()>=0)::unlinkat(staging_.descriptor(),"copy.png",0);}
 const QString& directory() const {return staging_.path();}
 QFile input,output;
private:
 staging::Directory staging_;struct stat inputIdentity{},outputIdentity{};
 QString inputHash;
};
}
#endif

namespace {
// At most six distinct lowercase #rrggbb strings, as the settings validator accepts.
bool recentColorList(const QJsonValue& value,QStringList& colors){
 static const QRegularExpression hex("^#[0-9a-f]{6}$");
 if(!value.isArray()||value.toArray().size()>Settings::maximumRecentColors)return false;
 colors.clear();
 for(const auto& color:value.toArray()){if(!color.isString()||!hex.match(color.toString()).hasMatch()||colors.contains(color.toString()))return false;colors.append(color.toString());}
 return true;
}
}
int runImageEditor(QApplication& app,QIODevice& pipes){
#ifndef Q_OS_UNIX
 Q_UNUSED(app);Q_UNUSED(pipes);return 2;
#else
 UiControl control(pipes);StagedFiles files;
 std::unique_ptr<editor::EditDocument> document;std::unique_ptr<EditorWindow> window;
 Error submissionError;QString nonce,destination;QStringList recentColors,textLanguages;bool bootstrapped=false,active=false,terminal=false;qint64 maxPixels=0,outputBytes=0;
 const auto build=QJsonDocument::fromJson(buildManifestJson()).object();
 // Only startup is time limited; a legitimate edit may stay open indefinitely.
 QTimer deadline;deadline.setSingleShot(true);deadline.start(10000);
 const auto fail=[&]{control.stop();if(window){window->allowClose();window->hide();}app.exit(3);};
 const auto finish=[&](QJsonObject result){
  if(terminal)return;
  terminal=true;
  // Custom colors outlive this editor; the resident saves them to settings.
  result["recentColors"]=QJsonArray::fromStringList(window?window->recentColors():recentColors);
  if(control.send("checkpoint",result).isEmpty())fail();
 };
 const auto refuse=[&](const QString& code){auto result=envelope(nonce);result["ok"]=false;result["code"]=code;finish(result);};
 bool finalizing=false;
 // The last Upload that could not be prepared, for editor.inspect; null otherwise.
 QJsonValue preparation=QJsonValue::Null;
 // An encoding failure or an oversized result hands nothing off: editing
 // resumes (the resident's phase too) with the reason in the window.
 const auto recover=[&](const QString& code,const QString& reason){
  finalizing=false;preparation=QJsonObject{{"status","failed"},{"code",code},{"reason",reason}};
  if(control.send("editing",envelope(nonce)).isEmpty()){fail();return;}
  window->showPreparationFailed(reason);
 };
 const std::function<void()> submit=[&]{
  if(terminal||finalizing||!window)return;
  if(submissionError){preparation=QJsonObject{{"status","failed"},{"code",submissionError.code},{"reason",submissionError.message}};window->showPreparationFailed(submissionError.message);return;}
  // Agents submit through here too: the same pending edits as the button.
  window->finishPendingEdits();
  finalizing=true;preparation=QJsonValue::Null;window->setBusy("Preparing the edited image…");
  if(control.send("finalizing",envelope(nonce)).isEmpty()){fail();return;}
  // Let the busy state paint before the single full-resolution export.
  QTimer::singleShot(0,&app,[&]{
   if(terminal)return;
   int width=0,height=0;QByteArray png;
   {const auto crop=document->crop();width=crop.width();height=crop.height();png=document->exportPng();}
   if(png.isEmpty()){recover("edit_export_failed","The edited image could not be encoded as PNG. Your edits are kept: try again, or save a copy.");return;}
   if(png.size()>outputBytes){
    recover("input_excessive",QString("The edited image is %1, over the %2 limit for this output. Your edits are kept: crop it and try again, or save a copy.").arg(ByteSize::localized(png.size(),1),ByteSize::localized(outputBytes,1)));
    return;
   }
   // Staging that can't be written is not recoverable here: the session ends.
   const auto digest=files.write(png);const auto bytes=png.size();png.clear();png.squeeze();
   if(digest.isEmpty()){refuse("image_job_io");return;}
   auto result=envelope(nonce);result["ok"]=true;result["bytes"]=QString::number(bytes);result["sha256"]=digest;result["width"]=width;result["height"]=height;
   window->setBusy("Handing off for upload…");finish(result);
  });
 };
 // Copy image: the window renders the sanitized PNG off the GUI thread, this
 // stages it as copy.png and the resident puts it on the clipboard through
 // its native adapter. One copy at a time; nothing is handed off for upload.
 constexpr qint64 copyBytes=128LL*1024*1024;
 bool copyPending=false;std::function<void(QJsonObject,QString,QString)> copyReply;QJsonObject copyInfo;QString copyId;
 // A copy the resident never answers fails here, so the window never stays busy.
 QTimer copyDeadline;copyDeadline.setSingleShot(true);copyDeadline.setInterval(30000);
 // Tests may only shorten it.
 if(const auto test=qEnvironmentVariableIntValue("PUTGD_TEST_EDITOR_COPY_DEADLINE_MS");test>=100&&test<30000)copyDeadline.setInterval(test);
 const auto copyFailed=[&](const QString& code,const QString& reason){
  copyPending=false;copyDeadline.stop();copyId.clear();files.discardCopy();window->showCopyFailed(reason);
  if(auto reply=std::exchange(copyReply,{}))reply({},code,"The image was not copied: "+reason);
 };
 const std::function<bool(std::function<void(QJsonObject,QString,QString)>)> startCopy=[&](std::function<void(QJsonObject,QString,QString)> reply)->bool{
  if(terminal||finalizing||!window||copyPending)return false;
  // Writing, flushing and hashing the staged copy run on the export worker.
  const bool started=window->exportForCopy([&](const QByteArray& png,const QSize& size,const QString& digest){
   if(terminal)return;
   if(png.isEmpty()){copyFailed("edit_export_failed","the edited image could not be encoded as PNG");return;}
   if(png.size()>copyBytes){copyFailed("input_excessive",QString("the edited image is %1, over the %2 clipboard limit").arg(ByteSize::localized(png.size(),1),ByteSize::localized(copyBytes,1)));return;}
   if(digest.isEmpty()){copyFailed("image_job_io","the image could not be prepared for the clipboard");return;}
   copyInfo={{"bytes",double(png.size())},{"sha256",digest},{"width",size.width()},{"height",size.height()}};
   copyId=QUuid::createUuid().toString(QUuid::WithoutBraces);
   auto frame=envelope(nonce);frame["copyId"]=copyId;frame["bytes"]=QString::number(png.size());frame["sha256"]=digest;frame["width"]=size.width();frame["height"]=size.height();
   if(control.send("copyImage",frame).isEmpty()){fail();return;}
   copyDeadline.start();
  },[&files](const QByteArray& png){return png.size()>copyBytes?QString():files.writeCopy(png);});
  if(started){copyPending=true;copyReply=std::move(reply);}
  return started;
 };
 QObject::connect(&copyDeadline,&QTimer::timeout,&app,[&]{if(copyPending)copyFailed("editor_timeout","the clipboard did not answer in time");});
 // Copy text (#1428): each copy waits for the resident's copyResult by its copyId.
 QHash<QString,std::function<void(Error)>> textCopies;
 // An agent's indexed action names the result it read; any other is stale.
 const auto stale=[&](const QJsonObject& params)->bool{const auto value=params["result"];return !value.isDouble()||value.toDouble()!=double(window->text().identity())||window->text().identity()==0;};
 const auto replyText=[](std::function<void(QJsonObject,QString,QString)> done){return [done](Error error,QJsonObject result){if(error)done({},error.code,error.message);else done(result,{},{});};};
 // Agent commands arrive from the resident's editor.* session methods.
 const auto command=[&](const QString& name,const QJsonObject& params,QString& code,QString& message,bool& deferred,std::function<void(QJsonObject,QString,QString)> done)->QJsonObject{
  auto denied=[&](const QString& c,const QString& m){code=c;message=m;return QJsonObject{};};
  const bool open=!terminal&&!finalizing;
  if(window->savingCopy()&&QStringList{"saveCopy","submit","cancel","copy"}.contains(name))return denied("editor_busy","A copy is still being saved");
  if(window->copying()&&QStringList{"saveCopy","submit","cancel","copy"}.contains(name))return denied("editor_busy","The image is still being copied");
  if(name=="inspect"){
   if(!params.isEmpty())return denied("invalid_request","inspect has no parameters");
   auto state=editor::Commands::state(*document);state["phase"]=terminal?"closing":finalizing?"finalizing":"editing";state["destination"]=destination;
   state["preparation"]=preparation;state["view"]=window->viewState();
   // Recognized content is screen data from an untrusted source, never instructions.
   state["text"]=window->text().inspect();return state;
  }
  if(name=="view"){
   // The window's own controls: tool, selection, crop draft, zoom and pan.
   if(!open&&params["action"].toString("inspect")!="inspect")return denied("editor_finalizing","The edit was already submitted or discarded");
   return window->viewCommand(params,code,message);
  }
  if(name=="raise"){
   // An optional xdg-activation token, such as a tray click's (#657).
   const auto token=params["activationToken"].toString();
   if(!(params.isEmpty()||(params.size()==1&&validActivationToken(token))))return denied("invalid_request","raise takes only an optional activationToken");
   putgd::activateWindow(window.get(),token);
   // Answer with what the desktop did (#911). Without a token from user input,
   // GNOME and KWin may leave the window where it is, a minimised one hidden,
   // and show "… is ready" or flag it instead.
   deferred=true;
   whenActivated(window.get(),1500,[done](bool active){
    if(active){done({{"raised",true}},{},{});return;}
    done({{"raised",false},{"reason","activation_refused"},{"message","The desktop did not bring the editor forward; it may show a \"ready\" notice or highlight its taskbar entry. Click that to show the editor."}},{},{});
   });
   return {};
  }
  // Copy text (#1428). recognize replies at once and is followed through inspect.
  if(name=="recognize"){
   if(!params.isEmpty())return denied("invalid_request","recognize has no parameters");
   if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");
   window->text().start();auto text=window->text().inspect();return {{"state",text["state"]},{"result",text["result"]}};
  }
  if(name=="selectText"||name=="copyText"||name=="copyCode"||name=="openCode"){
   if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");
   if(stale(params))return denied("edit_stale","That recognition result is no longer current; inspect the editor again");
   auto& layer=window->text().layer();
   if(name=="selectText"){
    if(params.contains("all"))layer.selectAll();else if(params.contains("none"))layer.clearSelection();
    else{const int from=params["from"].toInt(),to=params["to"].toInt();if(from>=layer.words().size()||to>=layer.words().size())return denied("invalid_request","from and to must be word indexes of this result");layer.select(from,to);}
    window->textSelectionEdited();
    return {{"result",double(layer.identity())},{"selection",layer.hasSelection()?QJsonValue(QJsonObject{{"from",layer.first()},{"to",layer.last()}}):QJsonValue()}};
   }
   deferred=true;
   if(name=="copyText")window->copyRecognizedText(params["all"].toBool(),replyText(done));
   else if(name=="copyCode")window->copyCode(params["index"].toInt(),replyText(done));
   else window->openCode(params["index"].toInt(),replyText(done));
   return {};
  }
  if(name=="apply"){
   if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");
   if(params.size()!=1||!params["commands"].isArray())return denied("invalid_request","apply needs only a commands array");
   // Quick redact (#1428): alone in its batch, checked before any recognition,
   // answering when the marks are added.
   {
    const auto commands=params["commands"].toArray();qsizetype quick=0;for(const auto& entry:commands)quick+=entry.toObject()["op"]=="redactText";
    if(quick){
     if(commands.size()!=1)return denied("edit_command_invalid","redactText must be the only command in its batch");
     const auto entry=commands.first().toObject();const auto list=entry["categories"].toArray();QStringList categories;
     if(entry.size()!=2||!entry["categories"].isArray()||list.isEmpty()||list.size()>5)return denied("edit_command_invalid","redactText takes categories: 1 to 5 of email, phone, ip, card and key");
     for(const auto& category:list){if(!category.isString()||!editor::quickRedact::categories().contains(category.toString())||categories.contains(category.toString()))return denied("edit_command_invalid","redactText takes categories: 1 to 5 of email, phone, ip, card and key");categories.append(category.toString());}
     deferred=true;
     window->quickRedact(categories,[&document,done](Error error,QJsonObject result){if(error){done({},error.code,error.message);return;}result["state"]=editor::Commands::state(*document);done(result,{},{});});
     return {};
    }
   }
   QJsonArray created;if(!editor::Commands::apply(*document,params["commands"].toArray(),created,code,message))return {};
   window->documentEdited();return {{"created",created},{"state",editor::Commands::state(*document)}};
  }
  if(name=="preview"){
   int edge=1024;
   if(!(params.isEmpty()||(params.size()==1&&params["maxEdge"].isDouble())))return denied("invalid_request","preview accepts only maxEdge");
   if(params.contains("maxEdge")){const double value=params["maxEdge"].toDouble();if(std::floor(value)!=value||value<64||value>editor::Commands::maximumPreviewEdge)return denied("invalid_request","maxEdge must be an integer from 64 to 1600");edge=int(value);}
   // The preview is drawn from the same sanitized render as Upload.
   auto image=document->render();if(image.isNull())return denied("edit_export_failed","The preview could not be rendered");
   const QSize source=image.size();if(image.width()>edge||image.height()>edge)image=image.scaled(edge,edge,Qt::KeepAspectRatio,Qt::SmoothTransformation);
   QByteArray png;{QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QImageWriter writer(&buffer,"png");if(!writer.write(image))return denied("edit_export_failed","The preview could not be encoded");}
   if(png.size()>6*1024*1024)return denied("preview_too_large","Request a smaller maxEdge");
   return {{"mime","image/png"},{"png",QString::fromLatin1(png.toBase64())},{"width",image.width()},{"height",image.height()},{"sourceWidth",source.width()},{"sourceHeight",source.height()}};
  }
  if(name=="saveCopy"){
   if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");
   const auto path=params["path"].toString();
   if(!params["path"].isString()||params.size()>2||(params.size()==2&&!params["overwrite"].isBool())||!QDir::isAbsolutePath(path)||!path.endsWith(".png",Qt::CaseInsensitive)||path.size()>4096)return denied("invalid_request","saveCopy needs an absolute .png path and optional overwrite boolean");
   const QFileInfo target(path);
   if(QFileInfo(target.absolutePath()).canonicalFilePath()==QFileInfo(files.directory()).canonicalFilePath())return denied("invalid_request","Choose a path outside private editor staging");
   if(target.exists()&&(!params["overwrite"].toBool()||!target.isFile()))return denied("file_exists","The file exists; pass overwrite to replace it");
   // The window's own Save a copy… path: pending edits, export, outcome shown.
   deferred=window->writeCopy(path,[path,done](const QByteArray& png,const QString& reason){
    if(png.isEmpty()){done({},"save_failed","The copy could not be saved: "+reason);return;}
    done({{"saved",true},{"path",path},{"bytes",double(png.size())},{"sha256",QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())}},{},{});
   });
   if(!deferred)return denied("editor_busy","A copy is still being saved");
   return {};
  }
  if(name=="copy"){
   // Copy image as Ctrl+Shift+C does: the sanitized PNG to the clipboard.
   if(!params.isEmpty())return denied("invalid_request","copy has no parameters");
   if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");
   deferred=startCopy([done](QJsonObject result,QString code,QString message){done(std::move(result),std::move(code),std::move(message));});
   if(!deferred)return denied("editor_busy","The image is still being copied or saved");
   return {};
  }
  if(name=="fileDialog"){
   // #259: Save a copy… as the button opens it (the desktop's chooser, or the
   // themed fallback), its state, or a cancel; editor.saveCopy saves to a path.
   const auto action=params["action"].toString();
   if(params.size()!=1||!QStringList{"open","inspect","cancel"}.contains(action))return denied("invalid_request","fileDialog needs action open, inspect or cancel");
   if(action=="open"){if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");if(window->fileDialog().isOpen())return denied("file_dialog_open","Save a copy is already open");window->saveCopy();}
   if(action=="cancel")window->fileDialog().cancel();
   return window->fileDialog().state();
  }
  if(name=="submit"){if(!params.isEmpty())return denied("invalid_request","submit has no parameters");if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");if(submissionError){submit();return denied(submissionError.code,submissionError.message);}QTimer::singleShot(0,&app,submit);return {{"submitted",true}};}
  if(name=="cancel"){if(!params.isEmpty())return denied("invalid_request","cancel has no parameters");if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");QTimer::singleShot(0,&app,[&]{if(!terminal&&!finalizing){window->setBusy("Discarding…");refuse("edit_cancelled");}});return {{"cancelled",true}};}
  return denied("invalid_request","Unknown editor command");
 };
 QObject::connect(&deadline,&QTimer::timeout,&app,fail);
 // Parent loss discards the session; nothing is exported without its acknowledgement.
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,fail);
 QObject::connect(&control,&UiControl::failed,&app,[&](Error){fail();});
 QObject::connect(&control,&UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& object){
  if(type=="bootstrap"&&!bootstrapped){
   nonce=object["nonce"].toString();const auto limits=object["limits"].toObject();destination=object["destination"].toString();
   const QUuid id(nonce);
   if(!recentColorList(object["recentColors"],recentColors)){fail();return;}
   const auto scheme=object["appearance"].toString();
   const auto submission=object["submissionError"].toObject();if(object.contains("submissionError")){if(submission.size()!=2||submission["code"]!="output_required"||!submission["message"].isString()){fail();return;}submissionError={submission["code"].toString(),submission["message"].toString(),{}};}
   // The saved text recognition languages (#1428); none means the locale's, then English.
   textLanguages.clear();for(const auto& language:object["textLanguages"].toArray())textLanguages.append(language.toString());
   if(!object["textLanguages"].isArray()||(!textLanguages.isEmpty()&&!textjob::validLanguages(textLanguages))){fail();return;}
   if(object.size()!=(object.contains("submissionError")?12:11)||(scheme!="dark"&&scheme!="light")||nonce.isEmpty()||id.isNull()||id.toString(QUuid::WithoutBraces)!=nonce||!validEnvelope(object,nonce)||object["build"].toObject()!=build
      ||limits.size()!=2||!number(limits["imagePixels"],maxPixels)||maxPixels<1||maxPixels>1000000000LL||!number(limits["outputBytes"],outputBytes)||outputBytes<1||outputBytes>maximumBytes
      ||!object["destination"].isString()||destination.isEmpty()||destination.size()>200||!object["input"].isObject()||!object["output"].isObject()
      ||!files.open(object["input"].toObject(),object["output"].toObject(),maxPixels*8+64*1024*1024)){fail();return;}
   // The interface's resolved scheme, before any window exists.
   putgd::Theme::setScheme(scheme=="light"?putgd::Theme::Scheme::Light:putgd::Theme::Scheme::Dark);
   bootstrapped=true;auto ready=envelope(nonce);ready["build"]=build;
   if(control.send("ready",ready).isEmpty())fail();
   return;
  }
  if(!bootstrapped||!validEnvelope(object,nonce)){fail();return;}
  if(type=="appearance"){
   const auto scheme=object["scheme"].toString();
   if(object.size()!=4||(scheme!="dark"&&scheme!="light")){fail();return;}
   Theme::setScheme(scheme=="light"?Theme::Scheme::Light:Theme::Scheme::Dark);return;
  }
  if(type=="copyResult"&&active&&window){
   // The resident's answer for the staged copy; copy.png is already gone.
   if(!object["ok"].isBool()||!object["copyId"].isString()||object.size()!=(object["ok"].toBool()?5:7)||(!object["ok"].toBool()&&(!object["code"].isString()||!object["message"].isString()))){fail();return;}
   if(auto text=textCopies.take(object["copyId"].toString())){text(object["ok"].toBool()?Error{}:Error{object["code"].toString().left(64),object["message"].toString().left(256),{}});return;}
   // An answer for a copy that already timed out here is dropped.
   if(!copyPending||object["copyId"].toString()!=copyId)return;
   if(!object["ok"].toBool()){copyFailed(object["code"].toString().left(64),object["message"].toString().left(256));return;}
   copyPending=false;copyDeadline.stop();copyId.clear();window->showCopied();
   if(auto reply=std::exchange(copyReply,{})){auto result=copyInfo;result["copied"]=true;reply(result,{},{});}
   return;
  }
  if(type!="command"&&object.size()!=3){fail();return;}
  if(type=="activate"&&!active&&!terminal){
   active=true;
   if(!files.intact()||!files.checkInput()){refuse("image_job_io");return;}
   // Static PNG or JPEG only, decoded once here with the file's orientation applied.
   // The format comes from the signature: a reader left to detect it instantiates
   // every installed image plugin (30 plugins, 12 MiB with KDE's kimageformats).
   const auto signature=files.input.peek(8);
   const QByteArray format=signature.startsWith("\x89PNG\r\n\x1a\n")?"png":signature.startsWith("\xff\xd8\xff")?"jpeg":"";
   if(format.isEmpty()){refuse("source_unsupported");return;}
   QImageReader reader(&files.input,format);reader.setAutoDetectImageFormat(false);reader.setAutoTransform(true);
   const auto size=reader.size();
   if(!size.isValid()||reader.supportsAnimation()||reader.imageCount()>1){refuse("source_unsupported");return;}
   const auto pixels=qint64(size.width())*size.height();
   if(pixels>maxPixels){refuse("input_excessive");return;}
   reader.setAllocationLimit(int(std::min<qint64>(pixels*4/(1024*1024)+2,INT_MAX)));
   const auto image=reader.read();if(image.isNull()){refuse("source_unsupported");return;}
   // Keep the pinned input descriptor open: final identity checks use it.
   document=std::make_unique<editor::EditDocument>(image);
   window=std::make_unique<EditorWindow>(*document);window->setRecentColors(recentColors);window->setDestination(destination);window->text().setLanguages(textLanguages);
   QObject::connect(window.get(),&EditorWindow::cancelRequested,&app,[&]{window->setBusy("Discarding…");refuse("edit_cancelled");});
   QObject::connect(window.get(),&EditorWindow::uploadRequested,&app,submit);
   QObject::connect(window.get(),&EditorWindow::copyRequested,&app,[&]{startCopy({});});
   window->setTextCopier([&](const QString& text,std::function<void(Error)> copied){
    if(terminal||finalizing){copied({"editor_finalizing","The edit was already submitted or discarded",{}});return;}
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);auto frame=envelope(nonce);frame["copyId"]=id;frame["text"]=text;
    if(control.send("copyText",frame).isEmpty()){copied({"editor_io","The text could not be handed to the clipboard",{}});fail();return;}
    textCopies.insert(id,std::move(copied));
   });
   deadline.stop();
   window->show();window->raise();window->activateWindow();
   if(control.send("editing",envelope(nonce)).isEmpty())fail();
   return;
  }
  if(type=="command"&&active&&window){
   const auto requestId=object["requestId"].toString();
   if(object.size()!=6||requestId.isEmpty()||requestId.size()>64||!object["command"].isString()||!object["params"].isObject()){fail();return;}
   const auto done=[&,requestId](QJsonObject result,QString code,QString message){
    auto reply=envelope(nonce);reply["requestId"]=requestId;reply["ok"]=code.isEmpty();
    if(code.isEmpty())reply["result"]=result;else{reply["code"]=code;reply["message"]=message.left(512);}
    if(control.send("commandResult",reply).isEmpty())fail();
   };
   QString code,message;bool deferred=false;
   auto result=command(object["command"].toString(),object["params"].toObject(),code,message,deferred,done);
   if(!deferred)done(result,code,message);
   return;
  }
  if(type=="checkpointAccepted"&&active&&terminal){
   if(window){window->allowClose();window->hide();}
   QTimer::singleShot(0,&app,&QCoreApplication::quit);return;
  }
  fail();
 });
 const auto exitCode=app.exec();control.stop();
 window.reset();document.reset();
 return exitCode;
#endif
}
}
