#pragma once
#include <QObject>
#include "core/Error.h"
#include <functional>
#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QDBusArgument>
#include <QDBusServiceWatcher>
#include <xkbcommon/xkbcommon.h>
#include <QTimer>
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
 void bind(const QHash<QString,QString>&,std::function<void(Error)> reply={});
 void setRecording(bool,std::function<void(Error)>);
 void setKeyboardMap(xkb_keymap*);
 bool available()const{return available_;}QJsonObject status()const{return status_;}
signals:
 void activated(const QString&);void changed();
private slots:
 void layoutChanged(uint);
 void layoutsChanged();
 void pressed(const QString&,const QString&,qlonglong);
 void shortcutsChanged(const QStringList&,const QList<putgd::KdeKeySequence>&);
private:
 void refreshLayout();
 void queryLayout();
 void applyLayout(xkb_layout_index_t);
 void connectComponent(quint64);void bindOne(QString,QString,quint64,std::function<void(Error)> complete={});
 bool recording_=false;
 QHash<QString,QString> requested_;QJsonObject status_;bool available_=false,apiReady_=false;quint64 generation_=0;
 QHash<QString,QKeySequence> effective_;
 xkb_keymap* keymap_=nullptr;xkb_layout_index_t layout_=XKB_LAYOUT_INVALID;quint64 layoutGeneration_=0;
 QString componentPath_;QDBusServiceWatcher watcher_,layoutWatcher_;QTimer layoutRetry_;int layoutRetryMs_=3000;
};
}
Q_DECLARE_METATYPE(putgd::KdeKeySequence)
Q_DECLARE_METATYPE(QList<putgd::KdeKeySequence>)
