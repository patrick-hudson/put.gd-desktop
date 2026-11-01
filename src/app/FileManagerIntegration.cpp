#include "FileManagerIntegration.h"
#include "FileManagerTemplates.h"
#include "LoginStartScript.h"
#include "LoginStartup.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <unistd.h>
namespace putgd {
namespace {
const QString marker=QStringLiteral("X-putgd-Managed=true");
constexpr auto programMode=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther;
constexpr auto dataMode=QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther;
constexpr auto modeBits=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|
 QFile::ReadOther|QFile::WriteOther|QFile::ExeOther;
constexpr qint64 textLimit=4*1024*1024;
const QString scriptName=QStringLiteral("Upload with put.gd");

QString hash(const QString& text) {return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(),QCryptographicHash::Sha256).toHex());}
bool plain(const QString& value) {static const QRegularExpression safe("^[A-Za-z0-9/._+-]+$");return safe.match(value).hasMatch();}
QString shellQuoted(QString value) {return "'"+value.replace("'","'\\''")+"'";}
QString shellWord(const QString& value) {return plain(value)?value:shellQuoted(value);}
QString keyFileEscaped(QString value) {value.replace("\\","\\\\");value.replace("\t","\\t");return value;}
// An Exec program for GDesktopAppInfo and KIO: desktop-entry quoting, then
// desktop-string escaping, which is decoded first.
QString desktopExec(QString value) {
 if(plain(value))return value;
 value.replace("\\","\\\\");value.replace("\"","\\\"");value.replace("$","\\$");value.replace("`","\\`");value.replace("%","%%");
 return "\""+keyFileEscaped(value)+"\"";
}
// Nemo and libfm expand their own % codes, then split the line as a shell does.
QString shellExec(const QString& value) {
 if(plain(value))return value;
 auto quoted=shellQuoted(value);quoted.replace("%","%%");
 return keyFileEscaped(quoted);
}
bool singleLine(const QString& value) {return !value.contains('\n')&&!value.contains('\r');}
bool ownedByUser(const QFileInfo& info) {return info.ownerId()==::getuid();}
QByteArray readSmall(const QString& path,bool* ok=nullptr) {
 QFile file(path);
 const auto opened=file.open(QIODevice::ReadOnly)&&file.size()<=textLimit;
 if(ok)*ok=opened;
 return opened?file.readAll():QByteArray{};
}
bool managedFile(const QString& path) {
 const QFileInfo info(path);
 return info.isFile()&&!info.isSymLink()&&ownedByUser(info)&&readSmall(path).contains(marker.toUtf8());
}

enum class Write {Written,Unchanged,Conflict,Failed};
// Atomic, with an explicit mode. A file put.gd did not write is left alone.
Write commit(const QString& path,const QByteArray& bytes,QFile::Permissions mode,bool requireMarker=true) {
 const QFileInfo info(path);
 if(info.exists()||info.isSymLink()) {
  if(info.isSymLink()||!info.isFile()||(requireMarker&&!managedFile(path)))return Write::Conflict;
  bool ok=false;const auto current=readSmall(path,&ok);
  if(ok&&current==bytes&&(info.permissions()&modeBits)==mode)return Write::Unchanged;
 }
 if(!QDir().mkpath(info.path()))return Write::Failed;
 QSaveFile file(path);file.setDirectWriteFallback(false);
 if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(mode)||file.write(bytes)!=bytes.size()||!file.commit())return Write::Failed;
 return Write::Written;
}
// Remove a file put.gd owns: in its own folder any regular file of this user,
// elsewhere only one that carries the managed marker.
bool removeOwned(const QString& path,const QString& ownDirectory) {
 const QFileInfo info(path);
 if(!info.exists()&&!info.isSymLink())return false;
 const auto own=QDir::cleanPath(path).startsWith(QDir::cleanPath(ownDirectory)+'/');
 if(info.isSymLink()||!info.isFile()||!ownedByUser(info)||(!own&&!managedFile(path)))return false;
 return QFile::remove(path);
}

// The owned-files record: line based, so the launcher's shell cleanup reads it too.
struct Record {
 QString id,image,updateInformation,cosmic,cosmicLine;
 QStringList files;
};
Record readRecord(const QString& path) {
 Record record;
 if(!managedFile(path))return record;
 const auto lines=QString::fromUtf8(readSmall(path)).split('\n');
 if(!lines.contains("# "+marker))return record;
 for(const auto& line:lines) {
  const auto value=[&](const QString& key){return line.mid(key.size());};
  if(line.startsWith("# X-putgd-IntegrationId="))record.id=value("# X-putgd-IntegrationId=");
  else if(line.startsWith("file "))record.files<<value("file ");
  else if(line.startsWith("image "))record.image=value("image ");
  else if(line.startsWith("update "))record.updateInformation=value("update ");
  else if(line.startsWith("cosmic-line "))record.cosmicLine=value("cosmic-line ");
  else if(line.startsWith("cosmic "))record.cosmic=value("cosmic ");
 }
 return record;
}
QByteArray recordText(const Record& record) {
 QString text="# put.gd file manager integration (#1105): the files put.gd owns. put.gd\n# rewrites this list; edits are replaced.\n# "+marker+'\n';
 if(!record.id.isEmpty())text+="# X-putgd-IntegrationId="+record.id+'\n';
 if(!record.image.isEmpty())text+="image "+record.image+'\n';
 if(!record.updateInformation.isEmpty())text+="update "+record.updateInformation+'\n';
 for(const auto& file:record.files)text+="file "+file+'\n';
 if(!record.cosmic.isEmpty())text+="cosmic "+record.cosmic+'\n';
 if(!record.cosmicLine.isEmpty())text+="cosmic-line "+record.cosmicLine+'\n';
 return text.toUtf8();
}

// COSMIC Files' context_actions value, Vec<ContextActionPreset> in RON
// (cosmic-files src/context_action.rs, epoch-1.0.10 to 1.11.0): fields name,
// confirm, selection (Any, Files, Folders, or their lowercase aliases) and
// steps, each optional. Strict: anything else (unknown fields, other values,
// extensions) is unparseable, so put.gd never rewrites a list it can't read.
// Every preset keeps its source span; edits splice text, so other entries keep
// their bytes, comments and layout.
struct Preset {
 QString name,selection=QStringLiteral("Any");
 bool confirm=false;
 QStringList steps;
 qsizetype start=0,end=0,comma=-1;
 bool sameValue(const Preset& other) const {return name==other.name&&selection==other.selection&&confirm==other.confirm&&steps==other.steps;}
};
struct PresetList {bool ok=false;qsizetype close=-1;QList<Preset> presets;};
class RonReader {
public:
 explicit RonReader(const QString& text):text_(text) {}
 PresetList read() {
  PresetList list;
  if(!skip())return list;
  if(at_==text_.size()) {list.ok=true;return list;} // an empty value: no list yet
  if(!take('['))return list;
  for(;;) {
   if(!skip())return list;
   if(take(']'))break;
   Preset preset;
   if(!readPreset(preset)||!skip())return list;
   if(take(',')) {preset.comma=at_-1;list.presets<<preset;continue;}
   list.presets<<preset;
   if(!skip()||!take(']'))return list;
   break;
  }
  list.close=at_-1;
  list.ok=skip()&&at_==text_.size();
  return list;
 }
private:
 const QString& text_;
 qsizetype at_=0;
 bool take(QChar c) {if(at_<text_.size()&&text_[at_]==c) {++at_;return true;}return false;}
 bool skip() {
  while(at_<text_.size()) {
   const auto c=text_[at_];
   if(c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f') {++at_;continue;}
   if(c=='/'&&at_+1<text_.size()&&text_[at_+1]=='/') {while(at_<text_.size()&&text_[at_]!='\n')++at_;continue;}
   if(c=='/'&&at_+1<text_.size()&&text_[at_+1]=='*') {
    at_+=2;int depth=1;
    while(depth>0) {
     if(at_+1>=text_.size())return false;
     if(text_[at_]=='/'&&text_[at_+1]=='*') {++depth;at_+=2;}
     else if(text_[at_]=='*'&&text_[at_+1]=='/') {--depth;at_+=2;}
     else ++at_;
    }
    continue;
   }
   break;
  }
  return true;
 }
 bool identifier(QString& out) {
  const auto start=at_;
  while(at_<text_.size()&&(text_[at_].isLetterOrNumber()||text_[at_]=='_')&&text_[at_].unicode()<0x80)++at_;
  out=text_.mid(start,at_-start);
  return !out.isEmpty()&&!out[0].isDigit();
 }
 bool string(QString& out) {
  out.clear();
  if(take('r')) {
   int hashes=0;while(take('#'))++hashes;
   if(!take('"'))return false;
   const auto close=QString('"')+QString(hashes,'#');
   const auto end=text_.indexOf(close,at_);
   if(end<0)return false;
   out=text_.mid(at_,end-at_);at_=end+close.size();
   return true;
  }
  if(!take('"'))return false;
  while(at_<text_.size()) {
   const auto c=text_[at_++];
   if(c=='"')return true;
   if(c!='\\') {out+=c;continue;}
   if(at_>=text_.size())return false;
   const auto e=text_[at_++];
   if(e=='"'||e=='\\'||e=='\'')out+=e;
   else if(e=='n')out+='\n';
   else if(e=='r')out+='\r';
   else if(e=='t')out+='\t';
   else if(e=='0')out+=QChar(0);
   else if(e=='b')out+='\b';
   else if(e=='f')out+='\f';
   else if(e=='x') {
    bool ok=false;const auto code=text_.mid(at_,2).toUInt(&ok,16);
    if(!ok||at_+2>text_.size()||code>0x7f)return false;
    out+=QChar(code);at_+=2;
   } else if(e=='u') {
    if(!take('{'))return false;
    const auto end=text_.indexOf('}',at_);
    bool ok=false;const auto code=end<0||end-at_<1||end-at_>6?0u:text_.mid(at_,end-at_).toUInt(&ok,16);
    if(!ok||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
    const char32_t point=code;out+=QString::fromUcs4(&point,1);at_=end+1;
   } else return false;
  }
  return false;
 }
 bool readPreset(Preset& preset) {
  preset.start=at_;
  if(at_<text_.size()&&text_[at_]!='(') {
   QString name;
   if(!identifier(name)||name!="ContextActionPreset"||!skip())return false;
  }
  if(!take('('))return false;
  QStringList seen;
  for(;;) {
   if(!skip())return false;
   if(take(')'))break;
   QString field;
   if(!identifier(field)||seen.contains(field)||!skip()||!take(':')||!skip())return false;
   seen<<field;
   if(field=="name") {if(!string(preset.name))return false;}
   else if(field=="confirm") {
    QString value;if(!identifier(value)||(value!="true"&&value!="false"))return false;
    preset.confirm=value=="true";
   } else if(field=="selection") {
    QString value;if(!identifier(value))return false;
    static const QStringList variants{"Any","Files","Folders"};
    const auto index=variants.indexOf(value)>=0?variants.indexOf(value):QStringList{"any","files","folders"}.indexOf(value);
    if(index<0)return false;
    preset.selection=variants[index];
   } else if(field=="steps") {
    if(!take('['))return false;
    for(;;) {
     if(!skip())return false;
     if(take(']'))break;
     QString step;if(!string(step)||!skip())return false;
     preset.steps<<step;
     if(take(','))continue;
     if(!skip()||!take(']'))return false;
     break;
    }
   } else return false;
   if(!skip())return false;
   if(take(','))continue;
   if(!skip()||!take(')'))return false;
   break;
  }
  preset.end=at_;
  return true;
 }
};
PresetList parsePresets(const QString& text) {return RonReader(text).read();}
// The program of an Exec line, split as cosmic-files does (shlex).
QString program(const QString& step) {
 QString word;qsizetype at=0;
 while(at<step.size()&&step[at].isSpace())++at;
 while(at<step.size()&&!step[at].isSpace()) {
  const auto c=step[at++];
  if(c=='\'') {while(at<step.size()&&step[at]!='\'')word+=step[at++];++at;}
  else if(c=='"') {
   while(at<step.size()&&step[at]!='"') {
    if(step[at]=='\\'&&at+1<step.size()&&QStringLiteral("\"\\$`").contains(step[at+1]))++at;
    word+=step[at++];
   }
   ++at;
  } else if(c=='\\'&&at<step.size())word+=step[at++];
  else word+=c;
 }
 return word;
}
bool runs(const Preset& preset,const QString& launcher) {
 for(const auto& step:preset.steps)if(program(step)==launcher)return true;
 return false;
}
QString ronString(const QString& value) {
 QString out="\"";
 for(const auto c:value) {
  if(c=='"'||c=='\\')out+='\\'+QString(c);
  else if(c.unicode()<0x20)out+="\\u{"+QString::number(c.unicode(),16)+"}";
  else out+=c;
 }
 return out+"\"";
}
QString cosmicEntry(const QString& launcher) {
 return "(name: "+ronString(FileManagerIntegration::label())+", confirm: false, selection: Any, steps: ["+
  ronString(shellWord(launcher)+" --upload -- %F")+"])";
}
QString cosmicLine(const QString& launcher) {return "    "+cosmicEntry(launcher)+",";}
bool decodeUtf8(const QByteArray& bytes,QString& text) {
 QStringDecoder decoder(QStringDecoder::Utf8,QStringDecoder::Flag::Stateless);
 text=decoder.decode(bytes);
 return !decoder.hasError()&&!bytes.startsWith("\xef\xbb\xbf");
}
// Remove [start,end) and, when that leaves its line blank, the whole line.
void removeSpan(QString& text,qsizetype start,qsizetype end) {
 auto lineStart=text.lastIndexOf('\n',start-1)+1;auto lineEnd=text.indexOf('\n',end);
 if(lineEnd<0)lineEnd=text.size();
 const auto blank=[&](qsizetype from,qsizetype to){for(auto i=from;i<to;++i)if(text[i]!=' '&&text[i]!='\t'&&text[i]!='\r')return false;return true;};
 if(start>0&&blank(lineStart,start)&&blank(end,lineEnd)&&lineEnd<text.size())text.remove(lineStart,lineEnd+1-lineStart);
 else text.remove(start,end-start);
}
struct CosmicChange {Error error;bool changed=false;QString line;};
// Adds put.gd's entry (want) or removes it (only when the record names this
// file), recognising it by the launcher it runs. Nothing is written unless the
// value parses, and the result parses back to the same other entries.
CosmicChange mergeCosmic(const QString& path,const QString& launcher,bool want,bool recorded) {
 CosmicChange change;
 const auto unparseable=Error{"cosmic_context_actions_unparseable","COSMIC Files' context actions could not be read; put.gd left them unchanged",{{"path",path}}};
 const QFileInfo info(path);
 const auto exists=info.exists()||info.isSymLink();
 if(!exists&&!want)return change;
 QByteArray before;QString text;
 if(exists) {
  bool ok=false;before=readSmall(path,&ok);
  if(info.isSymLink()||!info.isFile()||!ok||!decodeUtf8(before,text)) {change.error=unparseable;return change;}
 }
 const auto list=parsePresets(text);
 if(!list.ok) {change.error=unparseable;return change;}
 const auto entry=cosmicEntry(launcher);
 QList<Preset> mine,others;
 for(const auto& preset:list.presets)(runs(preset,launcher)?mine:others)<<preset;
 auto next=text;
 const auto canonical=mine.size()==1&&text.mid(mine[0].start,mine[0].end-mine[0].start)==entry;
 if(want&&!canonical) {
  for(auto i=mine.size()-1;i>=0;--i)removeSpan(next,mine[i].start,mine[i].comma>=0?mine[i].comma+1:mine[i].end);
  const auto rest=parsePresets(next);
  if(!rest.ok) {change.error=unparseable;return change;}
  if(rest.close<0)next="[\n"+cosmicLine(launcher)+"\n]\n";
  else {
   auto close=rest.close;
   if(!rest.presets.isEmpty()&&rest.presets.last().comma<0) {next.insert(rest.presets.last().end,',');++close;}
   next.insert(close,(close>0&&next[close-1]=='\n'?QString{}:QStringLiteral("\n"))+cosmicLine(launcher)+'\n');
  }
 } else if(!want&&recorded) {
  for(auto i=mine.size()-1;i>=0;--i)removeSpan(next,mine[i].start,mine[i].comma>=0?mine[i].comma+1:mine[i].end);
 }
 // Parse the new value back: every other entry unchanged, exactly one of ours or none.
 const auto after=parsePresets(next);
 QList<Preset> afterMine,afterOthers;
 for(const auto& preset:after.presets)(runs(preset,launcher)?afterMine:afterOthers)<<preset;
 auto same=after.ok&&afterOthers.size()==others.size();
 for(qsizetype i=0;same&&i<others.size();++i)same=others[i].sameValue(afterOthers[i])&&
  text.mid(others[i].start,others[i].end-others[i].start)==next.mid(afterOthers[i].start,afterOthers[i].end-afterOthers[i].start);
 if(!same||afterMine.size()!=(want?1:(recorded?0:mine.size()))) {change.error={"file_manager_io","put.gd could not update COSMIC Files' context actions",{{"path",path}}};return change;}
 if(want&&next.split('\n').count(cosmicLine(launcher))==1)change.line=cosmicLine(launcher);
 if(next==text)return change;
 const auto mode=exists?(info.permissions()&modeBits):QFile::Permissions(dataMode);
 const auto backup=exists?commit(path+".putgd-backup",before,mode,false):Write::Unchanged;
 if(backup==Write::Failed||backup==Write::Conflict) {change.error={"file_manager_io","Cannot keep a backup of COSMIC Files' context actions",{{"path",path}}};return change;}
 if(commit(path,next.toUtf8(),mode,false)==Write::Failed) {change.error={"file_manager_io","Cannot write COSMIC Files' context actions",{{"path",path}}};return change;}
 change.changed=true;
 return change;
}
bool cosmicRuns(const QString& path,const QString& launcher) {
 QString text;
 if(!QFileInfo(path).isFile()||!decodeUtf8(readSmall(path),text))return false;
 const auto list=parsePresets(text);
 for(const auto& preset:list.presets)if(runs(preset,launcher))return true;
 return false;
}

QStringList dataDirectories(const FileManagerContext& c) {
 if(!c.dataDirs.isEmpty())return c.dataDirs;
 QStringList directories;
 for(const auto& directory:qEnvironmentVariable("XDG_DATA_DIRS","/usr/local/share:/usr/share").split(':',Qt::SkipEmptyParts))
  if(QFileInfo(directory).isAbsolute())directories<<directory;
 return directories.isEmpty()?QStringList{"/usr/local/share","/usr/share"}:directories;
}
bool inDataDirectories(const QStringList& directories,const QString& relative) {
 for(const auto& directory:directories)if(QFileInfo::exists(QDir(directory).filePath(relative)))return true;
 return false;
}
bool onPath(const FileManagerContext& c,const QString& name) {
 const auto path=c.searchPath.isEmpty()?qEnvironmentVariable("PATH"):c.searchPath;
 for(const auto& directory:path.split(':',Qt::SkipEmptyParts)) {
  const QFileInfo candidate(QDir(directory).filePath(name));
  if(candidate.isFile()&&candidate.isExecutable())return true;
 }
 return false;
}
// put.gd's system plugin for a file manager: a putgd module where it loads them.
bool systemPlugin(const FileManagerContext& c,const QString& relative) {
 const auto roots=c.libraryDirs.isEmpty()?QStringList{"/usr/lib64","/usr/lib/x86_64-linux-gnu","/usr/lib/aarch64-linux-gnu","/usr/lib"}:c.libraryDirs;
 for(const auto& root:roots)
  for(const auto& name:QDir(QDir(root).filePath(relative)).entryList({"*putgd*"},QDir::Files))if(!name.isEmpty())return true;
 return false;
}
// GNOME desktop icons (DING, desktop-icons-ng): Ubuntu's package or its bundled
// copy, or one the user installed.
bool dingInstalled(const FileManagerContext& c) {
 const QString relative="gnome-shell/extensions/ding@rastersoft.com/metadata.json";
 return inDataDirectories(dataDirectories(c)+QStringList{c.dataHome,QDir(c.home).filePath(".local/share")},relative);
}
bool cosmicFilesInstalled(const FileManagerContext& c) {
 return inDataDirectories(dataDirectories(c)+QStringList{c.dataHome},"applications/com.system76.CosmicFiles.desktop")||onPath(c,"cosmic-files");
}

QString substitute(const QString& source,const QHash<QString,QString>& values) {
 QString text;qsizetype position=0;
 auto matches=QRegularExpression("__PUTGD_([A-Z_]+)__").globalMatch(source);
 while(matches.hasNext()) {
  const auto match=matches.next();
  text+=source.mid(position,match.capturedStart()-position)+values.value(match.captured(1),match.captured(0));
  position=match.capturedEnd();
 }
 return text+source.mid(position);
}
QByteArray scriptText(const QString& launcher,const QString& self,const QString& kind,const QString& icon) {
 return substitute(QString::fromUtf8(putgdFileManagerScript),{{"LAUNCHER",shellQuoted(launcher)},{"SELF",shellQuoted(self)},
  {"KIND",shellQuoted(kind)},{"ICON",shellQuoted(icon)}}).toUtf8();
}
// The login script's AppImage discovery (#307), the same in both scripts: an
// update writes the next image beside the old one under a new name.
QString loginDiscovery() {
 const auto source=QString::fromUtf8(putgdLoginStartScript);
 const auto begin=source.indexOf("# The user's private group"),end=source.indexOf("# Remove only this installation's managed startup files");
 return begin<0||end<begin?QString{}:source.mid(begin,end-begin).trimmed();
}
// One of the packaged menu files, run through the given launcher and icon.
QByteArray menuText(const char* source,const QString& exec,const QString& icon) {
 static const QString packaged="/usr/bin/putgd-launcher ";
 auto lines=QString::fromUtf8(source).split('\n');
 bool marked=false;
 for(auto& line:lines) {
  if(line.startsWith("Exec="+packaged))line="Exec="+exec+' '+line.mid(5+packaged.size());
  else if(line=="Icon=putgd"||line=="Icon-Name=putgd")line=line.left(line.indexOf('=')+1)+keyFileEscaped(icon);
  else if(!marked&&line.startsWith('[')) {line+='\n'+marker;marked=true;}
 }
 return lines.join('\n').toUtf8();
}
struct MenuFormat {const char* source;const char* directory;const char* name;bool shell;bool program;};
const MenuFormat menuFormats[]={
 // KIO runs a per-user service menu only when it is executable (KDesktopFile::isAuthorizedDesktopFile).
 {putgdKioServiceMenu,"kio/servicemenus","putgd-upload.desktop",false,true},
 {putgdNemoAction,"nemo/actions","putgd-upload.nemo_action",true,false},
 {putgdThunarSendTo,"Thunar/sendto","putgd-upload.desktop",false,false},
 {putgdFileManagerAction,"file-manager/actions","putgd-upload.desktop",true,false},
 {putgdContract,"contractor","putgd-upload.contract",false,false},
};
QString ownDirectory(const FileManagerContext& c) {return QDir(c.dataRoot).filePath("file-manager");}
QString recordPath(const FileManagerContext& c,const QString& installation) {return QDir(ownDirectory(c)).filePath(installation+".owned");}
QString dingScriptPath(const FileManagerContext& c) {return QDir(c.home).filePath(".local/share/nautilus/scripts/"+scriptName);}
bool isAppImage(const QString& path) {
 const QFileInfo info(path);
 if(!info.isFile()||info.isSymLink()||!info.isExecutable())return false;
 QFile file(path);
 return file.open(QIODevice::ReadOnly)&&file.read(11).mid(8)=="AI\x02";
}
Error ioError(const QString& message,const QString& path) {return {"file_manager_io",message,{{"path",path}}};}
}

QString FileManagerIntegration::label() {return QStringLiteral("Upload with put.gd");}
QString FileManagerIntegration::launcherPath(const FileManagerContext& c) {return QDir(ownDirectory(c)).filePath("putgd-upload");}
QString FileManagerIntegration::cosmicPath(const FileManagerContext& c) {return QDir(c.configHome).filePath("cosmic/com.system76.CosmicFiles/v1/context_actions");}

QJsonObject FileManagerResult::toJson() const {
 QJsonObject json{{"firstRun",firstRun},{"written",QJsonArray::fromStringList(written)},{"removed",QJsonArray::fromStringList(removed)},
  {"skipped",QJsonArray::fromStringList(skipped)},{"restartable",QJsonArray::fromStringList(restartable)}};
 if(!launcher.isEmpty())json["launcher"]=launcher;
 if(error)json["error"]=error.toJson();
 return json;
}

FileManagerResult FileManagerIntegration::apply(const FileManagerContext& c) {
 FileManagerResult result;
 const auto appImage=c.installation=="appimage",system=!appImage;
 const auto installation=appImage?QStringLiteral("appimage"):QStringLiteral("system");
 const auto own=ownDirectory(c),ownedPath=recordPath(c,installation),stampPath=QDir(own).filePath(installation+".stamp");
 for(const auto& root:{c.home,c.dataHome,c.configHome,c.dataRoot})
  if(!QFileInfo(root).isAbsolute()||!singleLine(root)) {result.error={"file_manager_io","File manager integration needs absolute XDG directories",{}};return result;}
 const auto previous=readRecord(ownedPath);
 // The system install's per-user files are not the AppImage's to write or remove.
 const auto systemRecord=appImage?readRecord(recordPath(c,"system")):Record{};
 Record next;
 const auto fail=[&](const Error& error){if(!result.error)result.error=error;};
 const auto keep=[&](const QString& path,const QByteArray& bytes,QFile::Permissions mode) {
  // In its own folder put.gd owns every file; elsewhere only the marked ones.
  switch(commit(path,bytes,mode,!QDir::cleanPath(path).startsWith(QDir::cleanPath(own)+'/'))) {
  case Write::Written:result.written<<path;next.files<<path;break;
  case Write::Unchanged:next.files<<path;break;
  case Write::Conflict:result.skipped<<path;break;
  case Write::Failed:fail(ioError("Cannot write a file manager entry",path));break;
  }
 };
 const auto cosmic=cosmicPath(c);
 const auto applyCosmic=[&](const QString& launcher,bool want) {
  const auto change=mergeCosmic(cosmic,launcher,want,previous.cosmic==cosmic);
  if(change.error) {
   fail(change.error);
   if(previous.cosmic==cosmic) {next.cosmic=previous.cosmic;next.cosmicLine=previous.cosmicLine;}
   return;
  }
  if(change.changed)(want?result.written:result.removed)<<cosmic;
  if(want) {next.cosmic=cosmic;next.cosmicLine=change.line;}
 };
 const auto ding=dingInstalled(c),cosmicFiles=cosmicFilesInstalled(c);
 if(system) {
  // DEB/RPM: per-user files only where a desktop needs them, at every start.
  const auto script=dingScriptPath(c);
  if(ding)keep(script,scriptText(c.systemLauncher,script,"nautilus","putgd"),programMode);
  if(cosmicFiles||previous.cosmic==cosmic)applyCosmic(c.systemLauncher,cosmicFiles);
 } else if(!c.enabled) {
  if(previous.cosmic==cosmic)applyCosmic(launcherPath(c),false);
 } else {
  const QFileInfo image(c.appImage);
  if(!image.isAbsolute()||!image.isFile()||!image.isExecutable()||!singleLine(c.appImage)) {
   result.error={"file_manager_appimage","File manager integration needs an accessible AppImage file",{{"path",c.appImage}}};return result;
  }
  const auto discovery=loginDiscovery();
  if(discovery.isEmpty()) {result.error={"file_manager_template","The AppImage discovery script is missing",{}};return result;}
  const auto configured=QDir::cleanPath(image.absoluteFilePath()),launcher=launcherPath(c);
  next.id=hash(launcher+'\n'+configured);next.image=configured;
  next.updateInformation=LoginStartup::appImageUpdateInformation(configured);
  if(!singleLine(next.updateInformation))next.updateInformation.clear();
  const auto source=QString::fromUtf8(putgdFileManagerLauncher).replace("# __PUTGD_DISCOVERY__",discovery);
  const auto text=substitute(source,{{"CONFIGURED",shellQuoted(configured)},{"UPDATE_INFORMATION",shellQuoted(next.updateInformation)},
   {"SCRIPT",shellQuoted(launcher)},{"RECORD",shellQuoted(ownedPath)},{"DIRECTORY",shellQuoted(own)},{"CGROUP",shellQuoted(c.cgroupFile)},
   {"ID",next.id},{"WATCH_UNIT",shellQuoted("putgd-file-manager-"+hash(launcher))}});
  keep(launcher,text.toUtf8(),programMode);
  if(!next.files.contains(launcher)) {fail(ioError("Cannot write the file manager launcher",launcher));return result;}
  result.launcher=launcher;
  // The image's own icon path is inside a per-launch mount.
  auto icon=QStringLiteral("putgd");
  bool iconRead=false;const auto iconBytes=readSmall(c.iconSource,&iconRead);
  const auto iconPath=QDir(own).filePath("putgd.svg");
  if(iconRead&&!iconBytes.isEmpty()) {
   keep(iconPath,iconBytes,dataMode);
   if(next.files.contains(iconPath))icon=iconPath;
  }
  // A system menu file of put.gd's makes the per-user copy a duplicate.
  const auto directories=dataDirectories(c);
  for(const auto& format:menuFormats) {
   const auto relative=QString::fromLatin1(format.directory)+'/'+QString::fromLatin1(format.name);
   const auto path=QDir(c.dataHome).filePath(relative);
   if(inDataDirectories(directories,relative)) {result.skipped<<path;continue;}
   keep(path,menuText(format.source,format.shell?shellExec(launcher):desktopExec(launcher),icon),format.program?programMode:dataMode);
  }
  // Nautilus Scripts where put.gd's Nautilus plugin is missing; DING reads
  // only $HOME/.local/share/nautilus/scripts, also beside a system plugin.
  QStringList scripts;
  if(!systemPlugin(c,"nautilus/extensions-4"))scripts<<QDir(c.dataHome).filePath("nautilus/scripts/"+scriptName);
  if(ding)scripts<<dingScriptPath(c);
  scripts.removeDuplicates();
  for(const auto& script:scripts) {
   if(systemRecord.files.contains(script)&&managedFile(script)) {result.skipped<<script;continue;}
   keep(script,scriptText(launcher,script,"nautilus",icon),programMode);
  }
  const auto caja=QDir(c.configHome).filePath("caja/scripts/"+scriptName);
  if(systemPlugin(c,"caja/extensions-2.0"))result.skipped<<caja;
  else keep(caja,scriptText(launcher,caja,"caja",icon),programMode);
  // One COSMIC entry: the system install's, when it has one.
  const auto systemCosmic=systemRecord.cosmic==cosmic&&cosmicRuns(cosmic,c.systemLauncher);
  if(systemCosmic)result.skipped<<cosmic;
  if(cosmicFiles&&!systemCosmic)applyCosmic(launcher,true);
  else if(previous.cosmic==cosmic)applyCosmic(launcher,false);
 }
 // Whatever the previous record owned and this pass did not keep. A file the
 // system install's record names is shared (the DING script) and stays.
 for(const auto& path:previous.files) {
  if(next.files.contains(path)||systemRecord.files.contains(path))continue;
  if(removeOwned(path,own))result.removed<<path;
 }
 if(next.files.isEmpty()&&next.cosmic.isEmpty()) {
  if(QFileInfo::exists(ownedPath)&&managedFile(ownedPath)&&!QFile::remove(ownedPath))fail(ioError("Cannot remove the file manager record",ownedPath));
 } else if(commit(ownedPath,recordText(next),dataMode)==Write::Failed)fail(ioError("Cannot write the file manager record",ownedPath));
 // The version stamp: once per installation identity, not after every upgrade.
 if(system||c.enabled) {
  const QFileInfo stamp(stampPath);
  result.firstRun=!stamp.exists();
  if(commit(stampPath,(c.version+'\n').toUtf8(),dataMode,false)==Write::Failed)fail(ioError("Cannot write the file manager version stamp",stampPath));
 }
 if(system&&result.firstRun) {
  const std::pair<const char*,const char*> plugins[]={{"nautilus","nautilus/extensions-4"},{"caja","caja/extensions-2.0"},{"peony","peony-extensions"}};
  for(const auto& [name,relative]:plugins)if(systemPlugin(c,relative)&&onPath(c,name))result.restartable<<name;
 }
 return result;
}

Error FileManagerIntegration::monitor(const FileManagerContext& c,bool enabled) {
#ifdef Q_OS_LINUX
 if(c.installation!="appimage")return {};
 const auto path=launcherPath(c);
 if(!QFileInfo::exists(path))return enabled?Error{"file_manager_monitor","The file manager launcher is missing",{}}:Error{};
 if(!managedFile(path))return {"file_manager_conflict","The file manager launcher is not managed by put.gd",{}};
 QProcess process;process.start("/bin/sh",{path,enabled?"--install-watch":"--stop-watch"});
 if(!process.waitForStarted(3000)||!process.waitForFinished(10000)) {
  process.kill();process.waitForFinished();return {"file_manager_monitor","Cannot configure the file manager integration monitor",{}};
 }
 if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0)
  return {"file_manager_monitor","Cannot configure the file manager integration monitor",{}};
#else
 Q_UNUSED(c);Q_UNUSED(enabled);
#endif
 return {};
}

Error FileManagerIntegration::cleanupIfMissing(const FileManagerContext& c) {
 const auto own=ownDirectory(c),ownedPath=recordPath(c,"appimage");
 const auto record=readRecord(ownedPath);
 if(record.image.isEmpty()||QFileInfo::exists(record.image))return {};
 // An update may have written the next image beside it under a new name.
 if(!record.updateInformation.isEmpty()) {
  QDirIterator images(QFileInfo(record.image).path(),{"putgd-*.AppImage"},QDir::Files);
  while(images.hasNext()) {
   const auto candidate=images.next();
   if(isAppImage(candidate)&&LoginStartup::appImageUpdateInformation(candidate)==record.updateInformation)return {};
  }
 }
 Error error;
 auto image=c;image.installation="appimage";
 if(auto stopped=monitor(image,false))error=stopped;
 if(record.cosmic==cosmicPath(c)) {
  if(auto change=mergeCosmic(record.cosmic,launcherPath(c),false,true);change.error&&!error)error=change.error;
 }
 for(const auto& path:record.files)removeOwned(path,own);
 if(!QFile::remove(ownedPath)&&QFileInfo::exists(ownedPath)&&!error)error=ioError("Cannot remove the file manager record",ownedPath);
 return error;
}
}
