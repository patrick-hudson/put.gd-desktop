#include "PanelTone.h"
#include "Theme.h"
#include "core/HostEnvironment.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <optional>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#endif

namespace putgd {
namespace {
// One key of one [group] in a KDE configuration file.
QString iniValue(const QString& text,const QString& group,const QString& key){
 bool inside=false;
 for(const auto& raw:text.split('\n')){
  const auto line=raw.trimmed();
  if(line.startsWith('[')){inside=line=="["+group+"]";continue;}
  if(inside&&line.startsWith(key+"="))return line.mid(key.size()+1).trimmed();
 }
 return {};
}
// "r,g,b" as KDE colour schemes write it: light when white text would be the
// weaker choice on it.
std::optional<bool> lightColour(const QString& value){
 const auto parts=value.split(',');if(parts.size()<3)return {};
 int rgb[3];for(int i=0;i<3;++i){bool ok=false;rgb[i]=parts[i].trimmed().toInt(&ok);if(!ok||rgb[i]<0||rgb[i]>255)return {};}
 const QColor colour(rgb[0],rgb[1],rgb[2]);
 return Theme::contrast(colour,Qt::black)>Theme::contrast(colour,Qt::white);
}
QString readSmall(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)&&file.size()<(1<<20)?QString::fromUtf8(file.readAll()):QString();}
// KDE reads ~/.config, then ~/.config/kdedefaults (written by the global theme),
// then XDG_CONFIG_DIRS, and the first file that sets a key wins.
QStringList cascade(const QString& name){
 const auto user=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
 QStringList files{user+"/"+name,user+"/kdedefaults/"+name};
 for(const auto& path:QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation,name))if(!files.contains(path))files.append(path);
 return files;
}
// The first file in the cascade whose [group] sets key, whole, so decide() reads it.
QString firstSetting(const QString& name,const QString& group,const QString& key){
 for(const auto& path:cascade(name)){const auto text=readSmall(path);if(!iniValue(text,group,key).isEmpty())return text;}
 return {};
}
QStringList desktops(){return qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower().split(':',Qt::SkipEmptyParts);}
}
std::pair<bool,QString> PanelTone::decide(const Inputs& in){
 const auto names=in.desktop.toLower().split(':',Qt::SkipEmptyParts);
 if(names.contains("kde")){
  const auto style=iniValue(in.plasmarc,"Theme","name").toLower();
  // A style with its own colours draws panels in its window colour.
  if(const auto tone=lightColour(iniValue(in.styleColors,"Colors:Window","BackgroundNormal")))return {*tone,"plasma-style"};
  if(style.contains("dark"))return {false,"plasmarc"};
  if(style.contains("light"))return {true,"plasmarc"};
  // The default Plasma style draws panels in the colour scheme's header
  // colours (window colours in schemes without a header group).
  for(const auto* group:{"Colors:Header","Colors:Window"})
   if(const auto tone=lightColour(iniValue(in.kdeglobals,QString::fromLatin1(group),"BackgroundNormal")))return {*tone,"kdeglobals"};
  // Plasma 6's own default, Breeze, has a light panel.
  return {true,"kde-default"};
 }
 if(names.contains("x-cinnamon")||names.contains("cinnamon")){
  const auto theme=in.cinnamonTheme.toLower();
  return theme.isEmpty()?std::pair<bool,QString>{false,"default"}:std::pair<bool,QString>{!theme.contains("dark"),"cinnamon-theme"};
 }
 if(names.contains("gnome")||names.contains("ubuntu")||names.contains("unity")||names.contains("pantheon"))return {false,"gnome-shell"};
 return {false,"default"};
}
PanelTone::PanelTone(QObject* parent):QObject(parent){
 debounce_.setSingleShot(true);debounce_.setInterval(300);
 connect(&debounce_,&QTimer::timeout,this,&PanelTone::refresh);
 if(desktops().contains("kde")){
  // Plasma rewrites both files atomically; watching the folder sees the swap.
  const auto config=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  watcher_.addPath(config);
  connect(&watcher_,&QFileSystemWatcher::directoryChanged,&debounce_,qOverload<>(&QTimer::start));
 }
#ifdef Q_OS_LINUX
 // A colour-scheme switch through the desktop's settings reaches the portal;
 // re-read the panel inputs then too.
 QDBusConnection::sessionBus().connect("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop","org.freedesktop.portal.Settings","SettingChanged",&debounce_,SLOT(start()));
#endif
 refresh();
}
void PanelTone::refresh(){
 Inputs in;in.desktop=qEnvironmentVariable("XDG_CURRENT_DESKTOP");
 const auto names=desktops();
 if(names.contains("kde")){
  in.plasmarc=firstSetting("plasmarc","Theme","name");
  in.kdeglobals=firstSetting("kdeglobals","Colors:Header","BackgroundNormal");
  if(in.kdeglobals.isEmpty())in.kdeglobals=firstSetting("kdeglobals","Colors:Window","BackgroundNormal");
  in.styleColors=readSmall(QStandardPaths::locate(QStandardPaths::GenericDataLocation,"plasma/desktoptheme/"+plasmaStyle()+"/colors"));
 }
 if(!names.contains("x-cinnamon")&&!names.contains("cinnamon")){settle(in);return;}
 auto* process=new QProcess(this);process->setProcessChannelMode(QProcess::SeparateChannels);process->setStandardErrorFile(QProcess::nullDevice());
 connect(process,&QProcess::finished,this,[this,process,in]()mutable{
  in.cinnamonTheme=QString::fromUtf8(process->readAllStandardOutput().left(512)).trimmed().remove('\'');process->deleteLater();settle(in);
 });
 connect(process,&QProcess::errorOccurred,this,[this,process,in](QProcess::ProcessError error){if(error==QProcess::FailedToStart){process->deleteLater();settle(in);}});
 QTimer::singleShot(3000,process,[process]{if(process->state()!=QProcess::NotRunning)process->kill();});
 // The host's gsettings, with the host's GLib (#326).
 process->setProcessEnvironment(host::environment());
 process->start("gsettings",{"get","org.cinnamon.theme","name"});
}
QString PanelTone::plasmaStyle(){
 const auto name=iniValue(firstSetting("plasmarc","Theme","name"),"Theme","name");
 return name.isEmpty()||name.contains('/')||name.contains("..")?QStringLiteral("default"):name;
}
void PanelTone::settle(const Inputs& in){
 const auto [light,source]=decide(in);
 if(light==light_&&source==source_)return;
 light_=light;source_=source;emit changed();
}
}
