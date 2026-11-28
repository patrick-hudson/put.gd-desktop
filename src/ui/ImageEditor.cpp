#include "ImageEditor.h"
#include "core/ByteSize.h"
#include "EditorWindow.h"
#include "SensitivePrompt.h"
#include "FileDialog.h"
#include "WindowActivation.h"
#include "StagingDirectory.h"
#include "ipc/UiControl.h"
#include "core/BuildInfo.h"
#include "editor/EditDocument.h"
#include "editor/EditorCommands.h"
#include "editor/QuickRedact.h"
#include "editor/FalseAlarmReport.h"
#include "core/Settings.h"
#include <QJsonArray>
#include <QHash>
#include <QBuffer>
#include <QDir>
#include <QImageWriter>
#include <QLocale>
#include <QApplication>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QThread>
#include <QPointer>
#include <QUuid>
#include <memory>
#include <algorithm>
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
 // #2134: a false-alarm report's report.json and report-covered.png are written by writeReportAt() through
 // duplicateDirectory(); the resident reads, checks and removes them.
 void discardReport(){if(staging_.descriptor()>=0){::unlinkat(staging_.descriptor(),"report.json",0);::unlinkat(staging_.descriptor(),"report-covered.png",0);}}
 // A duplicate of the staging directory's descriptor for a report's worker (N5); -1 when staging isn't intact.
 int duplicateDirectory(){return intact()&&staging_.descriptor()>=0?::fcntl(staging_.descriptor(),F_DUPFD_CLOEXEC,0):-1;}
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
namespace {
// #2134 N5: a report file written through its own duplicate of the staging directory's descriptor, so the worker
// that writes it never touches the session's files and the session never waits for it. Returns the digest, or empty.
QString writeReportAt(int directory,const char* name,const QByteArray& bytes){
#ifdef Q_OS_UNIX
 if(directory<0||bytes.isEmpty())return {};
 const int fd=::openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd<0)return {};
 qint64 written=0;
 while(written<bytes.size()){const auto n=::write(fd,bytes.constData()+written,size_t(bytes.size()-written));if(n<0&&errno==EINTR)continue;if(n<=0)break;written+=n;}
 if(::fchmod(fd,0600)!=0||written!=bytes.size()||::fsync(fd)!=0){::close(fd);::unlinkat(directory,name,0);return {};}
 ::close(fd);
 return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
#else
 Q_UNUSED(directory);Q_UNUSED(name);Q_UNUSED(bytes);return {};
#endif
}
// #1820: the kinds covered automatically, each with its count; kinds with none are left out.
QJsonObject coveredJson(const QList<QPair<QString,int>>& covered){QJsonObject out;for(const auto& [category,count]:covered)if(count>0)out[category]=count;return out;}
}
int runImageEditor(QApplication& app,QIODevice& pipes){
#ifndef Q_OS_UNIX
 Q_UNUSED(app);Q_UNUSED(pipes);return 2;
#else
 UiControl control(pipes);StagedFiles files;
 std::unique_ptr<editor::EditDocument> document;std::unique_ptr<EditorWindow> window;
 // #1893: the look saved in Settings, and whether this document starts with it (a new capture).
 CaptureStyle savedStyle;bool hasStyle=false,styleStart=false;qint64 styleMaxPixels=0;
 Error submissionError;QString nonce,destination;bool links=false;QJsonObject encrypt;QStringList recentColors,textLanguages,redactTerms,redactPatterns;bool allCores=false,bootstrapped=false,active=false,terminal=false;qint64 maxPixels=0,outputBytes=0;
 // #1804: check mode. The capture is recognized and matched before any window shows;
 // nothing found ends the session at once, anything else asks (or, for an agent, waits for operation.decide).
 bool checkMode=false,checkPrompt=false,checkRedact=false,checking=false,deciding=false;QStringList checkCategories,checkCover;QString checkFile;
 // #1820: what the check covered on its own (redact kinds), counted by kind; the warn kinds are in findings.
 QList<QPair<QString,int>> covered;
 // #1941: the QR codes Keys and tokens covered, by kind alone, for the prompt's wording.
 QJsonObject codeKinds;
 SensitiveFindings findings;std::unique_ptr<SensitivePrompt> prompt;
 // #2134: Not sensitive. The check's Quick redact run and its covered image, frozen when the check finished
 // (or when Open in editor opened it), so later edits don't change what a report says the check did.
 editor::falseAlarm::Run checkRun;QImage checkedCovered;bool checkEditing=false;
 bool falseAlarmPending=false,falseAlarmSaved=false;QString falseAlarmId,falseAlarmName,falseAlarmLate;
 std::function<void(QJsonObject,QString,QString)> falseAlarmReply;
 // F7: the report is encoded, written and hashed off the GUI thread, from the frozen snapshot.
 QPointer<QThread> falseAlarmWorker;
 QTimer falseAlarmDeadline;falseAlarmDeadline.setSingleShot(true);falseAlarmDeadline.setInterval(60000);
 // An agent's decision waits this long; then the capture is treated as Cancel.
 QTimer decisionDeadline;decisionDeadline.setSingleShot(true);decisionDeadline.setInterval(5*60*1000);
 if(const auto test=qEnvironmentVariableIntValue("PUTGD_TEST_SENSITIVE_DECISION_MS");test>=100&&test<5*60*1000)decisionDeadline.setInterval(test);
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
   // #1893: the exported size, which a capture style makes larger than the crop.
   {const auto size=document->exportSize();width=size.width();height=size.height();png=document->exportPng();}
   if(png.isEmpty()){recover("edit_export_failed","Couldn't encode the edited image as PNG. The upload didn't start. Select Upload again, or select Save a copy… instead.");return;}
   if(png.size()>outputBytes){
    recover("input_excessive",QString("Edited image is %1, over the %2 limit. The upload didn't start. Crop it and select Upload again, or select Save a copy… instead.").arg(ByteSize::localized(png.size(),1),ByteSize::localized(outputBytes,1)));
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
 static constexpr qint64 copyBytes=128LL*1024*1024;
 bool copyPending=false;std::function<void(QJsonObject,QString,QString)> copyReply;QJsonObject copyInfo;QString copyId;
 // A copy the resident never answers fails here, so the window never stays busy.
 QTimer copyDeadline;copyDeadline.setSingleShot(true);copyDeadline.setInterval(30000);
 // Tests may only shorten it.
 if(const auto test=qEnvironmentVariableIntValue("PUTGD_TEST_EDITOR_COPY_DEADLINE_MS");test>=100&&test<30000)copyDeadline.setInterval(test);
 const auto copyFailed=[&](const QString& code,const QString& reason){
  copyPending=false;copyDeadline.stop();copyId.clear();files.discardCopy();window->showCopyFailed(reason);
  if(auto reply=std::exchange(copyReply,{}))reply({},code,"Couldn't copy the image: "+reason);
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
 std::function<bool(const QString&)> decide;std::function<void(bool)> handOffCovered;
 // #1898: Redact and upload covers what was found: offered whenever the check found something on an image it read.
 const auto redactOffered=[&]{return findings.any()&&document;};
 // #2134: Not sensitive saves a false-alarm report: offered for a check that found or covered something on an
 // image it read, while its prompt waits or in the editor it opened.
 const auto falseAlarmOffered=[&]{return checkMode&&document&&checkRun.valid&&(findings.any()||!coveredJson(covered).isEmpty());};
 const auto showFalseAlarm=[&](const QString& state,const QString& text){if(prompt)prompt->showFalseAlarm(state,text,falseAlarmName);if(window&&checkEditing)window->showFalseAlarm(state,text,falseAlarmName);};
 // The line always reads "Couldn't save the report. <reason>", whoever gave the reason.
 const auto falseAlarmFailed=[&](const QString& code,QString reason){
  falseAlarmPending=false;falseAlarmDeadline.stop();falseAlarmId.clear();files.discardReport();
  static const QString prefix=QStringLiteral("Couldn't save the report");
  reason=reason.trimmed();if(reason.startsWith(prefix))reason=reason.mid(prefix.size()).trimmed();while(reason.startsWith('.')||reason.startsWith(':'))reason=reason.mid(1).trimmed();
  if(!reason.isEmpty()&&!reason.endsWith('.'))reason+='.';
  const auto text=reason.isEmpty()?prefix+'.':prefix+". "+reason;
  showFalseAlarm("failed",text);
  if(auto reply=std::exchange(falseAlarmReply,{}))reply({},code,text);
 };
 const std::function<bool(std::function<void(QJsonObject,QString,QString)>,QString&,QString&)> reportFalseAlarm=[&](std::function<void(QJsonObject,QString,QString)> reply,QString& code,QString& message)->bool{
  if(terminal||finalizing){code="editor_finalizing";message="The capture was already decided";return false;}
  if(!falseAlarmOffered()||!(deciding||checkEditing)){code="false_alarm_unavailable";message="Not sensitive is offered only while a capture the sensitive text check flagged waits for a decision, or in the editor that check opened";return false;}
  if(falseAlarmSaved){code="false_alarm_already_saved";message="A report for this capture was already saved";return false;}
  if(falseAlarmPending){code="editor_busy";message="The report is still being saved";return false;}
  if(falseAlarmWorker){code="editor_busy";message="The report is still being saved";return false;}
  // N2: a report that timed out here is still outstanding: its identity stays, so its late answer is still taken.
  falseAlarmPending=true;falseAlarmReply=std::move(reply);falseAlarmId=QUuid::createUuid().toString(QUuid::WithoutBraces);
  showFalseAlarm("saving",QStringLiteral("Saving a report for diagnostics…"));
  // While the prompt waits the document is as the check left it; the editor's copy was frozen when it opened.
  if(checkedCovered.isNull())checkedCovered=document->render();
  // F7: everything heavy on a worker, from copies: the covered image (shared, read only), the check's run and counts.
  editor::falseAlarm::Input input;input.run=checkRun;input.image=document->base().size();
  input.findings=findings.json();input.covered=coveredJson(covered);input.codeKinds=codeKinds;input.categories=checkCategories;input.cover=checkCover;
  struct Prepared {QString json,covered;qint64 jsonBytes=0,coveredBytes=0;QString failure;};
  auto prepared=std::make_shared<Prepared>();
  const QImage image=checkedCovered;const auto id=falseAlarmId;
  // The `from` the helper sends is where the check is when the frame goes, decided on the GUI thread below; the JSON
  // says the same.
  const QString from=deciding?QStringLiteral("prompt"):QStringLiteral("editor");input.reportedFrom=from;
  const int directory=files.duplicateDirectory();
  if(directory<0){falseAlarmFailed("image_job_io",QStringLiteral("The editor's private staging isn't available."));return true;}
  // Tests may slow the preparation down, to cancel the capture while a report is still being prepared (N5).
  const int delayMs=std::clamp(qEnvironmentVariableIntValue("PUTGD_TEST_FALSE_ALARM_DELAY_MS"),0,60000);
  falseAlarmWorker=QThread::create([prepared,image,input,directory,delayMs]{
   if(delayMs)QThread::msleep(ulong(delayMs));
   QByteArray coveredPng;{QBuffer buffer(&coveredPng);buffer.open(QIODevice::WriteOnly);QImageWriter writer(&buffer,"png");if(!writer.write(image))coveredPng.clear();}
   if(coveredPng.isEmpty()){prepared->failure=QStringLiteral("The covered image couldn't be encoded.");::close(directory);return;}
   const auto json=QJsonDocument(editor::falseAlarm::build(input)).toJson(QJsonDocument::Compact);
   prepared->json=writeReportAt(directory,"report.json",json);prepared->covered=prepared->json.isEmpty()?QString():writeReportAt(directory,"report-covered.png",coveredPng);
   prepared->jsonBytes=json.size();prepared->coveredBytes=coveredPng.size();
   if(prepared->json.isEmpty()||prepared->covered.isEmpty())prepared->failure=QStringLiteral("Check free disk space and try again.");
   ::close(directory);
  });
  QObject::connect(falseAlarmWorker.data(),&QThread::finished,&app,[&,prepared,id,from,width=image.width(),height=image.height()]{
   if(falseAlarmWorker)falseAlarmWorker->deleteLater();
   falseAlarmWorker.clear();
   if(terminal||!falseAlarmPending||falseAlarmId!=id){files.discardReport();return;}
   if(!prepared->failure.isEmpty()){falseAlarmFailed(prepared->json.isEmpty()&&prepared->covered.isEmpty()&&prepared->jsonBytes==0?"edit_export_failed":"image_job_io",prepared->failure);return;}
   // F1: a decision that finalized meanwhile wins: the report goes only where the check still waits or edits.
   const QString now=deciding?QStringLiteral("prompt"):checkEditing&&!finalizing?QStringLiteral("editor"):QString();
   if(now.isEmpty()){falseAlarmFailed("editor_finalizing",QStringLiteral("The capture was already decided."));return;}
   if(now!=from){falseAlarmFailed("editor_finalizing",QStringLiteral("The capture moved on before the report was ready. Try again."));return;}
   auto frame=envelope(nonce);frame["reportId"]=id;frame["from"]=now;
   frame["json"]=QJsonObject{{"bytes",QString::number(prepared->jsonBytes)},{"sha256",prepared->json}};
   frame["covered"]=QJsonObject{{"bytes",QString::number(prepared->coveredBytes)},{"sha256",prepared->covered},{"width",width},{"height",height}};
   if(control.send("falseAlarm",frame).isEmpty()){fail();return;}
   falseAlarmDeadline.start();
  });
  falseAlarmWorker->start();
  return true;
 };
 // F3: a reply that comes after this still counts: the resident keeps one report per prompt and answers a retry with it.
 QObject::connect(&falseAlarmDeadline,&QTimer::timeout,&app,[&]{if(falseAlarmPending){falseAlarmLate=falseAlarmId;falseAlarmFailed("editor_timeout",QStringLiteral("It wasn't saved in time. Try again."));}});
 const auto command=[&](const QString& name,const QJsonObject& params,QString& code,QString& message,bool& deferred,std::function<void(QJsonObject,QString,QString)> done)->QJsonObject{
  auto denied=[&](const QString& c,const QString& m){code=c;message=m;return QJsonObject{};};
  if(deciding){
   // #1804, #1898: the capture waits for Redact and upload, Open in editor, Upload anyway or Cancel; the resident sends only decide.
   if(name=="decide"){
    const auto choice=params["choice"].toString();
    if(params.size()!=1||!QStringList{"redact_upload","edit","upload","cancel"}.contains(choice))return denied("invalid_request","decide needs choice redact_upload, edit, upload or cancel");
    if(terminal)return denied("editor_finalizing","The capture was already decided");
    if(choice=="redact_upload"&&!redactOffered())return denied("sensitive_choice_unavailable","Redact and upload is not offered for this capture: nothing was found to cover");
    if(choice=="edit"&&!(checkRedact&&window))return denied("sensitive_choice_unavailable","Open in editor is not offered for this capture");
    if(!decide(choice))return denied("editor_finalizing","The capture was already decided");
    return {{"decided",choice}};
   }
   // #2134: Not sensitive, as the panel's button: the decision still waits.
   if(name=="reportFalseAlarm"){
    if(!params.isEmpty())return denied("invalid_request","reportFalseAlarm has no parameters");
    deferred=reportFalseAlarm(done,code,message);return {};
   }
   // The tray or a notification brings the panel forward (#1804): it is an ordinary window another can cover.
   if(name=="raisePrompt"&&prompt){
    const auto token=params["activationToken"].toString();
    if(!(params.isEmpty()||(params.size()==1&&validActivationToken(token))))return denied("invalid_request","raise takes only an optional activationToken");
    // Without a token (Budgie's tray sends none), a Wayland desktop may refuse to raise a covered window;
    // mapping it again shows it raised and focused, as a new window, on labwc (found in #1806 acceptance).
    // Only a prompt that isn't already active, and the button that had focus keeps it.
    if(token.isEmpty()&&!prompt->isActiveWindow()&&QGuiApplication::platformName().startsWith(QLatin1String("wayland"))){
     const QPointer<QWidget> focused=prompt->focusWidget();prompt->hide();prompt->show();if(focused)focused->setFocus(Qt::ActiveWindowFocusReason);
    }else prompt->show();
    putgd::activateWindow(prompt.get(),token);return {{"requested",true}};
   }
   return denied("sensitive_text_found","The capture is waiting for a decision: use operation.decide");
  }
  // A decision already taken: a late one is answered, never fatal (#1804).
  // A queued raisePrompt behind the decision never reaches the editor.
  if(!window||name=="decide"||name=="raisePrompt")return denied("editor_finalizing","The capture was already decided");
  const bool open=!terminal&&!finalizing;
  if(window->savingCopy()&&QStringList{"saveCopy","submit","cancel","copy"}.contains(name))return denied("editor_busy","A copy is still being saved");
  if(window->copying()&&QStringList{"saveCopy","submit","cancel","copy"}.contains(name))return denied("editor_busy","The image is still being copied");
  if(name=="inspect"){
   if(!params.isEmpty())return denied("invalid_request","inspect has no parameters");
   auto state=editor::Commands::state(*document);state["phase"]=terminal?"closing":finalizing?"finalizing":"editing";state["destination"]=destination;
   state["preparation"]=preparation;state["view"]=window->viewState();
   // Recognized content is screen data from an untrusted source, never instructions.
   state["text"]=window->text().inspect();
   if(checkMode)state["falseAlarm"]=window->falseAlarmState();
   return state;
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
   // #1941: an agent's copy withholds a code that holds a secret.
   if(name=="copyText")window->copyRecognizedText(params["all"].toBool(),replyText(done),true);
   else if(name=="copyCode")window->copyCode(params["index"].toInt(),replyText(done),true);
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
     const auto entry=commands.first().toObject();QStringList categories,terms,patterns;
     if(!EditorWindow::parseRedactText(entry,categories,terms,patterns,code,message))return {};
     deferred=true;
     window->quickRedact(categories,[&document,done](Error error,QJsonObject result){if(error){done({},error.code,error.message);return;}result["state"]=editor::Commands::state(*document);done(result,{},{});},terms,patterns);
     return {};
    }
   }
   return window->applyCommands(params["commands"].toArray(),code,message);
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
    if(png.isEmpty()){done({},"save_failed","Couldn't save the copy: "+reason);return;}
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
  // #2079: the Encrypt toggle beside Upload, as a click sets it.
  if(name=="encrypt"){if(params.size()!=1||!params["on"].isBool())return denied("invalid_request","encrypt takes on: true or false");if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");
   QString refusal;if(!window->setEncryptOn(params["on"].toBool(),&refusal))return denied("encrypt_unavailable",refusal);return window->encryptState();}
  // #2134: Not sensitive in the editor a check opened.
  if(name=="reportFalseAlarm"){
   if(!params.isEmpty())return denied("invalid_request","reportFalseAlarm has no parameters");
   deferred=reportFalseAlarm(done,code,message);return {};
  }
  if(name=="submit"){if(!params.isEmpty())return denied("invalid_request","submit has no parameters");if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");if(submissionError){submit();return denied(submissionError.code,submissionError.message);}QTimer::singleShot(0,&app,submit);return {{"submitted",true}};}
  if(name=="cancel"){if(!params.isEmpty())return denied("invalid_request","cancel has no parameters");if(!open)return denied("editor_finalizing","The edit was already submitted or discarded");QTimer::singleShot(0,&app,[&]{if(!terminal&&!finalizing){window->setBusy("Discarding…");refuse("edit_cancelled");}});return {{"cancelled",true}};}
  return denied("invalid_request","Unknown editor command");
 };
 // #1820: the capture with only the automatic covers, handed off for upload like an edit, without a window.
 // #1898: with `everything` (Redact and upload), every suggestion stays: exactly the covers the prompt's preview
 // shows, and the counts name the warn kinds found as well as the automatic covers.
 handOffCovered=[&](bool everything){
  if(terminal||finalizing||!document)return;
  if(everything){for(const auto& [category,count]:std::as_const(findings.found))if(count>0)covered.append({category,count});}
  else{QStringList warn;for(const auto& category:std::as_const(checkCategories))if(!checkCover.contains(category))warn<<category;document->removeSuggested(warn);}
  finalizing=true;
  auto frame=envelope(nonce);if(everything)frame["redacted"]=true;
  if(control.send("finalizing",frame).isEmpty()){fail();return;}
  QTimer::singleShot(0,&app,[&]{
   if(terminal)return;
   int width=0,height=0;QByteArray png;
   // #1893: the exported size, which a capture style makes larger than the crop.
   {const auto size=document->exportSize();width=size.width();height=size.height();png=document->exportPng();}
   if(png.isEmpty()){refuse("edit_export_failed");return;}
   if(png.size()>outputBytes){refuse("input_excessive");return;}
   const auto digest=files.write(png);const auto bytes=png.size();png.clear();png.squeeze();
   if(digest.isEmpty()){refuse("image_job_io");return;}
   auto result=envelope(nonce);result["ok"]=true;result["bytes"]=QString::number(bytes);result["sha256"]=digest;result["width"]=width;result["height"]=height;result["covered"]=coveredJson(covered);
   finish(result);
  });
 };
 // #1804: what the check found reaches the resident as kinds and counts only; then the prompt, or an agent's decision.
 std::function<void()> checked=[&]{
  deciding=true;
  // #2134: what the check matched, for Not sensitive.
  if(window)checkRun=window->lastQuickRedact();auto frame=envelope(nonce);const auto json=findings.json();frame["found"]=json["found"];frame["complete"]=findings.complete;
  if(!findings.complete)frame["reason"]=findings.reason;
  if(const auto counts=coveredJson(covered);!counts.isEmpty())frame["covered"]=counts;
  if(control.send("checked",frame).isEmpty()){fail();return;}
  if(!checkPrompt){decisionDeadline.start();return;}
  SensitivePrompt::Model model;model.findings=findings;model.covered=covered;model.redactAllowed=redactOffered();model.editAllowed=checkRedact&&window;model.destination=destination;model.file=checkFile;model.codes=codeKinds;
  model.falseAlarmAllowed=falseAlarmOffered();
  // The capture as Redact and upload sends it and Open in editor opens it: every suggestion painted black.
  if(document){model.preview=document->render();checkedCovered=model.preview;}
  prompt=std::make_unique<SensitivePrompt>(std::move(model));
  QObject::connect(prompt.get(),&SensitivePrompt::chosen,&app,[&](const QString& choice){decide(choice);});
  QObject::connect(prompt.get(),&SensitivePrompt::falseAlarmRequested,&app,[&]{QString code,message;reportFalseAlarm({},code,message);});
  prompt->show();prompt->raise();prompt->activateWindow();
 };
 decide=[&](const QString& choice)->bool{
  if(!deciding||terminal)return false;
  if(choice=="redact_upload"&&!redactOffered())return false;
  if(choice=="edit"&&!(checkRedact&&window))return false;
  deciding=false;decisionDeadline.stop();
  if(prompt){prompt->choose(choice);prompt->hide();}
  // #1898: Redact and upload: every suggestion covered, handed off for upload with no editor step.
  if(choice=="redact_upload"){handOffCovered(true);return true;}
  // #1820: Upload anyway keeps what was covered automatically: the capture goes with those covers, the warn suggestions taken off.
  if(choice=="upload"&&!coveredJson(covered).isEmpty()&&document){handOffCovered(false);return true;}
  if(choice=="upload"){refuse("check_upload");return true;}
  if(choice=="cancel"){refuse("check_cancel");return true;}
  // Open in editor: the same document, its suggestions already placed, in the ordinary editor.
  // #2134: the covered image as the check left it, before the style or any edit; Not sensitive goes on offer there.
  if(falseAlarmOffered()){
   if(checkedCovered.isNull())checkedCovered=document->render();
   checkEditing=true;window->offerFalseAlarm(true);
   if(falseAlarmSaved)window->showFalseAlarm("saved",QStringLiteral("Report saved for diagnostics."),falseAlarmName);
   else if(falseAlarmPending)window->showFalseAlarm("saving",QStringLiteral("Saving a report for diagnostics…"));
  }
  if(hasStyle&&styleStart){auto style=savedStyle;style.enabled=true;document->setInitialStyle(style);window->documentEdited();}
  window->selectTool(editor::Tool::Redact);
  window->show();window->raise();window->activateWindow();
  if(control.send("editing",envelope(nonce)).isEmpty())fail();
  return true;
 };
 QObject::connect(&decisionDeadline,&QTimer::timeout,&app,[&]{if(deciding&&!terminal){deciding=false;refuse("check_expired");}});
 QObject::connect(&deadline,&QTimer::timeout,&app,fail);
 // Parent loss discards the session; nothing is exported without its acknowledgement.
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,fail);
 QObject::connect(&control,&UiControl::failed,&app,[&](Error){fail();});
 QObject::connect(&control,&UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& object){
  if(type=="bootstrap"&&!bootstrapped){
   nonce=object["nonce"].toString();const auto limits=object["limits"].toObject();destination=object["destination"].toString();links=object["links"].toBool(false);
   // #2079: the Encrypt toggle's start, {available, on, reason?}; absent without an upload route.
   encrypt=object["encrypt"].toObject();if(object.contains("encrypt")&&(!object["encrypt"].isObject()||!encrypt["available"].isBool()||!encrypt["on"].isBool()||(encrypt.contains("reason")&&!encrypt["reason"].isString())||encrypt.size()>3)){fail();return;}
   const QUuid id(nonce);
   if(!recentColorList(object["recentColors"],recentColors)){fail();return;}
   const auto scheme=object["appearance"].toString();
   const auto submission=object["submissionError"].toObject();if(object.contains("submissionError")){if(submission.size()!=2||submission["code"]!="output_required"||!submission["message"].isString()){fail();return;}submissionError={submission["code"].toString(),submission["message"].toString(),{}};}
   // The saved text recognition languages (#1428); none means the locale's, then English.
   textLanguages.clear();for(const auto& language:object["textLanguages"].toArray())textLanguages.append(language.toString());
   if(!object["textLanguages"].isArray()||(!textLanguages.isEmpty()&&!textjob::validLanguages(textLanguages))){fail();return;}
   // Unhinged mode (#1937), present only when on.
   if(object.contains("allCores")&&!object["allCores"].isBool()){fail();return;}allCores=object["allCores"].toBool(false);
   // The saved Quick redact words and patterns (#1618), present only when not empty.
   {redactTerms.clear();redactPatterns.clear();
    const auto read=[&](const char* key,QStringList& into,int most,QString(*error)(const QString&)){
     if(!object.contains(key))return true;const auto list=object[key].toArray();
     if(!object[key].isArray()||list.isEmpty()||list.size()>most)return false;
     for(const auto& entry:list){if(!entry.isString()||!error(entry.toString()).isEmpty())return false;into.append(entry.toString());}
     return true;};
    if(!read("redactTerms",redactTerms,textjob::maxRedactTerms,textjob::redactTermError)||!read("redactPatterns",redactPatterns,textjob::maxRedactPatterns,Settings::quickRedactPatternError)){fail();return;}}
   // #1893: {saved, start}: the Settings look, and whether the document starts styled with it.
   if((hasStyle=object.contains("style"))){
    const auto style=object["style"].toObject();
    if(!object["style"].isObject()||style.size()!=3||!style["start"].isBool()||CaptureStyle::validate(style["saved"],"style")||!number(style["maxPixels"],styleMaxPixels)||styleMaxPixels<1){fail();return;}
    savedStyle=CaptureStyle::fromJson(style["saved"].toObject());styleStart=style["start"].toBool();
   }
   // #1804: check mode, its categories, whether a person is asked and whether Redact and upload is offered.
   if(object.contains("check")){
    const auto check=object["check"].toObject();checkCategories.clear();
    if(!object["check"].isObject()||check.size()!=3+(check.contains("file")?1:0)+(check.contains("cover")?1:0)||!check["prompt"].isBool()||!check["redact"].isBool()||!check["categories"].isArray()||(check.contains("file")&&(!check["file"].isString()||check["file"].toString().size()>300))){fail();return;}
    for(const auto& category:check["categories"].toArray()){if(!category.isString()||!editor::quickRedact::categories().contains(category.toString())||checkCategories.contains(category.toString())){fail();return;}checkCategories.append(category.toString());}
    if(checkCategories.isEmpty()){fail();return;}
    // #1820: the kinds covered without asking, each one of the categories.
    checkCover.clear();
    if(check.contains("cover")){
     if(!check["cover"].isArray()||check["cover"].toArray().isEmpty()){fail();return;}
     for(const auto& category:check["cover"].toArray()){if(!category.isString()||!checkCategories.contains(category.toString())||checkCover.contains(category.toString())){fail();return;}checkCover.append(category.toString());}
    }
    checkMode=true;checkPrompt=check["prompt"].toBool();checkRedact=check["redact"].toBool();checkFile=check["file"].toString();
   }
   if((object.contains("links")&&!object["links"].isBool())||object.size()!=11+(object.contains("links")?1:0)+(object.contains("encrypt")?1:0)+(object.contains("submissionError")?1:0)+(object.contains("redactTerms")?1:0)+(object.contains("redactPatterns")?1:0)+(object.contains("allCores")?1:0)+(checkMode?1:0)+(hasStyle?1:0)||(scheme!="dark"&&scheme!="light")||nonce.isEmpty()||id.isNull()||id.toString(QUuid::WithoutBraces)!=nonce||!validEnvelope(object,nonce)||object["build"].toObject()!=build
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
  // #2134: the resident's answer for a false-alarm report; its staged files are already gone.
  if(type=="reportResult"&&active){
   const bool ok=object["ok"].toBool();
   if(!object["ok"].isBool()||!object["reportId"].isString()||object.size()!=7||(ok&&(!object["name"].isString()||!object["bytes"].isString()))||(!ok&&(!object["code"].isString()||!object["message"].isString()))){fail();return;}
   const auto answered=object["reportId"].toString();
   // F3: a report saved after this side gave up waiting is still the report: it is taken as saved.
   if(!falseAlarmLate.isEmpty()&&answered==falseAlarmLate){
    falseAlarmLate.clear();
    if(!ok||falseAlarmSaved)return;
    // N2: it saved after all. A retry waiting meanwhile is answered with it too.
    falseAlarmSaved=true;falseAlarmName=object["name"].toString().left(128);
    if(falseAlarmPending){falseAlarmPending=false;falseAlarmDeadline.stop();falseAlarmId.clear();if(auto reply=std::exchange(falseAlarmReply,{}))reply({{"saved",true},{"name",falseAlarmName},{"bytes",object["bytes"]}},{},{});}
    showFalseAlarm("saved",QStringLiteral("Report saved for diagnostics."));return;
   }
   if(!falseAlarmPending||answered!=falseAlarmId)return;
   if(!ok){falseAlarmFailed(object["code"].toString().left(64),object["message"].toString().left(256));return;}
   falseAlarmPending=false;falseAlarmSaved=true;falseAlarmDeadline.stop();falseAlarmId.clear();falseAlarmName=object["name"].toString().left(128);
   showFalseAlarm("saved",QStringLiteral("Report saved for diagnostics."));
   if(auto reply=std::exchange(falseAlarmReply,{}))reply({{"saved",true},{"name",falseAlarmName},{"bytes",object["bytes"]}},{},{});
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
   if((!size.isValid()||reader.supportsAnimation()||reader.imageCount()>1)&&checkMode){deadline.stop();findings.complete=false;findings.reason="recognition_failed";checkRedact=false;checked();return;}
   if(!size.isValid()||reader.supportsAnimation()||reader.imageCount()>1){refuse("source_unsupported");return;}
   const auto pixels=qint64(size.width())*size.height();
   // #1804: an image too large to read can't be checked, and that is said, never passed over.
   if(pixels>maxPixels&&checkMode){deadline.stop();findings.complete=false;findings.reason="too_large";checkRedact=false;checked();return;}
   if(pixels>maxPixels){refuse("input_excessive");return;}
   // A check also opens 16-bit PNG files from a file manager: 8 bytes a pixel, inside the same pixel admission.
   reader.setAllocationLimit(int(std::min<qint64>(pixels*(checkMode?8:4)/(1024*1024)+2,INT_MAX)));
   const auto image=reader.read();
   if(image.isNull()&&checkMode){deadline.stop();findings.complete=false;findings.reason="recognition_failed";checkRedact=false;checked();return;}
   if(image.isNull()){refuse("source_unsupported");return;}
   // Keep the pinned input descriptor open: final identity checks use it.
   document=std::make_unique<editor::EditDocument>(image);
   window=std::make_unique<EditorWindow>(*document);window->setRecentColors(recentColors);window->setDestination(destination,links);window->setEncrypt(encrypt);
   // #2079: each change of the Encrypt toggle tells the resident, which applies it when the upload starts.
   QObject::connect(window.get(),&EditorWindow::encryptToggled,&app,[&](bool on){auto frame=envelope(nonce);frame["on"]=on;if(control.send("encrypt",frame).isEmpty())fail();});window->text().setLanguages(textLanguages);window->text().setAllCores(allCores);window->setRedactList(redactTerms,redactPatterns);
   // #1893: a new capture starts with the saved look on; a check's hidden document starts plain, so its own
   // covered export is the capture as it was (the resident styles it), and Redact and upload turns the look on.
   if(hasStyle){document->setStyleLimit(styleMaxPixels);window->setSavedStyle(savedStyle);if(styleStart&&!checkMode){auto style=savedStyle;style.enabled=true;document->setInitialStyle(style);window->documentEdited();}}
   QObject::connect(window.get(),&EditorWindow::cancelRequested,&app,[&]{window->setBusy("Discarding…");refuse("edit_cancelled");});
   QObject::connect(window.get(),&EditorWindow::uploadRequested,&app,submit);
   QObject::connect(window.get(),&EditorWindow::copyRequested,&app,[&]{startCopy({});});
   // #2134: Not sensitive in the editor the check opened, as the panel's button.
   QObject::connect(window.get(),&EditorWindow::falseAlarmRequested,&app,[&]{QString code,message;reportFalseAlarm({},code,message);});
   window->setTextCopier([&](const QString& text,std::function<void(Error)> copied){
    if(terminal||finalizing){copied({"editor_finalizing","The edit was already submitted or discarded",{}});return;}
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);auto frame=envelope(nonce);frame["copyId"]=id;frame["text"]=text;
    if(control.send("copyText",frame).isEmpty()){copied({"editor_io","The clipboard request couldn't be sent.",{}});fail();return;}
    textCopies.insert(id,std::move(copied));
   });
   deadline.stop();
   if(checkMode){
    // Recognition and Quick redact on the hidden editor, with the editor's own helper, deadline and limits.
    checking=true;
    // Adding the marks starts a new recognition revision, which clears the result: read whether it was cut first.
    auto truncated=std::make_shared<bool>(false);auto languageMissing=std::make_shared<bool>(false);
    window->text().whenRecognized([&,truncated,languageMissing](Error error){if(!error){*truncated=window->text().truncated();*languageMissing=!window->text().languagesSkipped().isEmpty();}});
    // #1820: the kinds covered automatically are matched first, so another kind never takes their words.
    window->quickRedact(checkCategories,[&,truncated,languageMissing](Error error,QJsonObject result){
     checking=false;if(terminal)return;
     findings={};covered.clear();codeKinds=result["codes"].toObject();
     const auto counts=result["counts"].toObject();
     // #1820: a redact kind's matches are covered already; only the warn kinds are left to decide.
     for(const auto& category:checkCategories)(checkCover.contains(category)?covered:findings.found).append({category,counts[category].toInt()});
     static const QStringList unavailable{"text_helper_missing","engine_unavailable","helper_sandbox_unavailable","language_unavailable"};
     if(error){findings.complete=false;findings.reason=unavailable.contains(error.code)?QStringLiteral("unavailable"):QStringLiteral("recognition_failed");}
     else if(*truncated){findings.complete=false;findings.reason="truncated";}
     else if(result["unfinished"].toBool()){findings.complete=false;findings.reason="matching_unfinished";}
     else if(*languageMissing){findings.complete=false;findings.reason="language_missing";}
     else if(!result["skippedPatterns"].toArray().isEmpty()){findings.complete=false;findings.reason="patterns_skipped";}
     else if(result["nameUnchecked"].toBool()){findings.complete=false;findings.reason="name_unchecked";}
     // Nothing found, all of it read: the upload goes ahead as if there were no check.
     if(findings.complete&&!findings.any()){
      // Covered automatically with nothing left to ask about: the covered capture uploads, no prompt (#1820).
      if(!coveredJson(covered).isEmpty()){handOffCovered(false);return;}
      refuse("check_clean");return;
     }
     checked();
    },{},{},checkCover);
    return;
   }
   window->show();window->raise();window->activateWindow();
   if(control.send("editing",envelope(nonce)).isEmpty())fail();
   return;
  }
  if(type=="command"&&active){
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
   if(prompt)prompt->hide();
   if(window){window->allowClose();window->hide();}
   QTimer::singleShot(0,&app,&QCoreApplication::quit);return;
  }
  fail();
 });
 const auto exitCode=app.exec();control.stop();
 // N5: a report still being prepared never holds the session up: its worker owns its own copies and its own
 // descriptor, and is left to end with the process; a report not handed over is not saved.
 if(falseAlarmWorker){falseAlarmWorker->disconnect();(void)falseAlarmWorker.data();}
 prompt.reset();window.reset();document.reset();
 return exitCode;
#endif
}
}
