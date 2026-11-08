#pragma once
#include "core/Error.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <functional>

namespace putgd {
// One way to hold put.gd's global shortcuts on a Wayland desktop whose
// compositor offers no shortcut protocol of its own: a desktop's settings
// bridge, the GlobalShortcuts portal, a compositor configuration file. Desktop
// adapters pick one at bind time and report its per-binding status, in the
// shape the Settings window and agents read:
//   {"requested", "effective"?, "actual": [...], "bound", "state", "error"?}
// ("effective" only for a bound action)
// with state one of pending, bound, unbound, shortcut_conflict,
// shortcut_invalid, shortcut_unavailable or recording.
class ShortcutBackend:public QObject {
 Q_OBJECT
public:
 using QObject::QObject;
 virtual QString name() const=0;
 // Whether the backend can hold shortcuts at all; status explains why not.
 virtual bool available() const=0;
 // action -> Qt portable text ("Ctrl+Shift+1"); an empty value unbinds.
 virtual void bind(const QHash<QString,QString>& desired)=0;
 virtual QJsonObject status() const=0;
 // While the shortcut recorder listens, the keys must reach its window.
 virtual void setPaused(bool paused,std::function<void(Error)> reply)=0;
 // Removes every binding this backend wrote, for a clean quit.
 virtual void clear() {}
signals:
 void changed();
 // A backend that receives the key press itself (the portal) reports it here;
 // a bridge whose command calls back over D-Bus never emits it.
 void activated(const QString& action);
};
}
