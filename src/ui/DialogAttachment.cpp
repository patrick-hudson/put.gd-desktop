#include "DialogAttachment.h"
#include "core/HostEnvironment.h"
#include <QList>
namespace putgd {
namespace {
constexpr char disabledInterfacesVariable[] = "QT_WAYLAND_DISABLED_INTERFACES";
constexpr char dialogProtocol[] = "xdg_wm_dialog_v1";
}
bool attachesModalDialogs(const QByteArray& currentDesktop) {
 for (const auto& entry : currentDesktop.split(':')) if (entry.trimmed().toUpper() == "GNOME") return true;
 return false;
}
QByteArray withDialogProtocolDisabled(const QByteArray& disabledInterfaces) {
 QList<QByteArray> names;
 for (const auto& name : disabledInterfaces.split(',')) if (!name.trimmed().isEmpty()) names.append(name.trimmed());
 if (!names.contains(dialogProtocol)) names.append(dialogProtocol);
 return names.join(',');
}
void keepModalDialogsDetached() {
 if (!attachesModalDialogs(qgetenv("XDG_CURRENT_DESKTOP"))) return;
 // Qt Wayland reads the list once, when the display first announces its globals.
 host::keep(disabledInterfacesVariable);
 qputenv(disabledInterfacesVariable, withDialogProtocolDisabled(qgetenv(disabledInterfacesVariable)));
}
}
