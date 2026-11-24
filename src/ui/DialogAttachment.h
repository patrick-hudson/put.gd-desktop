#pragma once
#include <QByteArray>
namespace putgd {
// GNOME attaches a modal dialog to its parent window: GNOME's session turns on
// org.gnome.mutter attach-modal-dialogs, and Mutter then moves the parent
// along whenever the dialog is dragged. Since Qt 6.8 Qt marks every modal
// window as modal through the xdg-dialog-v1 Wayland protocol, so dragging the
// editor's custom color dialog, a confirmation or a file dialog dragged the
// whole put.gd window with it. Qt blocks a modal dialog's parent by itself, and
// a dialog stays a transient of its parent without the protocol, so put.gd
// windows on GNOME leave it unbound and their dialogs move on their own.

// Whether the desktop attaches modal dialogs: a GNOME entry in
// XDG_CURRENT_DESKTOP (GNOME, ubuntu:GNOME, GNOME-Classic:GNOME).
bool attachesModalDialogs(const QByteArray& currentDesktop);
// QT_WAYLAND_DISABLED_INTERFACES with the dialog protocol added, keeping
// whatever the user already disabled.
QByteArray withDialogProtocolDisabled(const QByteArray& disabledInterfaces);
// Applies the above to this process before its QApplication exists. Host
// programs it starts get the original value back (host::environment()).
void keepModalDialogsDetached();
}
