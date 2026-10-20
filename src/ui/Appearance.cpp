#include "Appearance.h"
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QStyleHints>
#ifdef Q_OS_LINUX
#include "PortalSettings.h"
#endif

namespace putgd {
namespace {
QString iniValue(const QString& text,const QString& group,const QString& key){
 bool inside=false;
 for(const auto& raw:text.split('\n')){
  const auto line=raw.trimmed();
  if(line.startsWith('[')){inside=line=="["+group+"]";continue;}
  if(inside&&line.startsWith(key+"="))return line.mid(key.size()+1).trimmed();
 }
 return {};
}
// Light when black text has more contrast on it than white.
bool lightColour(const QColor& colour){return Theme::contrast(colour,Qt::black)>Theme::contrast(colour,Qt::white);}
int qtScheme(){
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
 // A Cinnamon, GNOME or KDE core resident has no GUI application and no hints.
 if(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()))return 0;
 const auto scheme=QGuiApplication::styleHints()->colorScheme();
 return scheme==Qt::ColorScheme::Light?1:scheme==Qt::ColorScheme::Dark?2:0;
#else
 return 0;
#endif
}
}
std::pair<QString,QString> Appearance::decide(const Inputs& in){
 if(in.qt==1)return {"light","qt"};
 if(in.qt==2)return {"dark","qt"};
 if(in.portal==1)return {"dark","portal"};
 if(in.portal==2)return {"light","portal"};
 if(in.gnomeScheme=="prefer-dark")return {"dark","portal-gnome"};
 if(in.gnomeScheme=="prefer-light")return {"light","portal-gnome"};
 // GNOME's "default" and Cinnamon's portal leave it to the GTK theme's name.
 if(!in.gtkTheme.isEmpty())return {in.gtkTheme.contains("dark",Qt::CaseInsensitive)?"dark":"light","portal-gtk-theme"};
 if(in.kde){
  const auto parts=iniValue(in.kdeglobals,"Colors:Window","BackgroundNormal").split(',');
  if(parts.size()>=3){bool r=false,g=false,b=false;const QColor window(parts[0].toInt(&r),parts[1].toInt(&g),parts[2].toInt(&b));if(r&&g&&b&&window.isValid())return {lightColour(window)?"light":"dark","kdeglobals"};}
 }
 return {{},"unknown"};
}
Appearance::Appearance(QObject* parent,Role role):QObject(parent),role_(role){
 inputs_.kde=qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':').contains("KDE",Qt::CaseInsensitive);
 debounce_.setSingleShot(true);debounce_.setInterval(200);
 connect(&debounce_,&QTimer::timeout,this,[this]{readKde();evaluate();});
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
 if(qobject_cast<QGuiApplication*>(QCoreApplication::instance()))connect(QGuiApplication::styleHints(),&QStyleHints::colorSchemeChanged,this,[this]{inputs_.qt=qtScheme();evaluate();});
#endif
#ifdef Q_OS_LINUX
 portal_=new PortalSettings(this);
 connect(portal_,&PortalSettings::changed,this,[this]{readPortal();evaluate();});
#endif
 if(inputs_.kde){
  watcher_.addPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
  connect(&watcher_,&QFileSystemWatcher::directoryChanged,&debounce_,qOverload<>(&QTimer::start));
 }
 inputs_.qt=qtScheme();readPortal();readKde();evaluate();
}
void Appearance::readPortal(){
#ifdef Q_OS_LINUX
 inputs_.portal=int(portal_->value("org.freedesktop.appearance","color-scheme").toUInt());
 inputs_.gnomeScheme=portal_->value("org.gnome.desktop.interface","color-scheme").toString();
 inputs_.gtkTheme=portal_->value("org.gnome.desktop.interface","gtk-theme").toString();
#endif
}
void Appearance::readKde(){
 if(!inputs_.kde)return;
 // The first kdeglobals in KDE's cascade that sets the window colour.
 const auto user=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
 QStringList files{user+"/kdeglobals",user+"/kdedefaults/kdeglobals"};files+=QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation,"kdeglobals");
 inputs_.kdeglobals.clear();
 for(const auto& path:files){QFile file(path);const auto text=file.open(QIODevice::ReadOnly)&&file.size()<(1<<20)?QString::fromUtf8(file.readAll()):QString();if(!iniValue(text,"Colors:Window","BackgroundNormal").isEmpty()){inputs_.kdeglobals=text;break;}}
}
void Appearance::setPreference(const QString& preference){
 const auto value=preference=="dark"||preference=="light"?preference:QStringLiteral("system");
 if(value==preference_)return;
 preference_=value;evaluate();
}
Theme::Scheme Appearance::scheme()const{
 const auto& chosen=preference_=="system"?desktop_:preference_;
 // Nothing says: put.gd's original dark look.
 return chosen=="light"?Theme::Scheme::Light:Theme::Scheme::Dark;
}
QJsonObject Appearance::state()const{
 const auto active=scheme();
 return {{"theme",preference_},{"desktop",desktop_.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(desktop_)},{"desktopSource",source_},{"scheme",Theme::name(active)},{"tokens",Theme::tokens(active)}};
}
void Appearance::evaluate(){
 const auto [desktop,source]=decide(inputs_);
 const bool desktopChanged=desktop!=desktop_||source!=source_;
 desktop_=desktop;source_=source;
 const auto next=scheme();
 const bool resolvedChanged=std::exchange(resolved_,next)!=next;
 const bool schemeChanged=role_==Role::Interface?Theme::setScheme(next):resolvedChanged;
 if(desktopChanged||schemeChanged)emit changed();
}
}
