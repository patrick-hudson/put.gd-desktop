#pragma once
#include "core/Error.h"
#include "platform/wayland/ShortcutBackend.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>

namespace putgd {
// Global shortcuts on Budgie. The resident owns org.putgd.App.Budgie on the
// session bus; a bound key runs
//   dbus-send --session --type=method_call --dest=org.putgd.App.Budgie
//     /org/putgd/App org.putgd.App.Budgie.Trigger string:<action>
// which needs no binary path (an AppImage's moves) and starts no put.gd
// process per press. With no resident running the call fails harmlessly.
//
// The backend is chosen when shortcuts are bound: Budgie 10.10's labwc bridge
// when its schema answers and rc.xml exists, else the GlobalShortcuts portal
// when the desktop offers it, else none, with the reason in each binding.
class BudgieShortcuts final:public QObject {
 Q_OBJECT
public:
 explicit BudgieShortcuts(QObject* parent=nullptr);
 ~BudgieShortcuts() override;
 void bind(const QHash<QString,QString>&);
 void setRecording(bool,std::function<void(Error)>);
 // {"available","backend","service","error"?,"bindings":{...}}
 QJsonObject capabilities()const;
 // Called by the D-Bus adaptor. Returns false for an action that is not bound.
 bool trigger(const QString& action);
 // Removes what the backend wrote; the resident calls it as it quits.
 void clear();
signals:
 void activated(const QString&);void changed();
private:
 void choose();
 void use(ShortcutBackend*);
 QHash<QString,QString> requested_;
 QPointer<ShortcutBackend> backend_;
 bool registered_=false,choosing_=false,chosen_=false,recording_=false;
 QString serviceError_,backendError_;
 QObject* adaptor_=nullptr;
};
}
