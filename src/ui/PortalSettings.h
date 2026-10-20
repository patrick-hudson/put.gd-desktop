#pragma once
#include <QDBusVariant>
#include <QHash>
#include <QObject>
#include <QVariant>

namespace putgd {
// The XDG desktop portal's Settings for the namespaces the appearance needs
// (#60): one bounded ReadAll when created, then its SettingChanged signals.
// Linux only; an absent portal leaves every value empty.
class PortalSettings final:public QObject {
 Q_OBJECT
public:
 explicit PortalSettings(QObject* parent=nullptr);
 QVariant value(const QString& group,const QString& key)const{return values_.value(group+'\n'+key);}
signals:
 void changed();
private slots:
 void settingChanged(const QString& group,const QString& key,const QDBusVariant& value);
private:
 QHash<QString,QVariant> values_;
};
}
