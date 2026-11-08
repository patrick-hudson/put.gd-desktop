#pragma once
#include "core/Error.h"
#include <QObject>
#include <QString>
#include <functional>

namespace putgd {
// hyprland_toplevel_mapping_manager_v1: which Hyprland window (its IPC
// "address") an ext-foreign-toplevel-list handle is. The window shortcut
// freezes the target through IPC (stableId, address); this confirms on the
// compositor's own protocol that the identifier capture will read is that same
// window before any pixel is taken.
class ToplevelMapping final:public QObject {
 Q_OBJECT
public:
 using Reply=std::function<void(QString address,Error)>;
 explicit ToplevelMapping(QObject* parent=nullptr);
 ~ToplevelMapping() override;
 // Address for the ext identifier, as "0x..." lower case; capture_target when
 // the identifier is not listed or the compositor cannot map it. Runs on a
 // worker thread with its own short-lived connection; replies on this thread.
 void addressFor(const QString& identifier,Reply);
 // The blocking body, for tests and the worker.
 static QString resolve(const QString& identifier,Error& error,int timeoutMs=2000);
 // "0x55d0c0ffee10" and "55d0c0ffee10" compare equal.
 static bool sameAddress(const QString&,const QString&);
private:
 QList<class QThread*> workers_;
};
}
