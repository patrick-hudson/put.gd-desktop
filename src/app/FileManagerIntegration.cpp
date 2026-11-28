#include "FileManagerIntegration.h"
#include "FileManagerTemplates.h"
#include "LoginStartScript.h"
#include "LoginStartup.h"
#include "core/Log.h"
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
#include <QDateTime>
#include <sys/stat.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
namespace putgd {
namespace {
const QString marker=QStringLiteral("X-putgd-Managed=true");
constexpr auto programMode=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther;
constexpr auto dataMode=QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther;
constexpr auto modeBits=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|
 QFile::ReadOther|QFile::WriteOther|QFile::ExeOther;
constexpr qint64 textLimit=4*1024*1024;
const QString scriptName=QStringLiteral("Upload with put.gd");
const QString encryptedScriptName=QStringLiteral("Upload encrypted with put.gd"); // #2119

QString hash(const QString& text) {return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(),QCryptographicHash::Sha256).toHex());}
QString bytesHash(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
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
 QString id,image,updateInformation,cosmic;
 QStringList files,cosmicLines; // #2119: one line per COSMIC entry
 // #2132: the SHA-256 of each entry file put.gd wrote outside its own folder, as written; a
 // removal deletes such a file only while it still has these bytes ("hash <hex> <path>").
 QHash<QString,QString> hashes;

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
  else if(line.startsWith("hash ")){const auto rest=value("hash ");const auto space=rest.indexOf(' ');if(space==64)record.hashes.insert(rest.mid(65),rest.left(64));}
  else if(line.startsWith("image "))record.image=value("image ");
  else if(line.startsWith("update "))record.updateInformation=value("update ");
  else if(line.startsWith("cosmic-line "))record.cosmicLines<<value("cosmic-line ");
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
 for(const auto& file:record.files)if(record.hashes.contains(file))text+="hash "+record.hashes.value(file)+' '+file+'\n';
 if(!record.cosmic.isEmpty())text+="cosmic "+record.cosmic+'\n';
 // After the cosmic line: the launcher's shell cleanup removes each as it reads it.
 for(const auto& line:record.cosmicLines)text+="cosmic-line "+line+'\n';
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
// put.gd's entries, in menu order: Upload with put.gd, then Upload encrypted with put.gd (#2119).
QStringList cosmicEntries(const QString& launcher) {
 const auto entry=[&](const QString& label,const QString& options){
  return "(name: "+ronString(label)+", confirm: false, selection: Any, steps: ["+ronString(shellWord(launcher)+" "+options+" -- %F")+"])";
 };
 return {entry(FileManagerIntegration::label(),"--upload"),entry(FileManagerIntegration::encryptedLabel(),"--upload --encrypt")};
}
QString cosmicLine(const QString& entry) {return "    "+entry+",";}
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
struct CosmicChange {Error error;bool changed=false;QStringList lines;};
// Adds put.gd's entries (want) or removes them (only when the record names this
// file), recognising them by the launcher they run. Nothing is written unless the
// value parses, and the result parses back to the same other entries.
CosmicChange mergeCosmic(const QString& path,const QString& launcher,bool want,bool recorded) {
 CosmicChange change;
 const auto unparseable=Error{"cosmic_context_actions_unparseable","Couldn't read COSMIC Files context actions. Left the file unchanged.",{{"path",path}}};
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
 const auto entries=cosmicEntries(launcher);
 QStringList lines;for(const auto& entry:entries)lines<<cosmicLine(entry);
 QList<Preset> mine,others;
 for(const auto& preset:list.presets)(runs(preset,launcher)?mine:others)<<preset;
 auto next=text;
 // Exactly put.gd's entries, in order; anything else (an older single entry, a hand edit) is rewritten.
 auto canonical=mine.size()==entries.size();
 for(qsizetype i=0;canonical&&i<mine.size();++i)canonical=text.mid(mine[i].start,mine[i].end-mine[i].start)==entries[i];
 if(want&&!canonical) {
  for(auto i=mine.size()-1;i>=0;--i)removeSpan(next,mine[i].start,mine[i].comma>=0?mine[i].comma+1:mine[i].end);
  const auto rest=parsePresets(next);
  if(!rest.ok) {change.error=unparseable;return change;}
  if(rest.close<0)next="[\n"+lines.join('\n')+"\n]\n";
  else {
   auto close=rest.close;
   if(!rest.presets.isEmpty()&&rest.presets.last().comma<0) {next.insert(rest.presets.last().end,',');++close;}
   next.insert(close,(close>0&&next[close-1]=='\n'?QString{}:QStringLiteral("\n"))+lines.join('\n')+'\n');
  }
 } else if(!want&&recorded) {
  for(auto i=mine.size()-1;i>=0;--i)removeSpan(next,mine[i].start,mine[i].comma>=0?mine[i].comma+1:mine[i].end);
 }
 // Parse the new value back: every other entry unchanged, exactly ours or none.
 const auto after=parsePresets(next);
 QList<Preset> afterMine,afterOthers;
 for(const auto& preset:after.presets)(runs(preset,launcher)?afterMine:afterOthers)<<preset;
 auto same=after.ok&&afterOthers.size()==others.size();
 for(qsizetype i=0;same&&i<others.size();++i)same=others[i].sameValue(afterOthers[i])&&
  text.mid(others[i].start,others[i].end-others[i].start)==next.mid(afterOthers[i].start,afterOthers[i].end-afterOthers[i].start);
 if(!same||afterMine.size()!=(want?entries.size():(recorded?0:mine.size()))) {change.error={"file_manager_io","Couldn't update COSMIC Files context actions.",{{"path",path}}};return change;}
 if(want) {const auto split=next.split('\n');for(const auto& line:lines)if(split.count(line)==1)change.lines<<line;}
 if(next==text)return change;
 const auto mode=exists?(info.permissions()&modeBits):QFile::Permissions(dataMode);
 const auto backup=exists?commit(path+".putgd-backup",before,mode,false):Write::Unchanged;
 if(backup==Write::Failed||backup==Write::Conflict) {change.error={"file_manager_io","Couldn't back up COSMIC Files context actions.",{{"path",path}}};return change;}
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
// put.gd's system plugin for a file manager: its module, a regular file (not a link), where the
// file manager loads modules (#2119: the same test decides skipping and replacing scripts).
bool systemPlugin(const FileManagerContext& c,const QString& relative) {
 const auto roots=c.libraryDirs.isEmpty()?QStringList{"/usr/lib64","/usr/lib/x86_64-linux-gnu","/usr/lib/aarch64-linux-gnu","/usr/lib"}:c.libraryDirs;
 const auto module=relative.startsWith("caja/")?QStringLiteral("libputgd-caja.so"):relative.startsWith("peony")?QStringLiteral("libputgd-peony.so"):QStringLiteral("libputgd-nautilus.so");
 for(const auto& root:roots) {
  const QFileInfo file(QDir(QDir(root).filePath(relative)).filePath(module));
  if(file.isFile()&&!file.isSymLink())return true;
 }
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
 static const QRegularExpression placeholder("__PUTGD_([A-Z_]+)__");
 auto matches=placeholder.globalMatch(source);
 while(matches.hasNext()) {
  const auto match=matches.next();
  text+=source.mid(position,match.capturedStart()-position)+values.value(match.captured(1),match.captured(0));
  position=match.capturedEnd();
 }
 return text+source.mid(position);
}
QByteArray scriptText(const QString& launcher,const QString& self,const QString& kind,const QString& icon,bool encrypt) {
 return substitute(QString::fromUtf8(putgdFileManagerScript),{{"LAUNCHER",shellQuoted(launcher)},{"SELF",shellQuoted(self)},
  {"KIND",shellQuoted(kind)},{"ICON",shellQuoted(icon)},{"ENCRYPT",encrypt?QStringLiteral("true"):QStringLiteral("false")}}).toUtf8();
}
// A script's entry: an encrypted one by its name (#2119).
bool encryptedScript(const QString& path) {return QFileInfo(path).fileName()==encryptedScriptName;}
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
// legacy: an earlier template of the same file, which older AppImages wrote (#2119).
struct MenuFormat {const char* source;const char* directory;const char* name;bool shell;bool program;const char* legacy=nullptr;};
const MenuFormat menuFormats[]={
 // KIO runs a per-user service menu only when it is executable (KDesktopFile::isAuthorizedDesktopFile).
 {putgdKioServiceMenu,"kio/servicemenus","putgd-upload.desktop",false,true,putgdKioServiceMenu1105},
 {putgdNemoAction,"nemo/actions","putgd-upload.nemo_action",true,false},
 {putgdThunarSendTo,"Thunar/sendto","putgd-upload.desktop",false,false},
 {putgdFileManagerAction,"file-manager/actions","putgd-upload.desktop",true,false},
 {putgdContract,"contractor","putgd-upload.contract",false,false},
 // Upload encrypted with put.gd (#2119): KIO's is a second action in its one service menu.
 {putgdNemoActionEncrypted,"nemo/actions","putgd-upload-encrypted.nemo_action",true,false},
 {putgdThunarSendToEncrypted,"Thunar/sendto","putgd-upload-encrypted.desktop",false,false},
 {putgdFileManagerActionEncrypted,"file-manager/actions","putgd-upload-encrypted.desktop",true,false},
 {putgdContractEncrypted,"contractor","putgd-upload-encrypted.contract",false,false},
};
QString ownDirectory(const FileManagerContext& c) {return QDir(c.dataRoot).filePath("file-manager");}

// #2132: the package's own copy of a menu file. The highest-priority XDG data folder that has
// the file decides (S5): it must be a system folder (not the user's own under any name), owned
// like its file by root, and the file a packaged put.gd menu file, its Exec running
// /usr/bin/putgd-launcher, that offers both entries: in itself (KIO's two actions) or with its
// encrypted or plain sibling beside it (L5). The old plain-only KIO menu doesn't (N2). The
// AppImage skips its copy for one, and the package's start moves the AppImage's copy aside for one.
bool packagedExec(const QString& path,bool encrypted) {
 const QFileInfo file(path);
 if(!file.isFile()||file.isSymLink())return false;
 const auto text=QString::fromUtf8(readSmall(path));
 return text.contains(encrypted?QStringLiteral("\nExec=/usr/bin/putgd-launcher --upload --encrypt -- "):QStringLiteral("\nExec=/usr/bin/putgd-launcher --upload -- "));
}
bool systemCopy(const FileManagerContext& c,const MenuFormat& format) {
 const auto relative=QString::fromLatin1(format.directory)+'/'+QString::fromLatin1(format.name);
 const auto userRoot=QFileInfo(c.dataHome).canonicalFilePath();
 for(const auto& directory:dataDirectories(c)) {
  const QFileInfo file(QDir(directory).filePath(relative));
  if(!file.exists()&&!file.isSymLink())continue;
  const auto root=QFileInfo(directory).canonicalFilePath();
  if(root.isEmpty()||root==userRoot||root.startsWith(userRoot+'/'))return false;
  if(!file.isFile()||file.isSymLink()||file.ownerId()!=c.systemOwner||QFileInfo(directory).ownerId()!=c.systemOwner)return false;
  const auto name=QString::fromLatin1(format.name);const bool encrypted=name.contains(QStringLiteral("-encrypted"));
  if(!packagedExec(file.filePath(),encrypted))return false;
  if(QString::fromLatin1(format.directory)=="kio/servicemenus")return packagedExec(file.filePath(),true);
  auto sibling=name;
  if(encrypted)sibling.remove(QStringLiteral("-encrypted"));else sibling.insert(QStringLiteral("putgd-upload").size(),QStringLiteral("-encrypted"));
  const auto siblingPath=QDir(QDir(directory).filePath(QString::fromLatin1(format.directory))).filePath(sibling);
  return QFileInfo(siblingPath).ownerId()==c.systemOwner&&packagedExec(siblingPath,!encrypted);
 }
 return false;
}
// A path's canonical form, also for a file that doesn't exist yet (its folder's canonical form).
QString canonicalPath(const QString& path) {
 const QFileInfo info(path);
 if(const auto canonical=info.canonicalFilePath();!canonical.isEmpty())return canonical;
 const auto folder=QFileInfo(info.absolutePath()).canonicalFilePath();
 return folder.isEmpty()?QDir::cleanPath(info.absoluteFilePath()):folder+'/'+info.fileName();
}
// root/relative as a regular file inside root: no folder between them is a link, and its
// canonical folder lies inside root's canonical form.
bool contained(const QString& root,const QString& relative) {
 const auto canonicalRoot=QFileInfo(root).canonicalFilePath();
 if(canonicalRoot.isEmpty()||relative.startsWith('/')||relative.contains(QStringLiteral("..")))return false;
 auto folder=QDir::cleanPath(root);
 const auto parts=relative.split('/',Qt::SkipEmptyParts);
 for(qsizetype i=0;i+1<parts.size();++i) {
  folder+='/'+parts[i];
  const QFileInfo step(folder);
  if(step.isSymLink()||!step.isDir())return false;
 }
 const QFileInfo file(QDir(root).filePath(relative));
 if(file.isSymLink()||!file.isFile())return false;
 return QFileInfo(file.absolutePath()).canonicalFilePath().startsWith(canonicalRoot+'/')||QFileInfo(file.absolutePath()).canonicalFilePath()==canonicalRoot;
}
// A regular file's identity; `mine` only when this user owns it (#2132 N6).
struct Identity {dev_t device=0;ino_t inode=0;off_t size=-1;timespec modified{};mode_t mode=0;bool mine=false;
 bool same(const Identity& o) const {return mine&&o.mine&&device==o.device&&inode==o.inode&&size==o.size&&modified.tv_sec==o.modified.tv_sec&&modified.tv_nsec==o.modified.tv_nsec;}};
Identity identity(const QString& path) {struct stat s{};Identity out;if(::lstat(QFile::encodeName(path).constData(),&s)==0&&S_ISREG(s.st_mode)){out.device=s.st_dev;out.inode=s.st_ino;out.size=s.st_size;out.modified=s.st_mtim;out.mode=s.st_mode&07777;out.mine=s.st_uid==::getuid();}return out;}
QString recordPath(const FileManagerContext& c,const QString& installation) {return QDir(ownDirectory(c)).filePath(installation+".owned");}
QStringList dingScriptPaths(const FileManagerContext& c) {
 const QDir scripts(QDir(c.home).filePath(".local/share/nautilus/scripts"));
 return {scripts.filePath(scriptName),scripts.filePath(encryptedScriptName)};
}
bool isAppImage(const QString& path) {
 const QFileInfo info(path);
 if(!info.isFile()||info.isSymLink()||!info.isExecutable())return false;
 QFile file(path);
 return file.open(QIODevice::ReadOnly)&&file.read(11).mid(8)=="AI\x02";
}
Error ioError(const QString& message,const QString& path) {return {"file_manager_io",message,{{"path",path}}};}
}

namespace {
// #2132 L4: the spellings of one path that name the same file: as given, canonical, and with the
// home folder spelled either way (/home and /var/home on Atomic desktops), and as the AppImage's
// record spelled it (`known`). Only spellings that resolve to the same place are kept.
QStringList spellings(const FileManagerContext& c,const QString& path,const QStringList& known={}) {
 const auto canonical=canonicalPath(path);
 QStringList out{QDir::cleanPath(path),canonical};
 // R2-3: a path the AppImage's record names, as it spelled it, when it is this same file.
 for(const auto& recorded:known)if(canonicalPath(recorded)==canonical)out<<QDir::cleanPath(recorded);
 const auto home=QDir::cleanPath(c.home),canonicalHome=canonicalPath(c.home);
 for(const auto& [from,to]:{std::pair{home,canonicalHome},std::pair{canonicalHome,home},std::pair{QStringLiteral("/home"),QStringLiteral("/var/home")},std::pair{QStringLiteral("/var/home"),QStringLiteral("/home")}})
  for(const auto& known:QStringList(out))if(!from.isEmpty()&&known.startsWith(from+'/'))out<<to+known.mid(from.size());
 out.removeDuplicates();
 out.removeIf([&](const QString& spelling){return canonicalPath(spelling)!=canonical;});
 return out;
}
// #2132: every byte-exact file put.gd has written for a per-user entry at `path`, with `launcher`
// (the AppImage's stable launcher or the package's): each shipped template, current and earlier,
// with only the launcher, icon and the script's own path filled in, each in any of its spellings.
QList<QByteArray> renderings(const FileManagerContext& c,const QString& path,const QString& launcher,const MenuFormat* format,const QString& kind,const QStringList& known={}) {
 QStringList icons=spellings(c,QDir(ownDirectory(c)).filePath("putgd.svg"),known);icons<<QStringLiteral("putgd");
 const auto launchers=spellings(c,launcher,known),selves=spellings(c,path,known);
 QList<QByteArray> out;
 for(const auto& spelled:launchers)for(const auto& icon:icons) {
  if(format) {
   const auto exec=format->shell?shellExec(spelled):desktopExec(spelled);
   out<<menuText(format->source,exec,icon);
   if(format->legacy)out<<menuText(format->legacy,exec,icon);
  } else for(const auto& self:selves) {
   out<<scriptText(spelled,self,kind,icon,encryptedScript(path));
   if(!encryptedScript(path))
    for(const char* legacy:{putgdFileManagerScript987483d,putgdFileManagerScriptFb3a313})
     out<<substitute(QString::fromUtf8(legacy),{{"LAUNCHER",shellQuoted(spelled)},{"SELF",shellQuoted(self)},{"KIND",shellQuoted(kind)},{"ICON",shellQuoted(icon)}}).toUtf8();
  }
 }
 return out;
}
// Whether `bytes` is a file put.gd wrote at `path` with `launcher` (the AppImage's stable launcher
// or the package's): an exact rendering, or for the AppImage its recorded hash.
bool generated(const FileManagerContext& c,const QString& path,const QByteArray& bytes,const MenuFormat* format,const QString& kind,const QString& launcher,const QStringList& known={},const QString& recordedHash={}) {
 if(!recordedHash.isEmpty()&&recordedHash==bytesHash(bytes))return true;
 return renderings(c,path,launcher,format,kind,known).contains(bytes);
}
// Where a per-user entry file lives: its root folder, the backup area named after it, the
// relative path, and its format (null for a Scripts entry, then `kind` names the file manager).
struct Place {QString root,area,relative;const MenuFormat* format=nullptr;QString kind;bool valid() const {return !root.isEmpty();}};
Place placeOf(const FileManagerContext& c,const QString& path) {
 const auto at=canonicalPath(path);const auto same=[&](const QString& candidate){return canonicalPath(candidate)==at;};
 for(const auto& format:menuFormats) {
  const auto relative=QString::fromLatin1(format.directory)+'/'+QString::fromLatin1(format.name);
  if(same(QDir(c.dataHome).filePath(relative)))return {c.dataHome,QStringLiteral("data"),relative,&format,{}};
 }
 for(const auto& name:{scriptName,encryptedScriptName}) {
  if(same(QDir(c.dataHome).filePath("nautilus/scripts/"+name)))return {c.dataHome,QStringLiteral("data"),"nautilus/scripts/"+name,nullptr,QStringLiteral("nautilus")};
  if(same(QDir(c.home).filePath(".local/share/nautilus/scripts/"+name)))return {c.home,QStringLiteral("home"),".local/share/nautilus/scripts/"+name,nullptr,QStringLiteral("nautilus")};
  if(same(QDir(c.configHome).filePath("caja/scripts/"+name)))return {c.configHome,QStringLiteral("config"),"caja/scripts/"+name,nullptr,QStringLiteral("caja")};
 }
 return {};
}
// Moves aside put.gd's per-user entry files, recoverably, under
// <state>/file-manager-replaced/<UTC time>/<area>/<relative path>, with their mode. One mover per pass.
class Mover {
public:
 Mover(const FileManagerContext& c,const char* reason):reason_(reason) {
  const auto state=c.stateRoot.isEmpty()?logging::stateRoot():c.stateRoot;
  const auto stamp=c.replacedStamp.isEmpty()?QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")):c.replacedStamp;
  backup_=QDir(state).filePath("file-manager-replaced/"+stamp);
 }
 // Moves `path` (at `place`) aside if it is still `before`; never overwrites or removes anything
 // already at the target. An empty answer: nothing moved, and the file is where it was.
 QString move(const QString& path,const Place& place,const Identity& before,const QByteArray& bytes) {
  const auto target=QDir(backup_).filePath(place.area+'/'+place.relative);
  if(!QDir().mkpath(QFileInfo(target).path()))return {};
  if(!before.same(identity(path)))return {};
  const auto from=QFile::encodeName(path),to=QFile::encodeName(target);
  // A hard link never replaces an existing target; the original goes only once the link stands.
  if(::link(from.constData(),to.constData())==0) {
   if(syncFolder(target)&&::unlink(from.constData())==0)return logged(path,target);
   ::unlink(to.constData());return {};
  }
  if(errno==EEXIST)return {};
  // Another filesystem (or one without links): a new file of our own, checked, then the original.
  QFile copy(target);
  if(!copy.open(QIODevice::WriteOnly|QIODevice::NewOnly))return {};
  const bool written=copy.write(bytes)==bytes.size()&&copy.flush()&&::fchmod(copy.handle(),before.mode)==0&&::fsync(copy.handle())==0;
  copy.close();
  if(!written||readSmall(target)!=bytes||!syncFolder(target)||!before.same(identity(path))||::unlink(from.constData())!=0) {::unlink(to.constData());return {};}
  return logged(path,target);
 }
private:
 // The backup's folder entry is on disk before the original goes (#2132 S8).
 static bool syncFolder(const QString& file) {
  const int folder=::open(QFile::encodeName(QFileInfo(file).path()).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  if(folder<0)return false;
  const bool synced=::fsync(folder)==0;::close(folder);return synced;
 }
 QString logged(const QString& path,const QString& target) {logging::info(logApp(),reason_,{{"path",path},{"movedTo",target}});return target;}
 const char* reason_;QString backup_;
};
// What retiring an entry file did (#2132 S6).
enum class Retired {Removed,Moved,Left,Failed};
// Retires a per-user entry file put.gd no longer keeps there (#2132 N1): one exactly as put.gd
// wrote it (with the retiring install's own launcher, or its recorded bytes) is removed; a file the
// package wrote is never removed by the AppImage (R2-2); one changed since is moved aside
// with a mover, never deleted, and left in place without one. A file outside the known places, or
// not put.gd's, is left; one that should go and can't is Failed, so its record line stays.
Retired retire(const FileManagerContext& c,const QString& path,const QString& own,Mover* mover,const QString& ownLauncher,const QStringList& known,const QString& recordedHash={}) {
 if(QDir::cleanPath(path).startsWith(QDir::cleanPath(own)+'/'))return removeOwned(path,own)||!QFileInfo::exists(path)?Retired::Removed:Retired::Failed;
 const QFileInfo info(path);
 if(!info.exists()&&!info.isSymLink())return Retired::Removed;
 const auto place=placeOf(c,path);
 if(!place.valid()||!contained(place.root,place.relative)||!managedFile(path))return Retired::Left;
 const auto before=identity(path);bool read=false;const auto bytes=readSmall(path,&read);
 if(!read||!before.mine)return Retired::Left;
 // R2-2: the AppImage never removes what the package wrote, whatever its record still says.
 if(ownLauncher!=c.systemLauncher&&generated(c,path,bytes,place.format,place.kind,c.systemLauncher,known))return Retired::Left;
 if(generated(c,path,bytes,place.format,place.kind,ownLauncher,known,recordedHash))return before.same(identity(path))&&QFile::remove(path)?Retired::Removed:Retired::Failed;
 if(!mover)return Retired::Left;
 return mover->move(path,place,before,bytes).isEmpty()?Retired::Failed:Retired::Moved;
}
// Drops `paths` (compared canonically) from the AppImage's record, with their hashes; every other
// line stays as it was (#2132 S2, S7).
void dropFromAppImageRecord(const FileManagerContext& c,const QStringList& paths,const std::function<void(const Error&)>& fail) {
 const auto recordFile=recordPath(c,"appimage");
 if(paths.isEmpty()||!managedFile(recordFile))return;
 QStringList canonical;for(const auto& path:paths)canonical<<canonicalPath(path);
 bool read=false;const auto lines=QString::fromUtf8(readSmall(recordFile,&read)).split('\n');
 if(!read)return;
 QStringList kept;
 for(const auto& line:lines) {
  QString named;
  if(line.startsWith("file "))named=line.mid(5);
  else if(line.startsWith("hash ")&&line.size()>70&&line[69]==' ')named=line.mid(70);
  if(named.isEmpty()||!canonical.contains(canonicalPath(named)))kept<<line;
 }
 if(kept.size()!=lines.size()&&commit(recordFile,kept.join('\n').toUtf8(),dataMode)==Write::Failed)fail(ioError("Cannot update the AppImage's file manager record",recordFile));
}
// The package's start moves an AppImage's per-user copy aside where the package has its own:
// only a file whose bytes are exactly what an AppImage wrote, inside the user's folder with no
// link on the way. The AppImage's record drops it, keeping every other line.
void moveAsideAppImageCopies(const FileManagerContext& c,FileManagerResult& result,const std::function<void(const Error&)>& fail,const QStringList& known) {
 QList<Place> candidates;
 for(const auto& format:menuFormats)
  if(systemCopy(c,format))candidates.append({c.dataHome,QStringLiteral("data"),QString::fromLatin1(format.directory)+'/'+QString::fromLatin1(format.name),&format,{}});
 // Its Scripts entries duplicate the package's Files and Caja plugins; the DING scripts are the package's own.
 QStringList dingScripts;
 if(dingInstalled(c))for(const auto& script:dingScriptPaths(c))dingScripts<<canonicalPath(script);
 for(const auto& name:{scriptName,encryptedScriptName}) {
  if(systemPlugin(c,"nautilus/extensions-4")&&!dingScripts.contains(canonicalPath(QDir(c.dataHome).filePath("nautilus/scripts/"+name))))
   candidates.append({c.dataHome,QStringLiteral("data"),"nautilus/scripts/"+name,nullptr,QStringLiteral("nautilus")});
  if(systemPlugin(c,"caja/extensions-2.0"))candidates.append({c.configHome,QStringLiteral("config"),"caja/scripts/"+name,nullptr,QStringLiteral("caja")});
 }
 Mover mover(c,"Moved aside an AppImage file manager entry that the installed package replaces");
 QStringList moved;
 for(const auto& place:std::as_const(candidates)) {
  const auto path=QDir(place.root).filePath(place.relative);
  if(!contained(place.root,place.relative))continue;
  const auto before=identity(path);bool read=false;const auto bytes=readSmall(path,&read);
  if(!read||!before.mine||!renderings(c,path,FileManagerIntegration::launcherPath(c),place.format,place.kind,known).contains(bytes))continue;
  // A move that fails leaves the file where it was and reports it; the next start tries again.
  if(mover.move(path,place,before,bytes).isEmpty()) {fail(ioError("Cannot move a replaced file manager entry aside",path));continue;}
  result.removed<<path;moved<<path;
 }
 // The AppImage's record no longer names them; every other line stays as it was.
 dropFromAppImageRecord(c,moved,fail);
}
}
namespace {
// #2132 R2-1: an AppImage's stable launcher written before entries carried hashes removes any
// marked entry when the image is gone, edited ones included. The package's start replaces one
// that is exactly such a launcher (any shipped version, with its own values) with the current,
// hash-aware launcher, keeping its paths, image and identity; and gives the AppImage's record a
// hash for each entry that is still exactly what the AppImage wrote, so the new launcher still
// removes those. A launcher that isn't an exact earlier rendering is left alone, and logged.
void upgradeAppImageLauncher(const FileManagerContext& c,const QStringList& known,const std::function<void(const Error&)>& fail) {
 const auto path=FileManagerIntegration::launcherPath(c);
 if(!managedFile(path))return;
 const auto text=QString::fromUtf8(readSmall(path));
 // Its own values, as it holds them: each assignment's first line, already shell-quoted.
 QHash<QString,QString> values;
 const QList<std::pair<QString,QString>> names{{"configured","CONFIGURED"},{"update_information","UPDATE_INFORMATION"},{"script","SCRIPT"},{"record","RECORD"},
  {"own_directory","DIRECTORY"},{"user_runtime","USER_RUNTIME"},{"cgroup","CGROUP"},{"watch_unit","WATCH_UNIT"}};
 for(const auto& line:text.split('\n')) {
  for(const auto& [name,key]:names)if(!values.contains(key)&&line.startsWith(name+'='))values.insert(key,line.mid(name.size()+1));
  if(!values.contains("ID")&&line.startsWith("integration_id='")&&line.endsWith('\''))values.insert("ID",line.mid(16,line.size()-17));
 }
 const auto discovery=loginDiscovery();
 const auto render=[&](const char* source){return substitute(QString::fromUtf8(source).replace("# __PUTGD_DISCOVERY__",discovery),values).toUtf8();};
 if(!values.contains("USER_RUNTIME"))values.insert("USER_RUNTIME",shellQuoted(c.userRuntime.isEmpty()?QStringLiteral("/run/user/%1").arg(::getuid()):c.userRuntime));
 const auto current=render(putgdFileManagerLauncher);
 if(text.toUtf8()!=current) {
  bool earlier=false;
  for(const char* legacy:{putgdFileManagerLauncher987483d,putgdFileManagerLauncher2f1bc55,putgdFileManagerLauncher217621f,putgdFileManagerLauncher4631e27})earlier|=render(legacy)==text.toUtf8();
  if(!earlier||values.size()<9||discovery.isEmpty()) {logging::info(logApp(),"Left the AppImage's file manager launcher as it is: it isn't one put.gd wrote",{{"path",path}});return;}
  // Its own mode stays (R3-1).
  if(commit(path,current,QFileInfo(path).permissions()&modeBits)==Write::Failed){fail(ioError("Cannot update the AppImage's file manager launcher",path));return;}
  logging::info(logApp(),"Updated the AppImage's file manager launcher so its removal keeps edited entries",{{"path",path}});
 }
 // Hashes for the record's entries that are still exactly what the AppImage wrote. This runs at
 // every start while the launcher is the current one, so a record write that failed is retried;
 // an entry that already has a hash gets no second one (R3-2).
 const auto recordFile=recordPath(c,"appimage");
 const auto record=readRecord(recordFile);
 if(!managedFile(recordFile))return;
 QString added;
 for(const auto& file:record.files) {
  if(record.hashes.contains(file)||QDir::cleanPath(file).startsWith(QDir::cleanPath(ownDirectory(c))+'/'))continue;
  const auto place=placeOf(c,file);bool read=false;const auto bytes=readSmall(file,&read);
  if(place.valid()&&read&&managedFile(file)&&generated(c,file,bytes,place.format,place.kind,path,known))added+="hash "+bytesHash(bytes)+' '+file+'\n';
 }
 if(added.isEmpty())return;
 auto updated=readSmall(recordFile);if(!updated.endsWith('\n'))updated+='\n';
 if(commit(recordFile,updated+added.toUtf8(),dataMode)==Write::Failed)fail(ioError("Cannot update the AppImage's file manager record",recordFile));
}
}
QString FileManagerIntegration::label() {return QStringLiteral("Upload with put.gd");}
QString FileManagerIntegration::encryptedLabel() {return QStringLiteral("Upload encrypted with put.gd");}
QByteArray FileManagerIntegration::earlierLauncherForTesting(const FileManagerContext& c) {
 const auto text=QString::fromUtf8(readSmall(launcherPath(c)));QHash<QString,QString> values;
 const QList<std::pair<QString,QString>> names{{"configured","CONFIGURED"},{"update_information","UPDATE_INFORMATION"},{"script","SCRIPT"},{"record","RECORD"},
  {"own_directory","DIRECTORY"},{"user_runtime","USER_RUNTIME"},{"cgroup","CGROUP"},{"watch_unit","WATCH_UNIT"}};
 for(const auto& line:text.split('\n')) {
  for(const auto& [name,key]:names)if(!values.contains(key)&&line.startsWith(name+'='))values.insert(key,line.mid(name.size()+1));
  if(!values.contains("ID")&&line.startsWith("integration_id='")&&line.endsWith('\''))values.insert("ID",line.mid(16,line.size()-17));
 }
 return substitute(QString::fromUtf8(putgdFileManagerLauncher4631e27).replace("# __PUTGD_DISCOVERY__",loginDiscovery()),values).toUtf8();
}
QByteArray FileManagerIntegration::packagedMenu(const QString& relative) {
 for(const auto& format:menuFormats)if(relative==QString::fromLatin1(format.directory)+'/'+QString::fromLatin1(format.name))return QByteArray(format.source);
 return {};
}
QStringList FileManagerIntegration::systemDirectories(const FileManagerContext& c) {
 QStringList directories;
 for(const auto& root:dataDirectories(c))for(const auto& format:menuFormats)directories<<QDir(root).filePath(QString::fromLatin1(format.directory));
 const auto roots=c.libraryDirs.isEmpty()?QStringList{"/usr/lib64","/usr/lib/x86_64-linux-gnu","/usr/lib/aarch64-linux-gnu","/usr/lib"}:c.libraryDirs;
 for(const auto& root:roots)for(const auto* relative:{"nautilus/extensions-4","caja/extensions-2.0"})directories<<QDir(root).filePath(QString::fromLatin1(relative));
 directories.removeDuplicates();
 directories.removeIf([](const QString& directory){return !QFileInfo(directory).isDir();});
 return directories;
}
QString FileManagerIntegration::restartScript(const QString& name) {
 // PCManFM has no quit option; both quit cleanly on SIGTERM and their sessions
 // don't bring back a clean exit. One that draws the desktop or runs as a daemon
 // comes back with its own arguments and display ($1 names the process; another session's stays).
 if(name=="pcmanfm"||name=="pcmanfm-qt")return QStringLiteral(
  "v(){ tr '\\0' '\\n' </proc/$1/environ | sed -n \"s/^$2=//p\"; }; "
  "p=; for q in $(pgrep -u \"$(id -u)\" -x \"$1\"); do w=$(v $q WAYLAND_DISPLAY); [ \"$(v $q DISPLAY)\" = \"$DISPLAY\" ] && [ \"${w:-wayland-0}\" = \"${WAYLAND_DISPLAY:-wayland-0}\" ] && p=$q; done; [ -n \"$p\" ] || exit 0; "
  "a=$(tr '\\0' '\\n' </proc/$p/cmdline); for n in WAYLAND_DISPLAY XDG_SESSION_TYPE QT_QPA_PLATFORM; do x=$(v $p $n); unset \"$n\"; [ -z \"$x\" ] || export \"$n=$x\"; done; kill \"$p\"; "
  "i=0; while kill -0 \"$p\" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; "
  "printf '%s\\n' \"$a\" | grep -qx -e --desktop -e --daemon-mode -e -d || exit 0; "
  "set -f; IFS='\n'; set -- $a; exec \"$@\"");
 return {};
}
QString FileManagerIntegration::launcherPath(const FileManagerContext& c) {return QDir(ownDirectory(c)).filePath("putgd-upload");}
QString FileManagerIntegration::cosmicPath(const FileManagerContext& c) {return QDir(c.configHome).filePath("cosmic/com.system76.CosmicFiles/v1/context_actions");}

QJsonObject FileManagerResult::toJson() const {
 QJsonObject json{{"firstRun",firstRun},{"announce",announce},{"written",QJsonArray::fromStringList(written)},{"removed",QJsonArray::fromStringList(removed)},
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
 // A removed DEB/RPM leaves its record behind; it no longer shares anything then.
 const auto systemRecord=appImage&&QFileInfo(c.systemLauncher).isExecutable()?readRecord(recordPath(c,"system")):Record{};
 Record next;
 const auto fail=[&](const Error& error){if(!result.error)result.error=error;};
 const auto keep=[&](const QString& path,const QByteArray& bytes,QFile::Permissions mode) {
  // In its own folder put.gd owns every file; elsewhere only the marked ones.
  const bool outside=!QDir::cleanPath(path).startsWith(QDir::cleanPath(own)+'/');
  switch(commit(path,bytes,mode,outside)) {
  case Write::Written:result.written<<path;next.files<<path;if(outside)next.hashes.insert(path,bytesHash(bytes));break;
  case Write::Unchanged:next.files<<path;if(outside)next.hashes.insert(path,bytesHash(bytes));break;
  case Write::Conflict:result.skipped<<path;break;
  case Write::Failed:fail(ioError("Cannot write a file manager entry",path));break;
  }
 };
 const auto cosmic=cosmicPath(c);
 const auto applyCosmic=[&](const QString& launcher,bool want) {
  const auto change=mergeCosmic(cosmic,launcher,want,previous.cosmic==cosmic);
  if(change.error) {
   fail(change.error);
   if(previous.cosmic==cosmic) {next.cosmic=previous.cosmic;next.cosmicLines=previous.cosmicLines;}
   return;
  }
  if(change.changed)(want?result.written:result.removed)<<cosmic;
  if(want) {next.cosmic=cosmic;next.cosmicLines=change.lines;}
 };
 const auto ding=dingInstalled(c),cosmicFiles=cosmicFilesInstalled(c);
 if(system) {
  // #2119: an AppImage's per-user copy of a menu file has the same name as the package's own, and
  // a user file shadows the system one, so an older copy hides the package's entries.
  const auto known=readRecord(recordPath(c,"appimage")).files;
  upgradeAppImageLauncher(c,known,fail);
  moveAsideAppImageCopies(c,result,fail,known);
  // DEB/RPM: per-user files only where a desktop needs them, at every start. A DING script that
  // isn't exactly what put.gd wrote (an edited copy) is moved aside before the package's
  // replaces it (#2132 S1); the AppImage's record stops naming the scripts the package now
  // keeps, so its removal never takes them (S2, L2).
  if(ding) {
   Mover mover(c,"Moved aside a changed file manager script before the package replaced it");
   QStringList taken;
   for(const auto& script:dingScriptPaths(c)) {
    const auto wanted=scriptText(c.systemLauncher,script,"nautilus","putgd",encryptedScript(script));
    if(managedFile(script)&&readSmall(script)!=wanted) {
     const auto before=identity(script);bool read=false;const auto bytes=readSmall(script,&read);
     const auto place=placeOf(c,script);
     if(!read||!place.valid()||!contained(place.root,place.relative))continue;
     const auto exact=generated(c,script,bytes,nullptr,QStringLiteral("nautilus"),launcherPath(c),known)||generated(c,script,bytes,nullptr,QStringLiteral("nautilus"),c.systemLauncher,known);
     if(!exact&&mover.move(script,place,before,bytes).isEmpty()) {fail(ioError("Cannot move a changed file manager script aside",script));continue;}
    }
    keep(script,wanted,programMode);
    if(next.files.contains(script))taken<<script;
   }
   dropFromAppImageRecord(c,taken,fail);
  }
  if(cosmicFiles||previous.cosmic==cosmic)applyCosmic(c.systemLauncher,cosmicFiles);
 } else if(!c.enabled) {
  // Stop the removal watch while its launcher still exists to run --stop-watch.
  if(auto stopped=monitor(c,false))fail(stopped);
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
   {"SCRIPT",shellQuoted(launcher)},{"RECORD",shellQuoted(ownedPath)},{"DIRECTORY",shellQuoted(own)},{"CGROUP",shellQuoted(c.cgroupFile)},{"USER_RUNTIME",shellQuoted(c.userRuntime.isEmpty()?QStringLiteral("/run/user/%1").arg(::getuid()):c.userRuntime)},
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
  for(const auto& format:menuFormats) {
   const auto relative=QString::fromLatin1(format.directory)+'/'+QString::fromLatin1(format.name);
   const auto path=QDir(c.dataHome).filePath(relative);
   if(systemCopy(c,format)) {result.skipped<<path;continue;}
   keep(path,menuText(format.source,format.shell?shellExec(launcher):desktopExec(launcher),icon),format.program?programMode:dataMode);
  }
  // Nautilus Scripts where put.gd's Nautilus plugin is missing; DING reads
  // only $HOME/.local/share/nautilus/scripts, also beside a system plugin.
  QStringList scripts;
  if(!systemPlugin(c,"nautilus/extensions-4"))for(const auto& name:{scriptName,encryptedScriptName})scripts<<QDir(c.dataHome).filePath("nautilus/scripts/"+name);
  if(ding)scripts<<dingScriptPaths(c);
  scripts.removeDuplicates();
  for(const auto& script:std::as_const(scripts)) {
   if(std::any_of(systemRecord.files.cbegin(),systemRecord.files.cend(),[&](const QString& shared){return canonicalPath(shared)==canonicalPath(script);})&&managedFile(script)) {result.skipped<<script;continue;}
   keep(script,scriptText(launcher,script,"nautilus",icon,encryptedScript(script)),programMode);
  }
  for(const auto& name:{scriptName,encryptedScriptName}) {
   const auto caja=QDir(c.configHome).filePath("caja/scripts/"+name);
   if(systemPlugin(c,"caja/extensions-2.0"))result.skipped<<caja;
   else keep(caja,scriptText(launcher,caja,"caja",icon,encryptedScript(caja)),programMode);
  }
  // One COSMIC entry: the system install's, when it has one.
  const auto systemCosmic=systemRecord.cosmic==cosmic&&cosmicRuns(cosmic,c.systemLauncher);
  if(systemCosmic)result.skipped<<cosmic;
  if(cosmicFiles&&!systemCosmic)applyCosmic(launcher,true);
  else if(previous.cosmic==cosmic)applyCosmic(launcher,false);
 }
 // Whatever the previous record owned and this pass did not keep. A file the
 // system install's record names is shared (the DING script) and stays, under any of its names
 // (#2132 N3). One changed since put.gd wrote it is moved aside rather than deleted (N1).
 {
  QStringList kept,shared;for(const auto& path:next.files)kept<<canonicalPath(path);for(const auto& path:systemRecord.files)shared<<canonicalPath(path);
  Mover mover(c,"Moved aside a changed file manager entry put.gd no longer uses");
  for(const auto& path:previous.files) {
   const auto canonical=canonicalPath(path);
   if(next.files.contains(path)||kept.contains(canonical)||shared.contains(canonical))continue;
   switch(retire(c,path,own,&mover,appImage?launcherPath(c):c.systemLauncher,previous.files,previous.hashes.value(path))) {
   case Retired::Removed:case Retired::Moved:result.removed<<path;break;
   case Retired::Left:break;
   // Kept in the record, so the next pass tries again (#2132 S6).
   case Retired::Failed:
    next.files<<path;if(previous.hashes.contains(path))next.hashes.insert(path,previous.hashes.value(path));
    logging::warning(logApp(),"Couldn't retire a file manager entry; it stays recorded for the next start",{{"path",path}});
    fail(ioError("Cannot remove a file manager entry",path));break;
   }
  }
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
 // Only the AppImage announces its entries: a DEB or RPM installs them with the
 // package, so there is nothing to tell the user at its first start.
 if(result.firstRun&&!system) {
  // The session's own file manager only: PCManFM on PATH says nothing on LXQt.
  const auto desktops=(c.desktop.isEmpty()?qEnvironmentVariable("XDG_CURRENT_DESKTOP"):c.desktop).split(':');
  const auto onDesktop=[&](const QStringList& names){return std::any_of(desktops.begin(),desktops.end(),[&](const QString& desktop){return names.contains(desktop,Qt::CaseInsensitive);});};
  // docs/design/file-manager-upload.md's unsupported list: no entry there, so no notice claiming one.
  const auto plasma5=onDesktop({"KDE"})&&(c.kdeVersion.isEmpty()?qEnvironmentVariable("KDE_SESSION_VERSION"):c.kdeVersion)=="5";
  result.announce=!onDesktop({"Deepin","DDE","Enlightenment","TDE","Trinity","UKUI"})&&!plasma5&&(cosmicFiles||!onDesktop({"COSMIC"}));
  // libfm and libfm-qt load FileManager-Actions at a process's first file menu. One whose
  // actions folder didn't exist then doesn't see a new entry (LXDE and LXQt guests, #1105).
  const QString actionsFile="file-manager/actions/putgd-upload.desktop";
  const auto actions=next.files.contains(QDir(c.dataHome).filePath(actionsFile));
  const struct {const char* name;QStringList desktops;bool installed;} managers[]={
   {"pcmanfm",{"LXDE"},actions},
   {"pcmanfm-qt",{"LXQt"},actions}};
  // At most one: the notice and its button name the session's file manager. The list
  // runs from the most specific desktop, so the first one with a match decides.
  for(const auto& desktop:desktops) {
   // Budgie:GNOME: Budgie usually runs Nemo, so GNOME's entry says nothing about its file manager.
   if(desktop.compare("Budgie",Qt::CaseInsensitive)==0)break;
   for(const auto& manager:managers)if(manager.installed&&manager.desktops.contains(desktop,Qt::CaseInsensitive)&&onPath(c,manager.name))result.restartable<<manager.name;
   if(!result.restartable.isEmpty())break;
  }
 }
 return result;
}

Error FileManagerIntegration::monitor(const FileManagerContext& c,bool enabled) {
#ifdef Q_OS_LINUX
 if(c.installation!="appimage")return {};
 const auto path=launcherPath(c);
 if(!QFileInfo::exists(path))return enabled?Error{"file_manager_monitor","The file manager launcher is missing",{}}:Error{};
 if(!managedFile(path))return {"file_manager_conflict","The file manager launcher is not managed by put.gd",{}};
 // What the user can do about it; the details are for the log and diagnostics.
 const auto failed=[](const QJsonObject& details){return Error{"file_manager_monitor",QStringLiteral("Couldn't watch for this AppImage being deleted, so its file manager menu entries may stay behind if you remove it. Selecting one later cleans them up."),details};};
 QProcess process;process.start("/bin/sh",{path,enabled?"--install-watch":"--stop-watch"});
 const bool started=process.waitForStarted(3000);
 if(!started||!process.waitForFinished(10000)) {
  const auto reason=process.errorString();process.kill();process.waitForFinished();
  return failed({{"stage",started?"timeout":"start"},{"enabled",enabled},{"reason",reason}});
 }
 const auto reason=QString::fromUtf8(process.readAllStandardError().left(1024)).trimmed();
 if(process.exitStatus()!=QProcess::NormalExit)return failed({{"stage","crash"},{"enabled",enabled},{"reason",reason}});
 if(process.exitCode()==3&&enabled) {
  logging::info(logApp(),"No file manager removal watch: no user service manager",{{"reason",reason}});
  return {};
 }
 if(process.exitCode()!=0)return failed({{"stage","exit"},{"enabled",enabled},{"exitCode",process.exitCode()},{"reason",reason}});
#else
 Q_UNUSED(c);Q_UNUSED(enabled);
#endif
 return {};
}

bool FileManagerIntegration::announceOnce(const FileManagerContext& c,const Error& error) {
 const auto path=QDir(ownDirectory(c)).filePath(c.installation+".problem");
 if(!error) {QFile::remove(path);return false;}
 const auto text=(error.code+'\n'+error.message+'\n').toUtf8();
 QFile previous(path);
 if(previous.open(QIODevice::ReadOnly)&&previous.read(64*1024)==text)return false;
 previous.close();
 if(!QDir().mkpath(ownDirectory(c)))return true;
 QSaveFile file(path);file.setDirectWriteFallback(false);
 // An unrecorded failure is announced again next time rather than never.
 if(file.open(QIODevice::WriteOnly)&&file.write(text)==text.size())file.commit();
 return true;
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
 // Only files still exactly as put.gd wrote them go; anything changed stays where it is (#2132 S2).
 for(const auto& path:record.files)if(retire(c,path,own,nullptr,launcherPath(c),record.files,record.hashes.value(path))==Retired::Failed&&!error)error=ioError("Cannot remove a file manager entry",path);
 if(!QFile::remove(ownedPath)&&QFileInfo::exists(ownedPath)&&!error)error=ioError("Cannot remove the file manager record",ownedPath);
 return error;
}
}
