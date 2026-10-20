#include "DiagnosticsBundle.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include "core/ZipWriter.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageAuthenticationCode>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QSysInfo>
#include <algorithm>
namespace putgd {
namespace {
const QStringList nameKeys{"name","candidate","fileName","folderName","label","notificationText"};
const QStringList urlKeys{"url","directUrl","publicUrl","publicUrlBase","link","shareUrl"};
const QStringList objectKeys{"key","objectKey","objectPrefix"};
const QStringList idKeys{"accountRootUid","folderUid","shareUid","linkUid","volumeId","nodeUid"};
const QStringList pathKeys{"path","localPath","previewPath","directory","stagingPath","root","from","to","modulePath","configPath"};
QString iso(const QDateTime& time) {return time.toUTC().toString(Qt::ISODateWithMs);}
QByteArray compact(const QJsonValue& value) {return value.isArray()?QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented):QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented);}
QString sha256(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QByteArray readBounded(const QString& path,qint64 limit) {QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.read(limit);}
QJsonObject osRelease() {
 QJsonObject result;
 for(const auto& line:readBounded("/etc/os-release",65536).split('\n')) {
  const auto separator=line.indexOf('=');if(separator<1)continue;
  const auto key=QString::fromUtf8(line.left(separator));auto value=QString::fromUtf8(line.mid(separator+1)).trimmed();
  if(value.size()>=2&&(value.startsWith('"')||value.startsWith('\''))&&value.endsWith(value.front()))value=value.mid(1,value.size()-2);
  if(QStringList{"ID","ID_LIKE","VERSION_ID","VERSION_CODENAME","VARIANT_ID"}.contains(key))result[key]=value.left(128);
 }
 return result;
}
qint64 memoryTotal() {
 for(const auto& line:readBounded("/proc/meminfo",65536).split('\n'))
  if(line.startsWith("MemTotal:"))return line.mid(9).trimmed().split(' ').first().toLongLong()*1024;
 return -1;
}
const QRegularExpression& packageName() {static const QRegularExpression value("^putgd-diagnostics-\\d{8}-\\d{6}Z(-\\d+)?\\.zip$");return value;}
QString uniqueName(const QString& folder,const QDateTime& now) {
 const auto base="putgd-diagnostics-"+now.toUTC().toString("yyyyMMdd-HHmmss")+"Z";
 auto path=QDir(folder).filePath(base+".zip");
 for(int index=2;QFileInfo::exists(path)&&index<1000;++index)path=QDir(folder).filePath(QString("%1-%2.zip").arg(base).arg(index));
 return path;
}
QJsonObject itemSummary(const OperationItem& item,int attempts) {
 QJsonObject result{{"id",item.artifact.id},{"mime",item.artifact.mime},{"category",item.artifact.category},{"bytes",item.artifact.bytes},
  {"name",item.artifact.name},{"candidate",item.candidate},{"uploadStatus",item.uploadStatus},{"phase",item.phase},
  {"local",QJsonObject{{"status",item.local.status},{"path",item.local.path},{"error",item.local.error.toJson()}}},
  {"shareStatus",item.shareStatus},{"remoteStatus",item.remoteStatus},{"attempts",attempts}};
 if(item.error)result["error"]=item.error.toJson();
 if(!item.receipt.directUrl.isEmpty())result["directUrl"]=item.receipt.directUrl;
 if(item.objectReference.contains("key"))result["objectKey"]=item.objectReference["key"];
 return result;
}
}
PackageRedactor::PackageRedactor(bool includeNames):names_(includeNames) {
 key_.resize(32);
 for(qsizetype index=0;index<key_.size();++index)key_[index]=char(QRandomGenerator::system()->bounded(256));
}
void PackageRedactor::root(const QString& path,const QString& placeholder) {
 const auto clean=QDir::cleanPath(path);
 if(path.isEmpty()||clean=="/"||!QDir::isAbsolutePath(clean))return;
 for(const auto& existing:roots_)if(existing.path==clean)return;
 roots_.append({clean,placeholder,QRegularExpression(QRegularExpression::escape(clean)+"(?=$|[/\\s\"'<>,;:)\\]}])")});
 std::sort(roots_.begin(),roots_.end(),[](const Root& a,const Root& b){return a.path.size()>b.path.size();});
}
void PackageRedactor::literal(const QString& kind,const QString& value) {
 if(names_||value.size()<4||literals_.contains(value))return;
 literals_.insert(value,token(kind,value));compiled_=false;
}
QString PackageRedactor::token(const QString& kind,const QString& value) const {
 const auto digest=QMessageAuthenticationCode::hash((kind+'\n'+value).toUtf8(),key_,QCryptographicHash::Sha256).toHex().left(12);
 QString suffix;
 if(kind=="name"||kind=="key"||kind=="path") {
  static const QRegularExpression extension("^[A-Za-z0-9]{1,8}$");
  const auto candidate=QFileInfo(value).suffix();if(extension.match(candidate).hasMatch())suffix="."+candidate;
 }
 return "<"+kind+":"+QString::fromLatin1(digest)+suffix+">";
}
void PackageRedactor::compile() const {
 if(compiled_)return;
 auto values=literals_.keys();std::sort(values.begin(),values.end(),[](const QString& a,const QString& b){return a.size()>b.size();});
 QStringList escaped;for(const auto& value:values)escaped.append(QRegularExpression::escape(value));
 literalPattern_=QRegularExpression(escaped.isEmpty()?QStringLiteral("(?!)"):escaped.join('|'));
 compiled_=true;
}
// Known folders become placeholders; in hashed mode the rest of a path under
// the home folder or the archive becomes one token.
QString PackageRedactor::paths(QString text) const {
 for(const auto& root:roots_)if(text.contains(root.path))text.replace(root.pattern,root.placeholder);
 if(!names_) {
  static const QRegularExpression below("(<home>|<archive>)/([^\\s\"'<>,;)\\]}]+)");
  QString result;qsizetype last=0;
  for(auto it=below.globalMatch(text);it.hasNext();) {
   const auto match=it.next();
   result+=text.mid(last,match.capturedStart()-last)+match.captured(1)+"/"+token("path",match.captured(2));last=match.capturedEnd();
  }
  text=result+text.mid(last);
 }
 return text;
}
QString PackageRedactor::text(const QString& input) const {
 auto text=paths(logging::redact(input));
 if(names_)return text;
 compile();
 if(!literals_.isEmpty()) {
  QString result;qsizetype last=0;
  for(auto it=literalPattern_.globalMatch(text);it.hasNext();) {const auto match=it.next();result+=text.mid(last,match.capturedStart()-last)+literals_.value(match.captured());last=match.capturedEnd();}
  text=result+text.mid(last);
 }
 static const QRegularExpression url("(?i)\\b(?:https?|wss?|s3)://[^\\s\"'<>]+");
 QString result;qsizetype last=0;
 for(auto it=url.globalMatch(text);it.hasNext();) {
  const auto match=it.next();auto value=match.captured();
  // Trailing punctuation belongs to the sentence, not the URL.
  while(!value.isEmpty()&&QStringLiteral(".,;:)]}").contains(value.back()))value.chop(1);
  // put.gd's own release links identify a build, not a user.
  const auto replacement=value.startsWith("https://github.com/patrick-hudson/")?value:token("url",value);
  result+=text.mid(last,match.capturedStart()-last)+replacement;last=match.capturedStart()+value.size();
 }
 return result+text.mid(last);
}
QJsonValue PackageRedactor::json(const QJsonValue& value,const QString& key) const {
 if(value.isString()) {
  const auto string=value.toString();
  if(string.isEmpty())return value;
  if(!names_) {
   if(nameKeys.contains(key))return token("name",string);
   if(urlKeys.contains(key))return token("url",string);
   if(objectKeys.contains(key))return token("key",string);
   if(idKeys.contains(key))return token("id",string);
   if(key=="bucket")return token("bucket",string);
   if(key=="endpoint")return token("endpoint",string);
  }
  return text(string);
 }
 if(value.isArray()) {QJsonArray result;for(const auto& entry:value.toArray())result.append(json(entry,key));return result;}
 if(value.isObject()) {
  const auto object=value.toObject();
  // A credential reference names a keychain entry; keep only how it is stored.
  if(key=="credentials"||key=="sharePasswordReference")return QJsonObject{{"mode",object["mode"].toString("<reference>")}};
  QJsonObject result;
  for(auto it=object.begin();it!=object.end();++it)result[it.key()]=json(it.value(),it.key());
  return result;
 }
 if(key=="sharePasswordReference"&&!value.isNull())return QStringLiteral("<reference>");
 return value;
}
DiagnosticsBundles::DiagnosticsBundles(OperationService& operations,QString stateRoot,QObject* parent):QObject(parent),operations_(operations),stateRoot_(std::move(stateRoot)) {}
QString DiagnosticsBundles::defaultFolder(const Settings& settings) {return QDir(settings.archive.root).filePath("diagnostics");}
Error DiagnosticsBundles::parse(const QJsonObject& params,BundleRequest& request) {
 const Error invalid{"invalid_request","diagnostics.bundle takes operation (an ID), since (1m to 365d, such as 7d or 24h), includeNames, output (an absolute .zip path or folder), preview, and upload with profileId and copyLink",{}};
 for(auto it=params.begin();it!=params.end();++it)if(!QStringList{"operation","since","includeNames","output","preview","upload","profileId","copyLink"}.contains(it.key()))return invalid;
 if(params.contains("upload")){if(!params["upload"].isBool())return invalid;request.upload=params["upload"].toBool();}
 if(params.contains("copyLink")){if(!params["copyLink"].isBool()||!request.upload)return invalid;request.copyLink=params["copyLink"].toBool();}
 if(params.contains("profileId")){const auto id=params["profileId"].toString();if(!params["profileId"].isString()||id.isEmpty()||id.size()>64||!request.upload)return invalid;request.profileId=id;}
 if(params.contains("operation")) {const auto id=params["operation"].toString();if(!params["operation"].isString()||id.isEmpty()||id.size()>128)return invalid;request.operationId=id;}
 if(params.contains("since")) {
  static const QRegularExpression pattern("^([1-9][0-9]{0,5})([mhd])$");
  const auto match=pattern.match(params["since"].toString());if(!params["since"].isString()||!match.hasMatch())return invalid;
  const qint64 unit=match.captured(2)=="m"?60000LL:match.captured(2)=="h"?3600000LL:86400000LL;
  request.sinceMs=match.captured(1).toLongLong()*unit;if(request.sinceMs>maximumSinceMs)return invalid;
 }
 if(params.contains("includeNames")){if(!params["includeNames"].isBool())return invalid;request.includeNames=params["includeNames"].toBool();}
 if(params.contains("preview")){if(!params["preview"].isBool())return invalid;request.preview=params["preview"].toBool();}
 if(params.contains("output")) {const auto path=params["output"].toString();if(!params["output"].isString()||path.isEmpty()||path.size()>4096||!QDir::isAbsolutePath(path))return invalid;request.output=QDir::cleanPath(path);}
 // A preview writes nothing, so it uploads nothing either.
 if(request.preview&&(params.contains("upload")||params.contains("copyLink")||params.contains("profileId")))return invalid;
 return {};
}
void DiagnosticsBundles::create(const BundleRequest& request,JsonReply reply) {
 if(running_){reply({"diagnostics_busy","A diagnostics package is already being created; try again when it finishes",{}},{});return;}
 const auto snapshot=operations_.settingsSnapshot();
 Context context;
 context.databasePath=operations_.databasePath();context.logsDirectory=stateRoot_+"/logs";context.defaultFolder=defaultFolder(snapshot);
 context.home=QDir::homePath();context.settings=snapshot.document;
 context.diagnostics=diagnostics_?diagnostics_():operations_.diagnostics();
 context.environment=environment_?environment_():QJsonObject{};
 const auto locations=context.diagnostics["locations"].toObject();
 // diagnostics.locations: {config, data} each with its path and any earlier layout.
 for(const auto& [key,placeholder]:QList<QPair<QString,QString>>{{"config","<config>"},{"data","<data>"}}) {
  const auto location=locations[key].toObject();
  if(location["path"].isString())context.roots.insert(location["path"].toString(),placeholder);
 }
 context.roots.insert(QFileInfo(context.databasePath).absolutePath(),"<data>");
 context.roots.insert(stateRoot_,"<state>");
 const auto runtime=qEnvironmentVariable("XDG_RUNTIME_DIR");if(!runtime.isEmpty())context.roots.insert(runtime,"<runtime>");
 context.roots.insert(snapshot.archive.root,"<archive>");
 running_=true;QPointer<DiagnosticsBundles> guarded(this);
 operations_.background([context,request](Error& error){return build(context,request,error);},[guarded,reply,request,preview=request.preview](Error error,QJsonObject result){
  if(guarded)guarded->running_=false;
  if(guarded&&!preview&&!error)guarded->saved_.insert(QDir::cleanPath(result["path"].toString()));
  if(!preview&&error)logging::warning(logApp(),"Diagnostics package failed",{{"error",logging::errorFields(error)}});
  else if(!preview)logging::info(logApp(),"Diagnostics package saved",{{"path",result["path"]},{"bytes",result["bytes"]},{"redaction",result["redaction"]},{"operations",result["operations"]},{"logRecords",result["logRecords"]}});
  // The local copy exists first; an upload that fails or is skipped never changes it.
  if(error||preview||!request.upload||!guarded){reply(error,result);return;}
  guarded->upload(result,request,reply);
 });
}
QString DiagnosticsBundles::defaultUploadProfile(const Settings& settings) {
 for(const auto& action:QStringList{"region","window","desktop","clipboard","edit",Settings::recordingActionName()}) {
  const auto route=settings.actions.value(action);
  if(route.destination=="upload"&&settings.profiles.contains(route.profileId))return route.profileId;
 }
 return {};
}
bool DiagnosticsBundles::credentialOrConfigurationError(const Error& error) {
 static const QRegularExpression pattern("^(credential|config)_|auth|permission_denied|access_denied|mfa_required|bucket_not_found|bucket_invalid|"
  "region_mismatch|settings_invalid|acl_incompatible|setup_required|account_changed|folder_unavailable|destination_unavailable|"
  "keychain_unavailable|provider_unavailable|unsupported_capability");
 return !error.code.isEmpty()&&pattern.match(error.code).hasMatch();
}
void DiagnosticsBundles::destinationTested(const QString& profileId) {tested_.insert(profileId,QDateTime::currentDateTimeUtc());}
void DiagnosticsBundles::check(const QString& profileId,std::function<void(Error,QJsonObject)> done) {
 const auto snapshot=operations_.settingsSnapshot();const auto revision=operations_.settingsRevision();
 const auto profile=snapshot.profiles.value(profileId);
 if(profile.isEmpty()){done({"profile_not_found","No destination has this ID",{{"profileId",profileId}}},{});return;}
 const auto provider=profile["provider"].toString();const bool proton=provider=="proton-drive";
 QJsonObject target{{"profileId",profileId},{"label",profile["label"].toString()},{"provider",provider},{"publicLink",true},
  {"warning",proton?QStringLiteral("Anyone with the link can download this package until the link expires in %1 days.").arg(OperationService::diagnosticsShareDays)
   :QStringLiteral("On a public-read bucket, anyone with the link can download this package. Delete it from History when it is no longer needed.")}};
 if(proton)target["shareExpirationDays"]=OperationService::diagnosticsShareDays;
 const auto database=operations_.databasePath();QPointer<DiagnosticsBundles> guarded(this);
 const auto settings=profile["settings"].toObject();const auto credentials=profile["credentials"].toObject();
 operations_.background([database,profileId,settings,credentials](Error& error){
  HistoryStore store(database);const auto last=store.lastUpload(profileId,&error);
  // A failure under different settings or credentials says nothing about them now.
  if(error||!last||last->destination.settings!=settings||last->destination.credentialReference!=credentials)return QJsonObject{};
  // The newest upload to this destination: an item or operation error, if any.
  Error failure=last->error;for(const auto& item:last->items)if(item.uploadStatus=="failed"&&item.error)failure=item.error;
  return QJsonObject{{"operationId",last->id},{"state",last->state},{"created",last->created},{"error",failure.toJson()}};
 },[guarded,profileId,revision,target,done](Error error,QJsonObject last) {
  if(!guarded)return;
  if(error){done(error,{});return;}
  const auto lastError=last["error"].toObject();
  const auto tested=guarded->tested_.value(profileId);const auto failedAt=QDateTime::fromString(last["created"].toString(),Qt::ISODateWithMs);
  const bool testedSince=tested.isValid()&&failedAt.isValid()&&tested>failedAt;
  if(!testedSince&&QStringList{"failed","partial"}.contains(last["state"].toString())&&credentialOrConfigurationError({lastError["code"].toString(),{},{}})) {
   done({"diagnostics_destination_failing","The last upload to this destination failed with a credential or configuration error, so the package was not uploaded. Fix the destination in Settings, then upload it.",
    {{"profileId",profileId},{"operationId",last["operationId"]},{"lastError",lastError["code"]}}},target);return;
  }
  const auto previous=guarded->checked_.value(profileId);
  if(previous.revision==revision&&previous.at.isValid()&&previous.at.msecsTo(QDateTime::currentDateTimeUtc())<checkReuseMs){done({},target);return;}
  if(!guarded->destinationTest_){done({"diagnostics_upload_unavailable","This resident cannot test destinations",{}},target);return;}
  auto* owner=guarded.data();
  guarded->destinationTest_(profileId,[owner,guarded,profileId,revision,target,done](Error tested) {
   // A provider test may answer on its worker thread; continue on the session thread.
   QMetaObject::invokeMethod(owner,[guarded,profileId,revision,target,done,tested] {
    if(!guarded)return;
    if(tested){guarded->checked_.remove(profileId);done({"diagnostics_destination_unavailable","Test connection failed for this destination, so the package was not uploaded",{{"profileId",profileId},{"error",tested.toJson()}}},target);return;}
    guarded->checked_.insert(profileId,{revision,QDateTime::currentDateTimeUtc()});guarded->destinationTested(profileId);
    done({},target);
   },Qt::QueuedConnection);
  });
 });
}
void DiagnosticsBundles::uploadTarget(const QString& requested,JsonReply reply) {
 const auto profileId=requested.isEmpty()?defaultUploadProfile(operations_.settingsSnapshot()):requested;
 if(profileId.isEmpty()){reply({},{{"available",false},{"reason","diagnostics_no_destination"},{"message","No upload destination is set up"}});return;}
 check(profileId,[reply,profileId](Error error,QJsonObject target) {
  if(error.code=="profile_not_found"){reply(error,{});return;}
  if(error){target["profileId"]=profileId;target["available"]=false;target["reason"]=error.code;target["message"]=error.message;target["details"]=error.details;reply({},target);return;}
  target["available"]=true;reply({},target);
 });
}
void DiagnosticsBundles::upload(QJsonObject result,const BundleRequest& request,JsonReply reply) {
 const auto profileId=request.profileId.isEmpty()?defaultUploadProfile(operations_.settingsSnapshot()):request.profileId;
 // A skipped or failed upload is part of a successful reply: the package is saved.
 auto settle=[reply](QJsonObject result,const Error& error) {
  if(error){result["uploadError"]=error.toJson();logging::warning(logApp(),"Diagnostics package not uploaded",{{"error",logging::errorFields(error)}});}
  reply({},result);
 };
 if(profileId.isEmpty()){settle(result,{"diagnostics_no_destination","No upload destination is set up, so the package was saved locally only",{}});return;}
 QPointer<DiagnosticsBundles> guarded(this);
 const auto path=result["path"].toString();const bool copyLink=request.copyLink;
 check(profileId,[guarded,result,settle,profileId,path,copyLink](Error error,QJsonObject target) mutable {
  if(!guarded)return;
  if(error){settle(result,error);return;}
  result["upload"]=target;
  guarded->operations_.uploadDiagnostics(path,profileId,[result,settle](Error accepted,QJsonObject) mutable {
   if(accepted)settle(result,accepted);
  },[guarded,result,settle,copyLink](const Operation& op) mutable {
   if(!guarded)return;
   auto upload=result["upload"].toObject();upload["operationId"]=op.id;upload["state"]=op.state;
   const auto item=op.items.value(0);
   if(!item.candidate.isEmpty())upload["name"]=item.candidate;
   if(item.artifact.id.size())upload["historyId"]=item.artifact.id;
   result["upload"]=upload;
   if(op.state!="completed"||item.uploadStatus!="uploaded") {
    auto error=item.error?item.error:op.error?op.error:Error{"diagnostics_upload_failed","The package was not uploaded",{}};
    error.details["operationId"]=op.id;settle(result,error);return;
   }
   if(!item.linkAvailable()){settle(result,{"diagnostics_link_unavailable","The package was uploaded, but this destination gives it no link",{{"operationId",op.id}}});return;}
   result["link"]=item.receipt.directUrl;
   logging::info(logApp(),"Diagnostics package uploaded",{{"operationId",op.id},{"provider",op.destination.providerId},{"profileId",op.destination.profileId}});
   if(!copyLink){settle(result,{});return;}
   guarded->operations_.copyHistoryUrl(item.receipt.directUrl,[result,settle](Error copied,QJsonObject) mutable {
    result["linkCopied"]=!copied;if(copied)result["copyError"]=copied.toJson();settle(result,{});
   });
  });
 });
}
QJsonObject DiagnosticsBundles::coredumps(const QString& configured,const QDateTime& from,const PackageRedactor& redactor) {
 if(configured=="-")return {{"available",false},{"reason","skipped"},{"entries",QJsonArray{}}};
 const auto program=configured.isEmpty()?QStandardPaths::findExecutable("coredumpctl"):configured;
 if(program.isEmpty())return {{"available",false},{"reason","coredumpctl_missing"},{"entries",QJsonArray{}}};
 auto run=[&](const QStringList& arguments,QByteArray& output) {
  QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);
  auto environment=QProcessEnvironment::systemEnvironment();environment.insert("SYSTEMD_COLORS","0");environment.insert("SYSTEMD_PAGER","");
  process.setProcessEnvironment(environment);
  process.start(program,QStringList{"--no-pager","--no-legend"}+arguments);
  if(!process.waitForFinished(10000)){process.kill();process.waitForFinished(1000);return -1;}
  output=process.readAllStandardOutput().left(1024*1024);
  return process.exitStatus()==QProcess::NormalExit?process.exitCode():-1;
 };
 QByteArray listing;
 const int status=run({"--json=short","--since=@"+QString::number(from.toSecsSinceEpoch()),"list"},listing);
 QJsonParseError parse{};const auto document=QJsonDocument::fromJson(listing,&parse);
 // coredumpctl exits 1 with nothing on stdout when no dump matches.
 if(status==1&&listing.trimmed().isEmpty())return {{"available",true},{"entries",QJsonArray{}}};
 if(status!=0||parse.error!=QJsonParseError::NoError||!document.isArray())return {{"available",false},{"reason","coredumpctl_failed"},{"entries",QJsonArray{}}};
 static const QStringList products{"putgd","putgd-ui","putgd-capture","putgd-kde-region","putgd-record","putgd-s3-worker","putgd-cli"};
 QList<QJsonObject> matches;
 for(const auto& value:document.array()) {
  const auto dump=value.toObject();
  if(products.contains(QFileInfo(dump["exe"].toString()).fileName()))matches.append(dump);
 }
 std::sort(matches.begin(),matches.end(),[](const QJsonObject& a,const QJsonObject& b){return a["time"].toDouble()>b["time"].toDouble();});
 QJsonArray entries;
 for(const auto& dump:matches.mid(0,maximumCoredumps)) {
  const auto pid=qint64(dump["pid"].toDouble());const auto time=qint64(dump["time"].toDouble());
  QByteArray info;run({"--since=@"+QString::number(time/1000000),"info","COREDUMP_PID="+QString::number(pid)},info);
  // Only the modules and stack traces systemd printed: the command line,
  // environment, host and unit lines stay out.
  QStringList stack;bool inside=false;
  for(const auto& raw:QString::fromUtf8(info).split('\n')) {
   const auto line=raw.trimmed();
   if(line.startsWith("Module ")||line.startsWith("Stack trace of thread"))inside=true;
   if(inside&&!line.isEmpty())stack.append(line.startsWith("#")||line.startsWith("Module ")||line.startsWith("Stack trace")||line.startsWith("ELF object")?line:QString());
  }
  stack.removeAll(QString());
  auto text=stack.join('\n');if(text.size()>coredumpTextBytes)text=text.left(coredumpTextBytes)+"\n<truncated>";
  entries.append(QJsonObject{{"time",iso(QDateTime::fromMSecsSinceEpoch(time/1000))},{"pid",pid},{"signal",dump["sig"].toInt()},
   {"executable",redactor.text(dump["exe"].toString())},{"storage",dump["corefile"].toString().left(32)},{"stack",redactor.text(text)}});
 }
 return {{"available",true},{"entries",entries}};
}
int DiagnosticsBundles::prune(const QString& folder,const QDateTime& now) {
 QDir dir(folder);QFileInfoList packages;
 for(const auto& info:dir.entryInfoList({"putgd-diagnostics-*.zip"},QDir::Files,QDir::Time))if(packageName().match(info.fileName()).hasMatch())packages.append(info);
 int removed=0;const auto cutoff=now.addDays(-keptDays);
 for(qsizetype index=0;index<packages.size();++index)
  if((index>=keptPackages||packages[index].lastModified().toUTC()<cutoff)&&QFile::remove(packages[index].filePath()))++removed;
 return removed;
}
QJsonObject DiagnosticsBundles::build(const Context& context,const BundleRequest& request,Error& error) {
 const auto now=context.now.toUTC();
 PackageRedactor redactor(request.includeNames);
 for(auto it=context.roots.begin();it!=context.roots.end();++it)redactor.root(it.key(),it.value());
 if(!context.home.isEmpty())redactor.root(context.home,"<home>");
 // Everything free text may name: destinations from settings, items from history.
 for(const auto& value:context.settings["profiles"].toArray()) {
  const auto profile=value.toObject();const auto settings=profile["settings"].toObject();
  redactor.literal("label",profile["label"].toString());
  redactor.literal("bucket",settings["bucket"].toString());redactor.literal("endpoint",settings["endpoint"].toString());
  redactor.literal("url",settings["publicUrlBase"].toString());redactor.literal("name",settings["folderName"].toString());
  for(const auto* key:{"accountRootUid","folderUid"})redactor.literal("id",settings[key].toString());
 }
 HistoryStore store(context.databasePath);if(store.error()){error=store.error();return {};}
 QList<Operation> operations;
 auto from=now.addMSecs(-request.sinceMs),to=now;
 if(!request.operationId.isEmpty()) {
  Error lookup;auto op=store.operation(request.operationId,&lookup);if(lookup){error=lookup;return {};}
  const auto created=QDateTime::fromString(op.created,Qt::ISODateWithMs);
  from=(created.isValid()?created:now).addSecs(-600);
  operations.append(std::move(op));
 }else {
  Error query;operations=store.operationsSince(iso(from),{"failed","partial","interrupted"},maximumOperations,&query);if(query){error=query;return {};}
 }
 // An uploaded earlier package stays out of later ones, its log records too;
 // one whose upload failed is kept, like any failure (#123).
 QSet<QByteArray> uploadedPackages;
 {
  Error query;
  for(const auto& op:store.operationsSince(iso(from),{"completed"},100000,&query))
   if(op.action==OperationService::diagnosticsAction&&op.id!=request.operationId)uploadedPackages.insert("\"operationId\":\""+op.id.toUtf8()+"\"");
 }
 const auto uploadedPackage=[&uploadedPackages](const QByteArray& line){for(const auto& needle:uploadedPackages)if(line.contains(needle))return true;return false;};
 for(const auto& op:operations)for(const auto& item:op.items) {
  redactor.literal("name",item.artifact.name);redactor.literal("path",item.artifact.path);redactor.literal("path",item.artifact.previewPath);
  redactor.literal("name",item.candidate);redactor.literal("key",item.objectReference["key"].toString());
  redactor.literal("url",item.receipt.directUrl);redactor.literal("path",item.local.path);
 }
 // The log files, newest first. For one operation the window runs from ten
 // minutes before it was accepted to ten minutes after its last record.
 QDir logs(context.logsDirectory);auto files=logs.entryList({"putgd-*.jsonl"},QDir::Files,QDir::Name);std::reverse(files.begin(),files.end());
 QHash<QString,QByteArray> contents;
 for(const auto& name:files)contents.insert(name,readBounded(logs.filePath(name),64LL*1024*1024));
 if(!request.operationId.isEmpty()) {
  QString last;const auto needle="\"operationId\":\""+request.operationId.toUtf8()+"\"";
  for(const auto& name:files)for(const auto& line:contents.value(name).split('\n'))if(line.contains(needle)){const auto time=QJsonDocument::fromJson(line).object()["time"].toString();if(time>last)last=time;}
  const auto end=QDateTime::fromString(last,Qt::ISODateWithMs);
  to=end.isValid()?qMin(now,end.addSecs(600)):now;
 }
 const auto fromText=iso(from),toText=iso(to);
 // Fixed parts first: the logs get what is left of the size limit.
 QJsonArray operationList;QHash<QString,int> attempts;
 auto environment=context.environment;
 environment["osRelease"]=osRelease();environment["kernel"]=QSysInfo::kernelType()+" "+QSysInfo::kernelVersion();
 environment["architecture"]=QSysInfo::currentCpuArchitecture();environment["qtCompiled"]=QT_VERSION_STR;environment["qtRuntime"]=qVersion();
 environment["locale"]=QLocale::system().name();environment["desktop"]=qEnvironmentVariable("XDG_CURRENT_DESKTOP");environment["sessionType"]=qEnvironmentVariable("XDG_SESSION_TYPE");
 environment["memoryTotalBytes"]=memoryTotal();
 const QStorageInfo storage(QFileInfo(context.databasePath).absolutePath());environment["dataFreeBytes"]=storage.isValid()?storage.bytesAvailable():-1;
 auto settings=context.settings;
 // Archive folders keep their shape below the home folder.
 auto archive=settings["archive"].toObject();
 auto home=[&](const QString& path){return !context.home.isEmpty()&&(path==context.home||path.startsWith(context.home+"/"))?"<home>"+path.mid(context.home.size()):path;};
 archive["root"]=home(archive["root"].toString());
 auto folders=archive["folders"].toObject();for(auto it=folders.begin();it!=folders.end();++it)if(it.value().isString())it.value()=home(it.value().toString());archive["folders"]=folders;
 settings.remove("archive");
 auto configRedacted=redactor.json(settings).toObject();configRedacted["archive"]=archive;
 const auto environmentBytes=compact(redactor.json(environment));
 const auto diagnosticsBytes=compact(redactor.json(context.diagnostics));
 const auto configBytes=compact(configRedacted);
 auto coredumpReport=coredumps(context.coredumpctl,from,redactor);coredumpReport["since"]=iso(from);
 const auto coredumpBytes=compact(coredumpReport);
 // Count upload attempts from the window's records before choosing lines.
 struct Line {QString time;QByteArray bytes;};QList<Line> selected;qint64 used=0,dropped=0,eligible=0;
 const qint64 fixedBytes=environmentBytes.size()+diagnosticsBytes.size()+configBytes.size()+coredumpBytes.size()+256*1024;
 for(const auto& name:files) for(const auto& raw:contents.value(name).split('\n')) {
  if(raw.isEmpty()||uploadedPackage(raw))continue;
  const auto object=QJsonDocument::fromJson(raw).object();const auto time=object["time"].toString();
  if(time.isEmpty()||time<fromText||time>toText)continue;
  if(object["message"]=="Upload attempt")attempts[object["itemId"].toString()]+=1;
 }
 for(const auto& op:operations) {
  QJsonArray items;for(const auto& item:op.items)items.append(itemSummary(item,attempts.value(item.artifact.id)));
  QJsonObject summary{{"id",op.id},{"action",op.action},{"state",op.state},{"phase",op.phase},{"created",op.created},{"cancelled",op.cancelled},
   {"copyStatus",op.copyStatus},{"provider",op.destination.providerId},{"profileId",op.destination.profileId},{"items",items}};
  if(op.error)summary["error"]=op.error.toJson();
  operationList.append(summary);
 }
 const auto operationsBytes=compact(redactor.json(operationList,"operations"));
 const qint64 budget=maximumBytes-fixedBytes-operationsBytes.size();
 for(const auto& name:files) {
  const auto lines=contents.value(name).split('\n');
  for(auto it=lines.crbegin();it!=lines.crend();++it) {
   if(it->isEmpty()||uploadedPackage(*it))continue;
   const auto object=QJsonDocument::fromJson(*it).object();const auto time=object["time"].toString();
   if(time.isEmpty()||time<fromText||time>toText)continue;
   ++eligible;
   // Newest first: once the limit is reached, the older records are dropped.
   if(used>=budget){++dropped;continue;}
   auto bytes=QJsonDocument(redactor.json(object).toObject()).toJson(QJsonDocument::Compact)+'\n';
   if(used+bytes.size()>budget){++dropped;used=budget;continue;}
   used+=bytes.size();selected.append({time,std::move(bytes)});
  }
 }
 // Chosen newest first; written oldest first, in file order (records can share a millisecond).
 std::reverse(selected.begin(),selected.end());
 QByteArray logBytes;for(const auto& line:selected)logBytes+=line.bytes;
 QList<QPair<QString,QByteArray>> entries{{"environment.json",environmentBytes},{"diagnostics.json",diagnosticsBytes},{"config.redacted.json",configBytes},
  {"operations.json",operationsBytes},{"coredumps.json",coredumpBytes},{"logs/putgd.jsonl",logBytes}};
 QJsonArray manifestFiles;for(const auto& [path,bytes]:entries)manifestFiles.append(QJsonObject{{"path",path},{"bytes",bytes.size()},{"sha256",sha256(bytes)}});
 QJsonObject scope{{"from",fromText},{"to",toText}};
 if(!request.operationId.isEmpty())scope["operationId"]=request.operationId;else scope["since"]=QString::number(request.sinceMs/60000)+"m";
 const QJsonObject manifest{{"schemaVersion",1},{"created",iso(now)},{"build",QJsonDocument::fromJson(buildManifestJson()).object()},
  {"redaction",request.includeNames?"names":"hashed"},{"scope",scope},{"files",manifestFiles},{"operations",operationList.size()},
  {"logs",QJsonObject{{"records",selected.size()},{"eligibleRecords",eligible},{"droppedRecords",dropped},{"sourceFiles",files.size()}}},
  {"maximumBytes",maximumBytes}};
 // A preview (the Settings dialog) shows what the package would hold and where it
 // would go, from the same selection, and writes nothing.
 if(request.preview) {
  QJsonArray names;names.append("manifest.json");for(const auto& entry:entries)names.append(entry.first);
  QJsonArray summaries;for(const auto& value:operationList){const auto op=value.toObject();summaries.append(QJsonObject{{"id",op["id"]},{"action",op["action"]},{"state",op["state"]},{"created",op["created"]},{"items",op["items"].toArray().size()}});}
  const QFileInfo output(request.output);
  const auto folder=request.output.isEmpty()?context.defaultFolder:output.isDir()?output.absoluteFilePath():output.absolutePath();
  return {{"preview",true},{"files",names},{"redaction",request.includeNames?"names":"hashed"},{"scope",scope},{"operations",operationList.size()},
   {"operationList",summaries},{"logRecords",selected.size()},{"droppedLogRecords",dropped},{"folder",folder},{"defaultFolder",request.output.isEmpty()},
   {"maximumBytes",maximumBytes}};
 }
 ZipWriter zip(now);zip.add("manifest.json",QJsonDocument(manifest).toJson(QJsonDocument::Indented));
 for(const auto& [path,bytes]:entries)zip.add(path,bytes);
 const auto archiveBytes=zip.finish();
 // Saved locally first and only: the default folder belongs to put.gd.
 QString path;bool inDefaultFolder=false;
 if(request.output.isEmpty()) {
  if(!QDir().mkpath(context.defaultFolder)){error={"diagnostics_output_unavailable","Cannot create the diagnostics folder",{{"path",context.defaultFolder}}};return {};}
  QFile::setPermissions(context.defaultFolder,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
  path=uniqueName(context.defaultFolder,now);inDefaultFolder=true;
 }else {
  const QFileInfo info(request.output);
  if(info.isDir())path=uniqueName(info.absoluteFilePath(),now);
  else if(info.exists()||info.isSymLink()){error={"diagnostics_output_exists","A file already exists at the output path; choose another name or a folder",{{"path",request.output}}};return {};}
  else if(!request.output.endsWith(".zip",Qt::CaseInsensitive)){error={"invalid_request","The output path must be a folder or end in .zip",{}};return {};}
  else if(!info.dir().exists()){error={"diagnostics_output_unavailable","The output folder does not exist",{{"path",info.absolutePath()}}};return {};}
  else path=info.absoluteFilePath();
 }
 QSaveFile file(path);file.setDirectWriteFallback(false);
 if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||file.write(archiveBytes)!=archiveBytes.size()||!file.commit()){error={"diagnostics_output_unavailable","Cannot write the diagnostics package",{{"path",path}}};return {};}
 const int pruned=inDefaultFolder?prune(context.defaultFolder,now):0;
 QJsonArray names;names.append("manifest.json");for(const auto& entry:entries)names.append(entry.first);
 return {{"path",path},{"bytes",archiveBytes.size()},{"sha256",sha256(archiveBytes)},{"redaction",request.includeNames?"names":"hashed"},
  {"files",names},{"operations",operationList.size()},{"logRecords",selected.size()},{"droppedLogRecords",dropped},
  {"defaultFolder",inDefaultFolder},{"pruned",pruned}};
}
}
