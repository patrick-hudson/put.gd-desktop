#pragma once
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QDBusArgument>
#include <QDBusServiceWatcher>
namespace putgd {
// KGlobalAccel v2 wire type: struct containing an array of Qt combined keys.
struct KdeKeySequence {QList<int> keys;};
QDBusArgument& operator<<(QDBusArgument&,const KdeKeySequence&);
const QDBusArgument& operator>>(const QDBusArgument&,KdeKeySequence&);
class KdeShortcuts final:public QObject {
 Q_OBJECT
public:
 explicit KdeShortcuts(QObject* parent=nullptr);
 ~KdeShortcuts() override;
 void bind(const QHash<QString,QString>&);
 bool available()const{return available_;}QJsonObject status()const{return status_;}
signals:
 void activated(const QString&);void changed();
private slots:
 void pressed(const QString&,const QString&,qlonglong);
 void shortcutsChanged(const QStringList&,const QList<putgd::KdeKeySequence>&);
private:
 void connectComponent(quint64);void bindOne(QString,QString,quint64);
 QHash<QString,QString> requested_;QJsonObject status_;bool available_=false;quint64 generation_=0;
 QString componentPath_;QDBusServiceWatcher watcher_;
};
}
Q_DECLARE_METATYPE(putgd::KdeKeySequence)
Q_DECLARE_METATYPE(QList<putgd::KdeKeySequence>)
