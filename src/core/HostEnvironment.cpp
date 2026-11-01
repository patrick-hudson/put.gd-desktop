#include "core/HostEnvironment.h"
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace putgd::host {
namespace {
QMutex keptLock;
Kept& keptValues(){static Kept values;return values;}
bool inside(const QString& entry,const QStringList& roots){
 for(const auto& root:roots)if(entry==root||entry.startsWith(root+'/'))return true;
 return false;
}
QStringList mountRoots(const QString& appdir){
 if(!QDir::isAbsolutePath(appdir))return {};
 QStringList roots{QDir::cleanPath(appdir)};
 const auto canonical=QFileInfo(appdir).canonicalFilePath();
 if(!canonical.isEmpty()&&!roots.contains(canonical))roots.append(canonical);
 return roots;
}
}
bool insideAppDir(const QProcessEnvironment& current,const QString& executable){
 const auto root=QFileInfo(current.value("APPDIR")).canonicalFilePath();
 if(root.isEmpty()||!QDir::isAbsolutePath(current.value("APPDIR"))||executable.isEmpty())return false;
 return QFileInfo(executable).canonicalFilePath().startsWith(root+'/');
}
bool fromAppImage(const QProcessEnvironment& current,const QString& executable){
 if(current.contains(savedNamesVariable))return true;
 return insideAppDir(current,executable);
}
QString installationClass(const QProcessEnvironment& current,const QString& executable){
 return insideAppDir(current,executable)?QStringLiteral("appimage"):QStringLiteral("system");
}
QString installationClass(){
 return installationClass(QProcessEnvironment::systemEnvironment(),QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath());
}
QProcessEnvironment environment(const Inputs& in){
 auto result=in.current;
 for(auto it=in.kept.cbegin();it!=in.kept.cend();++it){if(it.value())result.insert(it.key(),*it.value());else result.remove(it.key());}
 if(!fromAppImage(in.current,in.executable))return result;
 // What AppRun replaced, as the host gave it to this launch; unset if it was.
 static const QRegularExpression name("^[A-Z_][A-Z0-9_]*$");
 for(const auto& saved:in.current.value(savedNamesVariable).split(' ',Qt::SkipEmptyParts)){
  if(!name.match(saved).hasMatch()||saved.startsWith(savedPrefix))continue;
  const auto key=savedPrefix+saved;
  if(in.current.contains(key))result.insert(saved,in.current.value(key));else result.remove(saved);
 }
 for(const auto& key:result.keys())if(key==savedNamesVariable||key.startsWith(savedPrefix))result.remove(key);
 // Anything else that still names the mount (a path an older AppRun or a
 // helper set, GStreamer's or Python's): its entries there go, and a variable
 // with nothing left goes too. LD_PRELOAD also separates with spaces.
 const auto roots=mountRoots(in.current.value("APPDIR"));
 static const QRegularExpression preloadSeparator("[: ]");
 if(!roots.isEmpty())for(const auto& key:result.keys()){
  const auto value=result.value(key);
  const auto entries=key=="LD_PRELOAD"?value.split(preloadSeparator):value.split(':');
  QStringList kept;bool named=false;
  for(const auto& entry:entries){if(inside(QDir::cleanPath(entry),roots)&&!entry.isEmpty())named=true;else kept.append(entry);}
  if(!named)continue;
  if(kept.join(QString()).trimmed().isEmpty())result.remove(key);else result.insert(key,kept.join(key=="LD_PRELOAD"?' ':':'));
 }
 // The AppImage runtime's own variables: a host program is not this AppImage.
 for(const char* key:{"APPDIR","APPIMAGE","ARGV0","OWD"})result.remove(key);
 return result;
}
QProcessEnvironment environment(){
 Kept kept;{QMutexLocker lock(&keptLock);kept=keptValues();}
 return environment({QProcessEnvironment::systemEnvironment(),kept,QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath()});
}
void keep(const char* name){
 QMutexLocker lock(&keptLock);
 const auto key=QString::fromLatin1(name);
 if(keptValues().contains(key))return;
 keptValues().insert(key,qEnvironmentVariableIsSet(name)?std::optional<QString>(qEnvironmentVariable(name)):std::nullopt);
}
QString findProgram(const QString& program,const QProcessEnvironment& environment){
 if(QDir::isAbsolutePath(program))return QFileInfo(program).isExecutable()?program:QString();
 const auto path=environment.value("PATH").split(':',Qt::SkipEmptyParts);
 return path.isEmpty()?QStandardPaths::findExecutable(program):QStandardPaths::findExecutable(program,path);
}
bool startDetached(const QString& program,const QStringList& arguments,qint64* pid){
 const auto env=environment();
 const auto path=findProgram(program,env);
 if(path.isEmpty())return false;
 QProcess process;process.setProgram(path);process.setArguments(arguments);process.setProcessEnvironment(env);
 process.setStandardInputFile(QProcess::nullDevice());process.setStandardOutputFile(QProcess::nullDevice());process.setStandardErrorFile(QProcess::nullDevice());
 return process.startDetached(pid);
}
}
