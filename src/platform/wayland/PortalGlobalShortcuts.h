#pragma once
#include "ShortcutBackend.h"
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QHash>
#include <QJsonObject>
#include <QVariantMap>

namespace putgd {
// org.freedesktop.portal.GlobalShortcuts, for any desktop whose portal offers
// it. The desktop asks the user to confirm and may assign another trigger, so
// the reported "actual" key is the portal's trigger_description. A rebind or a
// pause closes the session and opens a new one: the portal binds once per session.
class PortalGlobalShortcuts final:public ShortcutBackend {
 Q_OBJECT
public:
 explicit PortalGlobalShortcuts(QObject* parent=nullptr);
 ~PortalGlobalShortcuts() override;
 QString name()const override{return QStringLiteral("portal-global-shortcuts");}
 bool available()const override{return available_;}
 void bind(const QHash<QString,QString>&) override;
 QJsonObject status()const override;
 void setPaused(bool,std::function<void(Error)>) override;
 void clear() override{close();}
 static void probe(std::function<void(bool,QString)> reply);
 // Qt portable text to the XDG shortcut format: "Ctrl+Shift+1" -> "CTRL+SHIFT+1".
 static QString xdgTrigger(const QString& portable);
private slots:
 void response(uint,const QVariantMap&);
 void activatedSignal(const QDBusObjectPath&,const QString&,qulonglong,const QVariantMap&);
 void shortcutsChanged(const QDBusObjectPath&,const QDBusArgument&);
private:
 void open();
 void close(std::function<void()> closed={});
 void request(const QString& method,const QList<QVariant>& arguments,const QString& token);
 QHash<QString,QString> requested_;QHash<QString,QString> triggers_;
 QString session_,pendingRequest_,stage_,error_;quint64 generation_=0;
 bool available_=true,paused_=false,bound_=false,registered_=false;
};
}
