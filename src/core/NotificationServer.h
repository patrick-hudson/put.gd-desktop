#pragma once
#include <QString>
#include <QStringList>

// Notification servers, by the name GetServerInformation returns.
namespace putgd::notifications {
// A failure notice is critical: it stays on screen until seen. GNOME Shell,
// Plasma and Cinnamon then keep it in their list until it is dismissed, and a
// GNOME screen lock hides a banner but destroys only a transient one, so a
// refusal History never records (no destination, another installation
// running) must not be transient there. Omarchy's quickshell server also
// restores its list after a restart and showed an old failure again after
// every reboot (#1298); there, and only there, a failure carries the spec's
// "transient" hint.
inline bool failuresTransient(const QString& server) { return server == QLatin1String("quickshell"); }
// CloseNotification takes a notification off the screen on GNOME Shell,
// Plasma, Cinnamon, Budgie and most others. Omarchy's shell (a quickshell
// configuration) answers it with NotificationClosed and leaves a card that is
// already showing where it is ("Visible snapshots outlive their sender",
// plugins/notifications/Service.qml of Omarchy 4.5), a critical one until the
// user dismisses it; and it shows every card for at least five seconds
// whatever expire_timeout asks. put.gd's own notices then stood in the next
// scroll capture's picture (#1376, lab guest of 2026-11-11). The shell's own
// IPC takes cards off by summary, the call omarchy-notification-dismiss
// wraps; `qs ipc --pid` reaches the shell that owns the bus name without
// Omarchy's environment, given the shell's XDG_RUNTIME_DIR (the caller runs
// it with the owner's). This is that command for one summary, empty where
// CloseNotification is enough. A quickshell configuration without the target
// refuses the call, which costs nothing.
inline QStringList dismissCommand(const QString& server, qint64 serverPid, const QString& summary) {
    if (server != QLatin1String("quickshell") || serverPid <= 0 || summary.isEmpty() || summary.startsWith(QLatin1Char('-'))) return {};
    return {QStringLiteral("qs"), QStringLiteral("ipc"), QStringLiteral("--pid"), QString::number(serverPid), QStringLiteral("call"),
            QStringLiteral("notifications"), QStringLiteral("dismiss"), summary};
}
}
