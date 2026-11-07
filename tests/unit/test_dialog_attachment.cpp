#include "ui/DialogAttachment.h"
#include "core/HostEnvironment.h"
#include <QtTest>
using namespace putgd;
class DialogAttachmentContract : public QObject {
 Q_OBJECT
private slots:
 void onlyGnomeAttachesModalDialogs() {
  QVERIFY(attachesModalDialogs("GNOME"));
  QVERIFY(attachesModalDialogs("ubuntu:GNOME"));
  QVERIFY(attachesModalDialogs("GNOME-Classic:GNOME"));
  QVERIFY(attachesModalDialogs("gnome"));
  QVERIFY(!attachesModalDialogs("KDE"));
  QVERIFY(!attachesModalDialogs("X-Cinnamon"));
  QVERIFY(!attachesModalDialogs("GNOME-Flashback"));
  QVERIFY(!attachesModalDialogs(""));
 }
 void theDialogProtocolJoinsTheUsersList() {
  QCOMPARE(withDialogProtocolDisabled(""), QByteArray("xdg_wm_dialog_v1"));
  QCOMPARE(withDialogProtocolDisabled("wp_fractional_scale_manager_v1"), QByteArray("wp_fractional_scale_manager_v1,xdg_wm_dialog_v1"));
  QCOMPARE(withDialogProtocolDisabled(" xdg_wm_dialog_v1 ,"), QByteArray("xdg_wm_dialog_v1"));
  QCOMPARE(withDialogProtocolDisabled("a,,b"), QByteArray("a,b,xdg_wm_dialog_v1"));
 }
 // The process gets the list; programs it starts get the user's value back.
 void hostProgramsKeepTheUsersValue() {
  qputenv("XDG_CURRENT_DESKTOP", "ubuntu:GNOME");
  qputenv("QT_WAYLAND_DISABLED_INTERFACES", "wp_viewporter");
  keepModalDialogsDetached();
  QCOMPARE(qgetenv("QT_WAYLAND_DISABLED_INTERFACES"), QByteArray("wp_viewporter,xdg_wm_dialog_v1"));
  QCOMPARE(host::environment().value("QT_WAYLAND_DISABLED_INTERFACES"), QStringLiteral("wp_viewporter"));
 }
 void otherDesktopsAreLeftAlone() {
  qputenv("XDG_CURRENT_DESKTOP", "KDE");
  qputenv("QT_WAYLAND_DISABLED_INTERFACES", "wp_viewporter");
  keepModalDialogsDetached();
  QCOMPARE(qgetenv("QT_WAYLAND_DISABLED_INTERFACES"), QByteArray("wp_viewporter"));
 }
};
QTEST_GUILESS_MAIN(DialogAttachmentContract)
#include "test_dialog_attachment.moc"
