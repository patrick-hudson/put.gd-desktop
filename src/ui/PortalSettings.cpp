#include "PortalSettings.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>

namespace putgd {
namespace {
const QStringList groups{"org.freedesktop.appearance","org.gnome.desktop.interface"};
QVariant plain(QVariant value){while(value.canConvert<QDBusVariant>()&&value.metaType()==QMetaType::fromType<QDBusVariant>())value=value.value<QDBusVariant>().variant();return value;}
}
PortalSettings::PortalSettings(QObject* parent):QObject(parent){
 auto bus=QDBusConnection::sessionBus();if(!bus.isConnected())return;
 bus.connect("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop","org.freedesktop.portal.Settings","SettingChanged",this,SLOT(settingChanged(QString,QString,QDBusVariant)));
 // Bounded: the first window waits for this, so its colours are right at once.
 auto call=QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop","org.freedesktop.portal.Settings","ReadAll");
 call<<groups;const auto reply=bus.call(call,QDBus::Block,300);
 if(reply.type()!=QDBusMessage::ReplyMessage||reply.arguments().isEmpty())return;
 const auto argument=reply.arguments().first().value<QDBusArgument>();
 argument.beginMap();
 while(!argument.atEnd()){
  QString group;QVariantMap entries;argument.beginMapEntry();argument>>group>>entries;argument.endMapEntry();
  if(!groups.contains(group))continue;
  for(auto it=entries.cbegin();it!=entries.cend();++it)values_.insert(group+'\n'+it.key(),plain(it.value()));
 }
 argument.endMap();
}
void PortalSettings::settingChanged(const QString& group,const QString& key,const QDBusVariant& value){
 if(!groups.contains(group))return;
 values_.insert(group+'\n'+key,plain(value.variant()));emit changed();
}
}
